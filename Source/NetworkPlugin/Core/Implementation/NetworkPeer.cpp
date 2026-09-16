#include "Foundation/System/SystemInformation.h"

#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/Driver/DriverInterface.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/System/SystemInformation.h>
#include <Foundation/Memory/PageAllocator.h>

#if BUILDSYSTEM_ENABLE_ENET_SUPPORT
#include <NetworkPlugin/Core/Driver/enet/DriverEnet.h>
#endif
#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>

plDynamicArray<plNetworkPeer*> plNetworkPeer::m_Peers;

plUInt32 lastHostID = 0;

namespace
{
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

  m_pDriver = CreateDriver(m_Config.m_DriverType);
  if (!m_pDriver)
  {
    return PL_FAILURE;
  }

  CreateMessageBuffers();
  m_MessagePool.Clear();

  for (plUInt32 messageId = 0; messageId < m_Config.m_uiMessagePoolSize; messageId++)
  {
    m_MessagePool.PushBack(messageId);
  }

  if(m_pDriver->Initalize(this, m_Config).Failed())
  {
    // The Platform driver needs a backend with a transport; without one the peer runs on ENet instead.
    if (m_Config.m_DriverType == plDriverTypeEnum::Platform)
    {
      plLog::Warning("PL Net: Platform transport unavailable, falling back to the ENet driver");
      PL_DEFAULT_DELETE(m_pDriver);
      m_Config.m_DriverType = plDriverTypeEnum::ENet;
      m_pDriver = CreateDriver(m_Config.m_DriverType);
    }

    if (!m_pDriver || m_pDriver->Initalize(this, m_Config).Failed())
    {
      plLog::Error("PL Net: Failed to initialize network driver");
      return PL_FAILURE;
    }
  }

  plStringBuilder sTmp;
  plLog::Info("PL Net: Network Peer Initialized using driver: {}", m_pDriver->GetDriverName().GetData(sTmp));
  return PL_SUCCESS;
}

void plNetworkPeer::Shutdown()
{
  if (!m_pDriver)
    return;

  m_pDriver->Shutdown();
  PL_DEFAULT_DELETE(m_pDriver);

  DestroyMessageBuffers();

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
void plNetworkPeer::Disconnect()
{
  plLog::Info("PL Net: Network Peer Disconnecting");
  m_pDriver->Disconnect();
}

void plNetworkPeer::Disconnect(const plNetworkConnection& connection)
{
  plLog::Info("PL Net: Network Peer Disconnecting");
  m_pDriver->Disconnect(connection);
}
plResult plNetworkPeer::PopEvent(plNetworkEvent& event)
{
  PL_PROFILE_SCOPE("plNetworkPeer::PopEvent");
  return m_pDriver->PopEvent(event);
}

void plNetworkPeer::SendMessages(const plNetworkMessage& message)
{
  if (!message.IsValid() || message.HasWriteError())
  {
    plLog::Error("PL Net: Dropping invalid outgoing message");
    if (message.IsValid())
    {
      plNetworkMessage messageToRelease = message;
      DestroyMessage(messageToRelease);
    }
    return;
  }

  plBufferMessage buffer;
  buffer.m_bIsClient = true;
  buffer.m_message = message;
  buffer.m_QueueTime = plTime::Now();

  m_OutgoingBuffer.PushBack(buffer);
}

void plNetworkPeer::SendMessages(const plNetworkMessage& message, plNetworkConnection target)
{
  if (!message.IsValid() || message.HasWriteError())
  {
    plLog::Error("PL Net: Dropping invalid outgoing message");
    if (message.IsValid())
    {
      plNetworkMessage messageToRelease = message;
      DestroyMessage(messageToRelease);
    }
    return;
  }

  plBufferMessage buffer;
  buffer.m_Targets.PushBack(target);
  buffer.m_message = message;
  buffer.m_QueueTime = plTime::Now();

  m_OutgoingBuffer.PushBack(buffer);
}

void plNetworkPeer::SendMessages(const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target)
{
  if (!message.IsValid() || message.HasWriteError())
  {
    plLog::Error("PL Net: Dropping invalid outgoing message");
    if (message.IsValid())
    {
      plNetworkMessage messageToRelease = message;
      DestroyMessage(messageToRelease);
    }
    return;
  }

  plBufferMessage buffer;
  buffer.m_Targets = target;
  buffer.m_message = message;
  buffer.m_QueueTime = plTime::Now();

  m_OutgoingBuffer.PushBack(buffer);
}

plNetworkPeer* plNetworkPeer::CreatePeer(const plNetworkConfig& config)
{
  if(config.m_sServerAddress.IsEmpty())
  {
    plLog::Error("PL Net: Server Address is empty when creating network peer");
    return nullptr;
  }

  plNetworkPeer* pPeer = PL_DEFAULT_NEW(plNetworkPeer);
  pPeer->m_iHostID = ++lastHostID;

  if(pPeer->Initialize(config).Failed())
  {
    PL_DEFAULT_DELETE(pPeer);
    return nullptr;
  }

  m_Peers.PushBack(pPeer);
  return pPeer;
}

void plNetworkPeer::DestroyPeer(plNetworkPeer* pPeer)
{
  if (!pPeer)
    return;

  pPeer->Shutdown();
  pPeer->m_iHostID = -1;
  m_Peers.RemoveAndSwap(pPeer);
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
  {
    plLog::Error("PL Net: Message pool exhausted - messages are not being cleaned up properly or pool size is too small");
    return plNetworkMessage(); // Return invalid message instead of crashing
  }

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
  m_MessagePool.PushBack(message.m_uiMessageID);
  message.m_bPendingRelease = true;
  plMemoryUtils::PatternFill(message.m_pBuffer, 0xDD, message.m_uiLength);
}

plNetworkMessage plNetworkPeer::BeginSendMessage()
{
  return CreateMessage();
}

void plNetworkPeer::CancelSendMessage(plNetworkMessage& message)
{
  PL_ASSERT_DEV(message.IsValid(), "Invalid message");
  DestroyMessage(message);
}

//bool plNetworkPeer::EndSendMessage(plNetworkChannelTypeEnum::Enum channelType, plNetworkMessage& message)
//{
//  PL_ASSERT_DEV(message.IsValid(), "Invalid message");
//  g_pDriver->SendMessages(channelType, message);
//  DestroyMessage(message);
//  return false;
//}
//
//bool plNetworkPeer::EndSendMessage(plNetworkChannelTypeEnum::Enum channelType, plNetworkMessage& message, const plNetworkConnection& target)
//{
//  PL_ASSERT_DEV(message.IsValid(), "Invalid message");
//  g_pDriver->SendMessages(channelType, message, target);
//  DestroyMessage(message);
//  return false;
//}
//
//bool plNetworkPeer::EndSendMessage(plNetworkChannelTypeEnum::Enum channelType, plNetworkMessage& message, const plDynamicArray<plNetworkConnection>& targets)
//{
//  PL_ASSERT_DEV(message.IsValid(), "Invalid message");
//  g_pDriver->SendMessages(channelType, message, targets);
//  DestroyMessage(message);
//  return false;
//}

bool plNetworkPeer::IsValidMessageID(plUInt32 messageID) const
{
  return messageID < m_Config.m_uiMessagePoolSize &&
         m_MessageBuffer != nullptr;
}

void plNetworkPeer::Update(float dt)
{
  PL_IGNORE_UNUSED(dt);
  FlushOutgoingBuffer();

  if (m_pDriver)
  {
    m_pDriver->Flush();
  }
}

void plNetworkPeer::DispatchBufferedMessage(plBufferMessage& bufferMessage)
{
  plNetworkMessage& msg = bufferMessage.m_message;

  if (bufferMessage.m_bIsClient)
  {
    m_pDriver->SendMessages(msg.m_ChannelType, msg);
  }
  else if (bufferMessage.m_Targets.GetCount() == 1)
  {
    m_pDriver->SendMessages(msg.m_ChannelType, msg, bufferMessage.m_Targets[0]);
  }
  else
  {
    m_pDriver->SendMessages(msg.m_ChannelType, msg, bufferMessage.m_Targets);
  }
}

void plNetworkPeer::FlushOutgoingBuffer()
{
  if (m_OutgoingBuffer.IsEmpty())
    return;

  m_OutgoingBuffer.Sort([](const plBufferMessage& a, const plBufferMessage& b) {
    return a.GetEffectivePriority() < b.GetEffectivePriority();
  });

  for (auto& bufferMessage : m_OutgoingBuffer)
  {
    DispatchBufferedMessage(bufferMessage);
    DestroyMessage(bufferMessage.m_message);
  }

  m_OutgoingBuffer.Clear();
}
