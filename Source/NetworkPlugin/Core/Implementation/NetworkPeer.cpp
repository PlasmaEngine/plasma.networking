#include "Foundation/System/SystemInformation.h"

#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/Driver/DriverInterface.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/System/SystemInformation.h>
#include <Foundation/Memory/PageAllocator.h>
#include <Foundation/Threading/Mutex.h>

#if BUILDSYSTEM_ENABLE_ENET_SUPPORT
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>
#endif
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/Driver/simulated/DriverSimulated.h>

plDynamicArray<plNetworkPeer*> plNetworkPeer::s_Peers;

namespace
{
  plMutex s_PeersMutex;

  plNetworkDriverInterface* CreateDriver(plDriverTypeEnum::Enum driverType)
  {
    switch (driverType)
    {
#if BUILDSYSTEM_ENABLE_ENET_SUPPORT
      case plDriverTypeEnum::ENet:
        return PL_DEFAULT_NEW(plDriverEnet);
#endif
      case plDriverTypeEnum::Platform:
        return PL_DEFAULT_NEW(plDriverPlatform);
      default:
        plLog::Error("No network driver enabled");
        return nullptr;
    }
  }
} // namespace

plResult plNetworkPeer::Initialize(const plNetworkConfig& config)
{
  if(m_pDriver)
    return PL_FAILURE;

  m_Config = config;
  if (config.m_uiMessageSize == 0 || config.m_uiMessagePoolSize == 0 || config.m_uiConnectionLimit == 0)
    return PL_FAILURE;

  plNetworkDriverInterface* pTransport = CreateDriver(m_Config.m_DriverType);
  if (!pTransport)
  {
    return PL_FAILURE;
  }

  CreateMessageBuffers();
  m_MessagePool.Clear();

  for (plUInt32 messageId = 0; messageId < m_Config.m_uiMessagePoolSize; messageId++)
  {
    m_MessagePool.PushBack(messageId);
  }

  if (pTransport->Initalize(this, m_Config).Failed())
  {
    // The Platform driver needs a backend with a transport; without one the peer runs on ENet instead. A transport that
    // another peer in this process holds is not missing, so that peer fails instead of opening an ENet socket.
    const bool bTransportInUse = m_Config.m_DriverType == plDriverTypeEnum::Platform && static_cast<plDriverPlatform*>(pTransport)->IsTransportInUse();
    if (bTransportInUse)
    {
      plLog::Error("PL Net: Failed to initialize network driver: the platform transport belongs to another peer");
    }
    else if (m_Config.m_DriverType == plDriverTypeEnum::Platform)
    {
      plLog::Warning("PL Net: Platform transport unavailable, falling back to the ENet driver");
      pTransport->Shutdown();
      PL_DEFAULT_DELETE(pTransport);
      m_Config.m_DriverType = plDriverTypeEnum::ENet;
      pTransport = CreateDriver(m_Config.m_DriverType);
    }

    if (bTransportInUse || !pTransport || pTransport->Initalize(this, m_Config).Failed())
    {
      if (!bTransportInUse)
        plLog::Error("PL Net: Failed to initialize network driver");

      if (pTransport)
      {
        pTransport->Shutdown();
        PL_DEFAULT_DELETE(pTransport);
      }
      DestroyMessageBuffers();
      m_MessagePool.Clear();
      return PL_FAILURE;
    }
  }

  // Every transport runs through the simulating driver, which passes messages straight on while no conditions are set.
  m_pDriver = PL_DEFAULT_NEW(plDriverSimulated, pTransport);
  m_pDriver->Initalize(this, m_Config).IgnoreResult();

  plStringBuilder sTmp;
  plLog::Info("PL Net: Network Peer Initialized using driver: {}", m_pDriver->GetDriverName().GetData(sTmp));
  return PL_SUCCESS;
}

void plNetworkPeer::Shutdown()
{
  if (!m_pDriver)
    return;

  // Whatever is still queued, such as the disconnect a stopping peer sends, goes out before the driver closes.
  Flush();

  m_pDriver->Shutdown();
  PL_DEFAULT_DELETE(m_pDriver);

  DestroyMessageBuffers();
  m_OutgoingBuffer.Clear();
  m_MessagePool.Clear();

  for (plUInt8* pBuffer : m_ScratchBuffers)
    plFoundation::GetDefaultAllocator()->Deallocate(pBuffer);
  m_ScratchBuffers.Clear();
  m_uiScratchInUse = 0;

  m_pDriver = nullptr;
}

plResult plNetworkPeer::Listen()
{
  plLog::Info("PL Net: Network Peer Listening on: {0}:{1}", m_Config.m_sServerAddress, m_Config.m_uiServerPort);
  m_Role = plNetworkRoleEnum::Server;
  return m_pDriver->Listen();
}

plResult plNetworkPeer::Connect()
{
  plLog::Info("PL Net: Network Peer Connecting to: {0}:{1}", m_Config.m_sServerAddress, m_Config.m_uiServerPort);
  m_Role = plNetworkRoleEnum::Client;
  return m_pDriver->Connect();
}
void plNetworkPeer::Disconnect(plUInt32 uiReason)
{
  plLog::Info("PL Net: Network Peer Disconnecting");

  // The disconnect message and anything else queued go out before the connection closes.
  Flush();
  m_pDriver->Disconnect(uiReason);
}

void plNetworkPeer::Disconnect(const plNetworkConnection& connection, plUInt32 uiReason)
{
  plLog::Info("PL Net: Network Peer Disconnecting");
  Flush();
  m_pDriver->Disconnect(connection, uiReason);
}

void plNetworkPeer::DisconnectAll(plUInt32 uiReason)
{
  Flush();
  m_pDriver->DisconnectAll(uiReason);
}
plResult plNetworkPeer::PopEvent(plNetworkEvent& event)
{
  PL_PROFILE_SCOPE("plNetworkPeer::PopEvent");
  return m_pDriver->PopEvent(event);
}

plResult plNetworkPeer::Enqueue(const plNetworkMessage& message, bool bToServer, plArrayPtr<const plNetworkConnection> targets)
{
  if (!message.IsValid() || message.HasWriteError() || message.m_uiLength == 0)
  {
    // A message that did not fit its buffer is not sent in part. The caller keeps the state it meant to send.
    const plTime now = plNetworkClock::Now();
    if (now - m_LastSendErrorLog > plTime::MakeFromSeconds(1.0))
    {
      plLog::Error("PL Net: Not sending a message that is invalid or larger than {} bytes", m_Config.m_uiMessageSize);
      m_LastSendErrorLog = now;
    }
    return PL_FAILURE;
  }

  if (!bToServer && targets.IsEmpty())
    return PL_SUCCESS;

  plBufferMessage& buffer = m_OutgoingBuffer.ExpandAndGetRef();
  buffer.m_bToServer = bToServer;
  buffer.m_Targets = targets;
  buffer.m_ChannelType = message.GetChannelType();
  buffer.m_Data.SetCountUninitialized(message.m_uiLength);
  plMemoryUtils::Copy(buffer.m_Data.GetData(), message.m_pBuffer, message.m_uiLength);
  return PL_SUCCESS;
}

plResult plNetworkPeer::SendToServer(const plNetworkMessage& message)
{
  PL_ASSERT_DEV(m_Role != plNetworkRoleEnum::Server, "SendToServer is only valid on a client; a listening peer names its targets");
  if (m_Role == plNetworkRoleEnum::Server)
    return PL_FAILURE;

  return Enqueue(message, true, {});
}

plResult plNetworkPeer::SendTo(const plNetworkMessage& message, const plNetworkConnection& target)
{
  return SendTo(message, plArrayPtr<const plNetworkConnection>(&target, 1));
}

plResult plNetworkPeer::SendTo(const plNetworkMessage& message, plArrayPtr<const plNetworkConnection> targets)
{
  PL_ASSERT_DEV(m_Role == plNetworkRoleEnum::Server, "SendTo is only valid on a listening peer; a client sends to the server");
  if (m_Role != plNetworkRoleEnum::Server)
    return PL_FAILURE;

  return Enqueue(message, false, targets);
}

plNetworkDriverInterface* plNetworkPeer::GetTransportDriver() const
{
  return m_pDriver ? static_cast<plDriverSimulated*>(m_pDriver)->GetTransport() : nullptr;
}

plNetworkSimulator* plNetworkPeer::GetSimulator() const
{
  return m_pDriver ? &static_cast<plDriverSimulated*>(m_pDriver)->GetSimulator() : nullptr;
}

plNetworkPeer* plNetworkPeer::CreatePeer(const plNetworkConfig& config)
{
  if(config.m_sServerAddress.IsEmpty())
  {
    plLog::Error("PL Net: Server Address is empty when creating network peer");
    return nullptr;
  }

  plNetworkPeer* pPeer = PL_DEFAULT_NEW(plNetworkPeer);

  if(pPeer->Initialize(config).Failed())
  {
    PL_DEFAULT_DELETE(pPeer);
    return nullptr;
  }

  PL_LOCK(s_PeersMutex);
  s_Peers.PushBack(pPeer);
  return pPeer;
}

plUInt32 plNetworkPeer::GetPeerCount()
{
  PL_LOCK(s_PeersMutex);
  return s_Peers.GetCount();
}

void plNetworkPeer::DestroyPeer(plNetworkPeer* pPeer)
{
  if (!pPeer)
    return;

  pPeer->Shutdown();
  {
    PL_LOCK(s_PeersMutex);
    s_Peers.RemoveAndSwap(pPeer);
  }
  PL_DEFAULT_DELETE(pPeer);
}

void plNetworkPeer::CreateMessageBuffers()
{
  PL_ASSERT_DEV(m_MessageBuffer == nullptr, "Message buffer already exists");

  const plUInt32 pageSize = plSystemInformation::Get().GetMemoryPageSize();
  const plUInt64 totalSize = static_cast<plUInt64>(m_Config.m_uiMessagePoolSize) * m_Config.m_uiMessageSize;
  const plUInt32 numPages = totalSize > pageSize ? plMath::CeilToInt(totalSize / static_cast<float>(pageSize)) : 1;

  m_MessageBuffer = static_cast<plUInt8*>(plPageAllocator::AllocatePage(numPages * pageSize));
  plMemoryUtils::PatternFill(m_MessageBuffer, 0xCD, numPages * pageSize);
}

void plNetworkPeer::DestroyMessageBuffers()
{
  PL_ASSERT_DEV(m_MessageBuffer != nullptr, "Message buffer does not exist");
  plPageAllocator::DeallocatePage(m_MessageBuffer);
  m_MessageBuffer = nullptr;
}

plNetworkMessage plNetworkPeer::CreateMessage()
{
  if (m_MessagePool.GetCount() == 0)
    return plNetworkMessage(); // The driver leaves the packet queued until a buffer is free again.

  const plUInt32 messageId = m_MessagePool.PeekBack();

  m_MessagePool.PopBack();
  if (!IsValidMessageID(messageId))
    return plNetworkMessage();

  plUInt8* messageBuffer = GetMessageBuffer(messageId);
  return plNetworkMessage(messageBuffer, messageId, m_Config.m_uiMessageSize, 0, 0);
}

void plNetworkPeer::DestroyMessage(plNetworkMessage& message)
{
  if (!message.IsValid())
  {
    plLog::Warning("PL Net: Invalid message");
    return;
  }

  message.m_bPendingRelease = true;
  if (!IsValidMessageID(message.m_uiMessageID))
    return; // a scratch buffer, recycled in Update()

  m_MessagePool.PushBack(message.m_uiMessageID);
  plMemoryUtils::PatternFill(message.m_pBuffer, 0xDD, message.m_uiLength);
}

plNetworkMessage plNetworkPeer::BeginSendMessage()
{
  if (m_uiScratchInUse == m_ScratchBuffers.GetCount())
    m_ScratchBuffers.PushBack(static_cast<plUInt8*>(plFoundation::GetDefaultAllocator()->Allocate(m_Config.m_uiMessageSize, 16)));

  // IDs at and above the pool size mark scratch buffers, which never return to the incoming pool.
  const plUInt32 uiIndex = m_uiScratchInUse++;
  return plNetworkMessage(m_ScratchBuffers[uiIndex], m_Config.m_uiMessagePoolSize + uiIndex, m_Config.m_uiMessageSize, 0, 0);
}

void plNetworkPeer::CancelSendMessage(plNetworkMessage& message)
{
  message.m_bPendingRelease = true;
}

bool plNetworkPeer::IsValidMessageID(plUInt32 messageID) const
{
  return messageID < m_Config.m_uiMessagePoolSize &&
         m_MessageBuffer != nullptr;
}

void plNetworkPeer::Update(float dt)
{
  PL_IGNORE_UNUSED(dt);
  Flush();

  // Every send copied its bytes, so no scratch buffer handed out before this point is still needed.
  m_uiScratchInUse = 0;
}

void plNetworkPeer::Flush()
{
  FlushOutgoingBuffer();

  if (m_pDriver)
    m_pDriver->Flush();
}

void plNetworkPeer::DispatchBufferedMessage(plBufferMessage& bufferMessage)
{
  const plUInt32 uiLength = bufferMessage.m_Data.GetCount();
  plNetworkMessage msg(bufferMessage.m_Data.GetData(), 0, uiLength, uiLength, 0);
  msg.SetChannelType(bufferMessage.m_ChannelType);

  if (bufferMessage.m_bToServer)
    m_pDriver->SendMessages(bufferMessage.m_ChannelType, msg);
  else if (bufferMessage.m_Targets.GetCount() == 1)
    m_pDriver->SendMessages(bufferMessage.m_ChannelType, msg, bufferMessage.m_Targets[0]);
  else
    m_pDriver->SendMessages(bufferMessage.m_ChannelType, msg, bufferMessage.m_Targets);
}

void plNetworkPeer::FlushOutgoingBuffer()
{
  if (m_OutgoingBuffer.IsEmpty() || !m_pDriver)
    return;

  // First in, first out: reliable ordered messages reach the driver in the order they were sent.
  for (plBufferMessage& bufferMessage : m_OutgoingBuffer)
    DispatchBufferedMessage(bufferMessage);

  m_OutgoingBuffer.Clear();
}
