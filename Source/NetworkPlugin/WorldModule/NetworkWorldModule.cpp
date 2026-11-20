#include <NetworkPlugin/NetworkPluginPCH.h>

#include <Core/GameApplication/GameApplicationBase.h>
#include <Core/GameState/GameStateBase.h>
#include <Core/World/World.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Utilities/CommandLineUtils.h>
#include <Foundation/Types/ScopeExit.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Components/NetworkedTransformComponent.h>
#include <NetworkPlugin/Core/Driver/DriverInterface.h>
#include <NetworkPlugin/Core/NetworkBandwidth.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkRpc.h>
#include <NetworkPlugin/Core/NetworkTime.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <RendererCore/Debug/DebugRenderer.h>

PL_IMPLEMENT_WORLD_MODULE(plNetworkWorldModule);
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plNetworkWorldModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE

plCVarBool cvar_DrawNetworkInfo("Networking.DrawInfo", false, plCVarFlags::Default, "Draw Network info to the screen.");

namespace
{
  void WriteReplicationHeader(plNetworkMessage& ref_msg, plUInt32 uiClientID, plUInt32 uiComponentID)
  {
    ref_msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::ObjectReplicate));
    ref_msg.WriteUInt32(uiClientID);
    ref_msg.WriteUInt32(uiComponentID);
  }

  bool ReadReplicationHeader(plNetworkMessage& ref_msg, plNetworkReplicationMessage& out_header)
  {
    out_header.m_uiClientID = ref_msg.ReadUInt32();
    out_header.m_uiUUID = ref_msg.ReadUInt32();
    return !ref_msg.HasReadError();
  }

  plStringBuilder BuildGameObjectPath(const plGameObject* pObject)
  {
    plStringBuilder path;
    plStringBuilder segment;
    for (const plGameObject* p = pObject; p != nullptr; p = p->GetParent())
    {
      segment = p->GetName().IsEmpty() ? "_" : p->GetName();

      // Disambiguate identically-named (incl. unnamed) siblings by their child index,
      // which is deterministic across peers loading the same scene.
      if (const plGameObject* pParent = p->GetParent())
      {
        plUInt32 uiSameName = 0;
        for (auto itChild = pParent->GetChildren(); itChild.IsValid(); ++itChild)
        {
          const plGameObject* pSibling = itChild;
          if (pSibling == p)
            break;
          if (pSibling->GetName() == p->GetName())
            ++uiSameName;
        }
        if (uiSameName > 0)
          segment.AppendFormat("#{}", uiSameName);
      }

      path.Prepend(segment);
      path.Prepend("/");
    }
    return path;
  }

  plUInt32 ComputeSceneNetID(const plNetworkComponent* pComponent)
  {
    const plGameObject* pOwner = pComponent->GetOwner();
    plStringBuilder path = BuildGameObjectPath(pOwner);
    plUInt32 hash = plHashingUtils::xxHash32String(path);
    hash = plHashingUtils::CombineHashValues32(hash, plHashingUtils::StringHash(pComponent->GetDynamicRTTI()->GetTypeName()));

    plUInt32 uiTypeIndex = 0;
    for (const plComponent* pSibling : pOwner->GetComponents())
    {
      if (pSibling == pComponent)
        break;
      if (pSibling->GetDynamicRTTI() == pComponent->GetDynamicRTTI())
        ++uiTypeIndex;
    }
    hash = plHashingUtils::CombineHashValues32(hash, uiTypeIndex);
    return hash | 0x80000000u;
  }
}

plNetworkWorldModule::plNetworkWorldModule(plWorld* pWorld)
  : plWorldModule(pWorld)
{
}

plNetworkWorldModule::~plNetworkWorldModule()
{
  m_bDestroying = true;
  m_NetworkComponents.Clear();
  m_ComponentsByNetID.Clear();
  m_NetworkObjects.Clear();
  Stop();
  m_Session.Shutdown();
  m_LevelManager.Shutdown();
  m_LagCompensation.Shutdown();
  m_NetworkTime.Shutdown();
  m_BandwidthManager.Shutdown();
  m_RpcManager.Shutdown();
  m_ObjectManager.Shutdown();
  m_NetworkComponents.Clear();
  m_NetworkObjects.Clear();
}

void plNetworkWorldModule::Initialize()
{
  plWorldModule::Initialize();

  // Initialize managers
  m_ObjectManager.Initialize(this);
  m_RpcManager.Initialize(this);
  m_BandwidthManager.Initialize(this);
  m_PredictionManager.Initialize(this);
  m_NetworkTime.Initialize(this);
  m_LagCompensation.Initialize();
  m_LevelManager.Initialize(this);
  m_Session.Initialize(this);

  {
    auto updateDesc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plNetworkWorldModule::OnUpdate, this);
    updateDesc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostTransform;
    updateDesc.m_bOnlyUpdateWhenSimulating = true;

    RegisterUpdateFunction(updateDesc);
  }
}

bool plNetworkWorldModule::StartClient()
{
  if (m_bUpdating || m_bStopping)
    return false;
  PL_PROFILE_SCOPE("Network Start Client");
  Stop();

  m_Role = plNetworkRoleEnum::Client;

  plLog::Dev("Starting Network Client");
  if (!StartPeer())
  {
    m_Status = plNetworkConnectionState::Offline;
    return false;
  }

  if (m_pPeer->Connect().Failed())
  {
    Stop();
    return false;
  }

  m_uiLocalClientID = 0;
  m_uiNextClientID = 1;
  plNetworkConnection connection;
  connection.m_uiConnectionID = 0;
  m_pLocalClient = PL_DEFAULT_NEW(plNetworkClient);
  m_pLocalClient->m_Connection = connection;
  m_pLocalClient->m_uiClientID = m_uiLocalClientID;
  m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Connecting;
  return true;
}

void plNetworkWorldModule::LogInfo()
{
  if (m_pPeer && cvar_DrawNetworkInfo.GetValue())
  {
    plWorld* pWorld = GetWorld();
    plNetworkStats stats;
    plNetworkDriverInterface* pDriver = m_pPeer->GetDriver();
    if (m_pPeer->m_Role == plNetworkRoleEnum::Client)
    {
      stats = pDriver->GetStats();

      plStringBuilder sbPacketLost;
      sbPacketLost.SetFormat("Packets Lost: {0}", stats.m_uiPacketsLost);
      plFormatString sbPacketLostStr(sbPacketLost);

      plStringBuilder sbPacketSentTotal;
      sbPacketSentTotal.SetFormat("Total Packets Sent: {0}", stats.m_uiTotalPacketsSent);
      plFormatString sbPacketSentTotalStr(sbPacketSentTotal);

      plStringBuilder sbPacketLostTotal;
      sbPacketLostTotal.SetFormat("Total Packets Sent: {0}", stats.m_uiTotalPacketsLost);
      plFormatString sbPacketLostTotalStr(sbPacketLostTotal);

      plStringBuilder sbBytesSentTotal;
      sbBytesSentTotal.SetFormat("Total Bytes Sent: {0}", stats.m_uiTotalBytesSent);
      plFormatString sbBytesTotalStr(sbBytesSentTotal);

      plStringBuilder sbBytesReceivedTotal;
      sbBytesReceivedTotal.SetFormat("Total Bytes Sent: {0}", stats.m_uiTotalBytesReceived);
      plFormatString sbBytesReceivedTotalStr(sbBytesReceivedTotal);

      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", "Network Role: Client", plColor::White);
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", sbPacketLostStr, plColor::White);
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", sbPacketSentTotalStr, plColor::White);
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", sbPacketLostTotalStr, plColor::White);
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", sbBytesTotalStr, plColor::White);
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", sbBytesReceivedTotalStr, plColor::White);
    }
    else
    {
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", "Network Role: Server", plColor::White);

      plStringBuilder sClientCount;
      sClientCount.SetFormat("Client Count: {0}", m_pClients.GetCount());
      plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::BottomLeft, "Network", sClientCount, plColor::White);
    }

    // Draw bandwidth stats (uses its own CVar)
    m_BandwidthManager.DrawDebugStats(pWorld);
  }
}

plNetworkClient* plNetworkWorldModule::GetClient(plUInt32 uiClientID)
{
  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient->m_uiClientID == uiClientID)
    {
      return pClient;
    }
  }

  return nullptr;
}

plNetworkClient* plNetworkWorldModule::GetClient(const plNetworkConnection& connection)
{
  if (connection.m_uiConnectionID == 0)
  {
    return m_pLocalClient;
  }

  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient->m_Connection == connection)
    {
      return pClient;
    }
  }

  return nullptr;
}


bool plNetworkWorldModule::StartServer()
{
  if (m_bUpdating || m_bStopping)
    return false;
  PL_PROFILE_SCOPE("Network Start Server");
  Stop();

  m_Role = plNetworkRoleEnum::Server;

  plLog::Dev("Starting Network Server");
  if (!StartPeer())
  {
    m_Status = plNetworkConnectionState::Offline;
    return false;
  }

  if (m_pPeer->Listen().Failed())
  {
    Stop();
    return false;
  }

  m_uiLocalClientID = m_uiServerClientID;
  m_uiNextClientID = m_uiServerClientID + 1;

  m_Status = plNetworkConnectionState::Connected;
  BroadcastNetworkStateChange();

  return true;
}

bool plNetworkWorldModule::StartHost()
{
  if (m_bUpdating || m_bStopping)
    return false;
  PL_PROFILE_SCOPE("Network Start Host");
  Stop();

  m_Role = plNetworkRoleEnum::Host;

  plLog::Dev("Starting Network Host");
  if (!StartPeer())
  {
    m_Role = plNetworkRoleEnum::Undefined;
    m_Status = plNetworkConnectionState::Offline;
    return false;
  }

  if (m_pPeer->Listen().Failed())
  {
    // Nobody is connected yet, so there is nothing to announce: release the driver and its socket right away instead of
    // keeping a dead host peer until the next Stop().
    StopPeer();
    m_Role = plNetworkRoleEnum::Undefined;
    m_Status = plNetworkConnectionState::Offline;
    return false;
  }

  m_uiLocalClientID = m_uiServerClientID;
  m_uiNextClientID = m_uiServerClientID + 1;

  plNetworkConnection connection;
  connection.m_uiConnectionID = 0;

  m_pLocalClient = PL_DEFAULT_NEW(plNetworkClient);
  m_pLocalClient->m_Connection = connection;
  m_pLocalClient->m_uiClientID = m_uiLocalClientID;

  m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Connecting;
  m_Status = plNetworkConnectionState::Connected;
  BroadcastNetworkStateChange();

  m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Connected;
  m_pLocalClient->m_bAdmissionNotified = true;
  BroadcastClientConnected(m_pLocalClient);
  return true;
}

void plNetworkWorldModule::Stop(plNetworkDisconnectReason::Enum reason)
{
  if (m_bStopping)
    return;
  if (m_bUpdating)
  {
    m_bStopRequested = true;
    m_DeferredStopReason = reason;
    return;
  }
  m_bStopping = true;
  PL_SCOPE_EXIT(m_bStopping = false);
  if (m_Status == plNetworkConnectionState::Offline && m_Role == plNetworkRoleEnum::Undefined)
  {
    return;
  }

  PL_PROFILE_SCOPE("Network Stop");

  // A host or dedicated server that stops without a reason of its own is shutting the session down.
  if (m_Role != plNetworkRoleEnum::Client && reason == plNetworkDisconnectReason::ClientQuit)
    reason = plNetworkDisconnectReason::ServerShutdown;

  plLog::Dev("Stopping Network (reason: {})", static_cast<plUInt8>(reason));
  m_LastDisconnectReason = reason;

  // Every admitted client, or the server, learns why; the peer sends it before the connections close.
  if (m_pPeer && m_Role == plNetworkRoleEnum::Client)
  {
    SendDisconnect(reason);
  }
  else if (m_pPeer)
  {
    BroadcastDisconnect(reason);
    m_pPeer->DisconnectAll(reason);
  }

  m_Status = plNetworkConnectionState::Disconnecting;
  if (m_pLocalClient)
  {
    m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
  }
  for (plNetworkClient* client : m_pClients)
  {
    client->m_ConnectionState = plNetworkConnectionState::Disconnecting;
  }
  BroadcastNetworkStateChange();

  // Notify all clients of disconnection first, then delete them
  // (avoid modifying array while iterating)
  for (plNetworkClient* client : m_pClients)
  {
    if (client->m_bAdmissionNotified)
      BroadcastClientDisconnected(client);
    client->m_ConnectionState = plNetworkConnectionState::Disconnected;
  }
  
  // Now delete all clients and clear the array
  for (plNetworkClient* client : m_pClients)
  {
    PL_DEFAULT_DELETE(client);
  }
  m_pClients.Clear();

  if (m_Role == plNetworkRoleEnum::Host && m_pLocalClient)
  {
    BroadcastClientDisconnected(m_pLocalClient);
    m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Disconnected;
  }

  StopPeer();
  m_LevelManager.Shutdown();
  m_LevelManager.Initialize(this);

  // The next session starts clean: no objects or NetIDs, no clock offset, no held messages.
  m_ObjectManager.ResetSession(!m_bDestroying);
  m_NetworkTime.Initialize(this);
  m_ReplicationAccumulator = plTime::MakeZero();

  m_HeldMessages.Clear();
  m_HeldReady.Clear();
  m_uiHeldBytes = 0;
  if (m_pLocalClient)
  {
    PL_DEFAULT_DELETE(m_pLocalClient);
    m_pLocalClient = nullptr;
  }

  m_Status = plNetworkConnectionState::Disconnected;
  m_Role = plNetworkRoleEnum::Undefined;
  m_LastUpdate = plNetworkClock::Now();
  BroadcastNetworkStateChange();
}

void plNetworkWorldModule::RegisterNetworkComponent(plNetworkComponent* pComponent)
{
  if (pComponent == nullptr || m_NetworkComponents.Contains(pComponent))
    return;

  // A spawned component got its NetID at instantiation. A scene component hashes its path; components with the same
  // path chain the hash in the order they register, which is the same on every peer that loads the scene.
  if (pComponent->GetNetID() == 0)
  {
    plUInt32 uiSceneID = ComputeSceneNetID(pComponent);
    for (plUInt32 uiChain = 1; m_ComponentsByNetID.Contains(uiSceneID); ++uiChain)
      uiSceneID = plHashingUtils::CombineHashValues32(uiSceneID, uiChain) | 0x80000000u;

    pComponent->SetNetID(uiSceneID);
  }
  else if (plNetworkComponent* pExisting = FindNetworkComponent(pComponent->GetNetID()))
  {
    plLog::Warning("PL Net: NetID {} of '{}' is already used by '{}'", pComponent->GetNetID(), BuildGameObjectPath(pComponent->GetOwner()),
      BuildGameObjectPath(pExisting->GetOwner()));
  }

  m_NetworkComponents.PushBack(pComponent);
  m_ComponentsByNetID[pComponent->GetNetID()] = pComponent;

  // Messages that arrived before the component started are applied at the start of the next update, ahead of newer ones.
  if (m_HeldMessages.Contains(pComponent->GetNetID()))
    m_HeldReady.PushBack(pComponent->GetNetID());

  if (m_Status == plNetworkConnectionState::Connected)
    pComponent->OnNetworkStateChanged(m_Status);
}

void plNetworkWorldModule::RemoveNetworkComponent(plNetworkComponent* pComponent)
{
  if (!m_NetworkComponents.RemoveAndSwap(pComponent))
    return;

  plNetworkComponent* pMapped = nullptr;
  if (m_ComponentsByNetID.TryGetValue(pComponent->GetNetID(), pMapped) && pMapped == pComponent)
    m_ComponentsByNetID.Remove(pComponent->GetNetID());

  m_BandwidthManager.RemoveComponentState(pComponent->GetNetID());
}

void plNetworkWorldModule::RegisterNetworkObject(plNetworkObject* pNetObject)
{
  if (pNetObject)
  {
    pNetObject->SetNetID(m_uiNextNetID++);
    m_NetworkObjects.PushBack(pNetObject);
  }

  if (m_uiNextNetID > 4000000000)
  {
    plLog::Error("PL Net: Too many networked items in use");
  }
}

void plNetworkWorldModule::RemoveNetworkObject(plNetworkObject* pNetObject)
{
  if (m_NetworkObjects.Contains(pNetObject))
  {
    m_NetworkObjects.RemoveAndSwap(pNetObject);
  }
}

void plNetworkWorldModule::OnUpdate(const UpdateContext& ctxt)
{
  // Callbacks may stop the session. Keep the peer, clients and pooled event storage alive
  // until the current dispatch unwinds; never process the next event after that request.
  m_bUpdating = true;
  PL_SCOPE_EXIT({
    m_bUpdating = false;
    if (m_bStopRequested)
    {
      m_bStopRequested = false;
      Stop(m_DeferredStopReason);
    }
  });
  float dt = GetWorld()->GetClock().GetTimeDiff().AsFloatInSeconds();
  plTime timeNow = plNetworkClock::Now();

  // Always update bandwidth manager for debug visualization (even when not connected)
  m_BandwidthManager.Update(dt);

  // Prediction owns a fixed 60 Hz accumulator and is intentionally independent of send gating.
  m_PredictionManager.Update(dt);

  // Update network time synchronization
  m_NetworkTime.Update();

  // Update lag compensation (server only - prune old snapshots)
  if (IsServer() || IsHost())
  {
    m_LagCompensation.Update(m_NetworkTime.GetNetworkTime());
  }

  // Update level manager (server-side timeout tracking)
  m_LevelManager.Update(dt);

  // Starts a client connection that a resolved platform lobby join deferred to the world update.
  // Session::Update may intentionally start a deferred lobby connection.
  m_bUpdating = false;
  m_Session.Update();
  m_bUpdating = true;

  if (m_bStopRequested || m_Role == plNetworkRoleEnum::Undefined || !m_pPeer)
  {
    LogInfo();
    return;
  }

  PL_PROFILE_SCOPE("Network Update");

  // What arrived before its component started is applied ahead of anything newer.
  ReleaseHeldMessages();

  // Drain incoming events every frame so ENet can flush/receive without tick aliasing.
  plNetworkEvent event;
  while (!m_bStopRequested && m_pPeer->PopEvent(event).Succeeded())
  {
    switch (event.m_EventType)
    {
      case plNetworkEventTypeEnum::Connected:
      {
        plLog::Info("Incoming Connection with ID: {}", event.m_Sender.m_uiConnectionID);
        if (IsClient())
        {
          plNetworkClientConnectionData data;
          data.m_pClient = m_pLocalClient,
          data.m_Result = PL_SUCCESS,
          data.m_sPlatformName = PL_PLATFORM_NAME;

          for (plNetworkComponent* pComponent : m_NetworkComponents)
          {
            pComponent->OnClientConnecting(data);
            if (m_bStopRequested)
              return;
          }

          for (plNetworkObject* pNetObject : m_NetworkObjects)
          {
            pNetObject->OnClientConnecting(data);
            if (m_bStopRequested)
              return;
          }

          if (data.m_Result.Failed())
          {
            plLog::Warning("Connection failed");
            Stop();
            break;
          }

          plNetworkMessage msg = m_pPeer->BeginSendMessage();
          msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::Handshake));
          msg.WriteUInt32(NETWORK_PROTOCOL_VERSION);
          msg.WriteUInt32(m_uiGameVersion);
          msg.WriteString(PL_PLATFORM_NAME);
          const plUInt32 uiPayloadSize = plMath::Min<plUInt32>(data.m_pData.GetCount(), 0xFFFFu);
          msg.WriteUInt16(static_cast<plUInt16>(uiPayloadSize));
          if (!data.m_pData.IsEmpty())
          {
            msg.WriteBytes(data.m_pData.GetData(), uiPayloadSize);
          }

          msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
          m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
          m_pPeer->SendToServer(msg).IgnoreResult();
        }
        else
        {
          if (event.m_Sender.m_uiConnectionID == 0 || GetClient(event.m_Sender) != nullptr ||
              m_pClients.GetCount() >= m_Config.m_uiConnectionLimit || m_uiNextClientID == 0)
          {
            const bool bFull = m_pClients.GetCount() >= m_Config.m_uiConnectionLimit;
            m_pPeer->Disconnect(event.m_Sender, bFull ? plNetworkDisconnectReason::ServerFull : plNetworkDisconnectReason::Unknown);
            break;
          }
          plNetworkClient* pClient = PL_DEFAULT_NEW(plNetworkClient);
          pClient->m_uiClientID = m_uiNextClientID++;
          pClient->m_Connection = event.m_Sender;
          pClient->UpdateActivity(); // Initialize activity timestamp
          pClient->m_AdmissionStartTime = plNetworkClock::Now();
          pClient->m_ConnectionState = plNetworkConnectionState::Connecting;
          m_pClients.PushBack(pClient);
        }
        break;
      }
      case plNetworkEventTypeEnum::Disconnected:
      case plNetworkEventTypeEnum::Timeout:
      {
        plLog::Info("Client Disconnected with ID: {}", event.m_Sender.m_uiConnectionID);
        if (IsClient())
        {
          // The server's reason travels with the disconnect; without one the connection was lost.
          plNetworkDisconnectReason::Enum reason = event.m_EventType == plNetworkEventTypeEnum::Timeout ? plNetworkDisconnectReason::Timeout : plNetworkDisconnectReason::Unknown;
          if (event.m_uiData > plNetworkDisconnectReason::Unknown && event.m_uiData <= plNetworkDisconnectReason::Banned)
            reason = static_cast<plNetworkDisconnectReason::Enum>(event.m_uiData);
          Stop(reason);
          return;
        }
        else
        {
          plNetworkClient* pClient = GetClient(event.m_Sender);
          if (!pClient)
          {
            plLog::Dev("Client with ID: {} not found", event.m_Sender.m_uiConnectionID);
            break;
          }

          plLog::Dev("Client Disconnected with ID: {}", event.m_Sender.m_uiConnectionID);
          m_pClients.RemoveAndCopy(pClient);
          RemoveClient(pClient, pClient->m_bAdmissionNotified);
        }
        break;
      }
      case plNetworkEventTypeEnum::Message:
      {
        plNetworkClient* pClient = GetClient(event.m_Sender);
       if (!pClient || pClient->m_ConnectionState == plNetworkConnectionState::Disconnecting ||
           pClient->m_ConnectionState == plNetworkConnectionState::Disconnected)
       {
          m_pPeer->DestroyMessage(event.m_Message);
          break;
       }

        // Update client activity timestamp
        if (pClient)
        {
          pClient->UpdateActivity();

          // Check rate limit for this client
          if (!CheckRateLimit(pClient))
          {
            // Rate limit exceeded - drop message
            m_pPeer->DestroyMessage(event.m_Message);
            break;
          }
        }

        plUInt8 id = event.m_Message.ReadUInt8();
        plNetworkMessageIDs::Enum msgID = static_cast<plNetworkMessageIDs::Enum>(id);
        const bool bHandshake = msgID == plNetworkMessageIDs::Handshake;
        const bool bReply = msgID == plNetworkMessageIDs::HandshakeReply;
        const bool bAdmitting = pClient->m_ConnectionState == plNetworkConnectionState::Connecting;
        if (event.m_Message.HasReadError() ||
            (bHandshake && (IsClient() || !bAdmitting)) ||
            (bReply && (!IsClient() || !bAdmitting)) ||
            (!bHandshake && !bReply && pClient->m_ConnectionState != plNetworkConnectionState::Connected))
        {
          RecordProtocolViolation(pClient, "message is not valid in the current admission state");
          m_pPeer->DestroyMessage(event.m_Message);
          break;
        }
        const plUInt32 senderClientID = IsClient() ? m_uiServerClientID : pClient->m_uiClientID;
        switch (msgID)
        {
          case plNetworkMessageIDs::Handshake:
          {
            event.m_Message.m_uiPosition = 1;
            HandleHandshake(event, pClient);
            break;
          }
          case plNetworkMessageIDs::HandshakeReply:
          {
            event.m_Message.m_uiPosition = 1;
            HandleHandshakeReply(event, pClient);
            break;
          }
          case plNetworkMessageIDs::ObjectSpawn:
          {
            if (!IsClient())
              break;
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_ObjectManager.HandleSpawnMessage(event.m_Message);
            break;
          }
          case plNetworkMessageIDs::ObjectDespawn:
          {
            if (!IsClient())
              break;
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_ObjectManager.HandleDespawnMessage(event.m_Message);
            break;
          }
          case plNetworkMessageIDs::ObjectRole:
          {
            if (!IsClient())
              break;
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_ObjectManager.HandleOwnershipMessage(event.m_Message);
            break;
          }
          case plNetworkMessageIDs::ObjectRpc:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_RpcManager.HandleRpcMessage(event.m_Message, senderClientID);
            break;
          }
          case plNetworkMessageIDs::TimeSync:
          {
            // Time sync request from client - server handles this
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            if (IsServer() || IsHost())
            {
              plUInt32 clientID = pClient ? pClient->m_uiClientID : 0;
              m_NetworkTime.HandleTimeSyncRequest(event.m_Message, clientID);
            }
            break;
          }
          case plNetworkMessageIDs::TimeSyncResponse:
          {
            // Time sync response from server - client handles this
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            if (IsClient())
            {
              m_NetworkTime.HandleTimeSyncResponse(event.m_Message);
            }
            break;
          }
          case plNetworkMessageIDs::ObjectReplicate:
          {
            event.m_Message.m_uiPosition = 1;
            plNetworkReplicationMessage msgData;
            if (!ReadReplicationHeader(event.m_Message, msgData))
            {
              break;
            }
            
            if (msgData.m_uiClientID == m_uiLocalClientID)
            {
              break;
            }

            HandleReplicationMessage(event.m_Message, msgData.m_uiUUID, senderClientID);
            break;
          }
          case plNetworkMessageIDs::Disconnect:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            const plUInt32 uiSenderClientID = event.m_Message.ReadUInt32();
            plUInt8 reasonByte = event.m_Message.ReadUInt8();
            plNetworkDisconnectReason::Enum reason = static_cast<plNetworkDisconnectReason::Enum>(reasonByte);
            PL_IGNORE_UNUSED(uiSenderClientID);
            if (event.m_Message.HasReadError())
              break;
            
            if (IsClient())
            {
              // Server is disconnecting us
              plLog::Info("PL Net: Server disconnected us (reason: {})", reasonByte);
              Stop(reason);
            }
            else if (pClient)
            {
              // Client is gracefully disconnecting
              plLog::Info("PL Net: Client {} disconnected (reason: {})", pClient->m_uiClientID, reasonByte);
              KickClient(pClient->m_uiClientID, reason);
            }
            break;
          }
          case plNetworkMessageIDs::Reconnect:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            plUInt64 sessionToken = event.m_Message.ReadUInt64();
            
            if (IsServer() || IsHost())
            {
              plNetworkClient* pReconnectClient = FindClientBySessionToken(sessionToken);
              if (pReconnectClient && pReconnectClient->m_bReservedForReconnect)
              {
                plLog::Info("PL Net: Client reconnected with token {}", sessionToken);
                pReconnectClient->m_bReservedForReconnect = false;
                pReconnectClient->m_ConnectionState = plNetworkConnectionState::Connected;
                pReconnectClient->m_Connection = event.m_Sender;
                pReconnectClient->UpdateActivity();
                
                // Send reconnect response with success
                plNetworkMessage response = m_pPeer->BeginSendMessage();
                response.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::ReconnectResponse));
                response.WriteUInt8(1); // Success
                response.WriteUInt32(pReconnectClient->m_uiClientID);
                response.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
                m_pPeer->SendTo(response, event.m_Sender).IgnoreResult();
                
                // Sync state to reconnected client
                SyncComponentStatesToClient(pReconnectClient);
              }
              else
              {
                plLog::Warning("PL Net: Reconnection failed - invalid or expired token {}", sessionToken);
                
                // Send reconnect response with failure
                plNetworkMessage response = m_pPeer->BeginSendMessage();
                response.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::ReconnectResponse));
                response.WriteUInt8(0); // Failure
                response.WriteUInt32(0); // No client ID
                response.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
                m_pPeer->SendTo(response, event.m_Sender).IgnoreResult();
              }
            }
            break;
          }
          case plNetworkMessageIDs::ReconnectResponse:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            plUInt8 success = event.m_Message.ReadUInt8();
            plUInt32 clientID = event.m_Message.ReadUInt32();
            
            if (success && IsClient())
            {
              plLog::Info("PL Net: Reconnection successful, client ID: {}", clientID);
              m_uiLocalClientID = clientID;
              m_Status = plNetworkConnectionState::Connected;
              if (m_pLocalClient)
              {
                m_pLocalClient->m_uiClientID = clientID;
                m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Connected;
              }
              BroadcastNetworkStateChange();
            }
            else if (IsClient())
            {
              plLog::Warning("PL Net: Reconnection failed, starting fresh connection");
              // Could trigger a fresh connection attempt here
            }
            break;
          }
          case plNetworkMessageIDs::LevelLoad:
          {
            if (!IsClient())
              break;
            event.m_Message.m_uiPosition = 1;
            const plUInt64 uiGeneration = event.m_Message.ReadUInt64();
            plString sLevelPath = event.m_Message.ReadString();
            if (!event.m_Message.HasReadError())
              m_LevelManager.HandleLevelLoadMessage(sLevelPath, uiGeneration);
            break;
          }
          case plNetworkMessageIDs::LevelLoadComplete:
          {
            event.m_Message.m_uiPosition = 1;
            const plUInt64 uiGeneration = event.m_Message.ReadUInt64();
            if (!IsClient() && !event.m_Message.HasReadError())
              m_LevelManager.HandleLevelLoadComplete(senderClientID, uiGeneration);
            break;
          }
          case plNetworkMessageIDs::LevelStart:
          {
            event.m_Message.m_uiPosition = 1;
            const plUInt64 uiGeneration = event.m_Message.ReadUInt64();
            if (IsClient() && !event.m_Message.HasReadError())
              m_LevelManager.HandleLevelStartMessage(uiGeneration);
            break;
          }
          case plNetworkMessageIDs::TransformState:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            plUInt32 targetNetID = event.m_Message.ReadUInt32();
            if (event.m_Message.HasReadError() || !ValidateStateUpdate(targetNetID, senderClientID))
              break;

            for (plNetworkComponent* pComponent : m_NetworkComponents)
            {
              if (targetNetID == pComponent->GetNetID())
              {
                if (pComponent->GetDynamicRTTI()->IsDerivedFrom<plNetworkedTransformComponent>())
                {
                  static_cast<plNetworkedTransformComponent*>(pComponent)->HandleTransformStateMessage(event.m_Message);
                }
                break;
              }
            }
            break;
          }
          default:
          {
            // Check for engine component messages (64-127)
            if (id >= plNetworkMessageIDs::EngineMessageStart && id <= plNetworkMessageIDs::EngineMessageEnd)
            {
              event.m_Message.m_uiPosition = 1; // Skip the message ID byte
              HandleEngineMessage(id, event.m_Message, event.m_Sender);
            }
            // IDs >= 128 are reserved for user/game messages
            else if (id >= plNetworkMessageIDs::UserMessageStart)
            {
              // Broadcast as plMsgNetworkUserMessage to all network components
              event.m_Message.m_uiPosition = 1; // Skip the message ID byte
              plMsgNetworkUserMessage msg;
              msg.m_uiMessageID = id;
              msg.m_Sender = event.m_Sender;
              msg.m_pNetworkMessage = &event.m_Message;
              BroadcastToNetworkComponents(msg);
              if (m_bStopRequested)
                break;

              // Also forward to plNetworkObject instances so game code using
              // RegisterNetworkObject() can handle user-range messages via OnNetworkMessage()
              HandleEngineMessage(id, event.m_Message, event.m_Sender);
            }
            else
            {
              plLog::Warning("Message of id {} not found", id);
            }
            break;
          }
        }
        if (!m_bStopRequested && event.m_Message.HasReadError())
        {
          RecordProtocolViolation(pClient, "message payload ended before all fields were read");
        }
        m_pPeer->DestroyMessage(event.m_Message);
        break;
      }
      default:
        break;
    }
  }

  if (m_bStopRequested)
    return;

  // Dirty replication is gated by network tick rate; transport pump runs every frame.
  if (IsReplicationTick(timeNow))
  {
    if (IsServer() || IsHost())
    {
      RecordLagCompensationSnapshots();
    }

    ReplicateDirtyComponents();
  }

  // Check for inactive client timeouts (server only)
  if (IsServer() || IsHost())
  {
    CheckClientTimeouts();
    CleanupExpiredReconnectSlots();
  }

  m_pPeer->Update(dt);

  // Draw debug HUD (gated internally by the Networking.DrawInfo CVar)
  LogInfo();
}

void plNetworkWorldModule::HandleReplicationMessage(plNetworkMessage& msg, plUInt32 uiComponentNetID, plUInt32 uiSenderClientID)
{
  plNetworkComponent* pComponent = FindNetworkComponent(uiComponentNetID);

  if (pComponent == nullptr)
  {
    for (plNetworkObject* pNetObject : m_NetworkObjects)
    {
      if (uiComponentNetID == pNetObject->GetNetID())
      {
        pNetObject->OnNetworkMessage(msg);
        return;
      }
    }
  }

  // A client gets state for a component that has not started yet: a spawned one, or one in a level still loading.
  if (pComponent == nullptr || m_HeldMessages.Contains(uiComponentNetID))
  {
    if (IsClient() && uiSenderClientID == m_uiServerClientID)
      HoldMessage(HeldMessage::Kind::State, uiComponentNetID, uiSenderClientID, plArrayPtr<const plUInt8>(msg.m_pBuffer + msg.m_uiPosition, msg.GetRemainingBytes()));
    else
      plLog::Warning("PL Net: Rejected state update from client {} for unknown component {}", uiSenderClientID, uiComponentNetID);

    msg.m_uiPosition = msg.m_uiLength;
    return;
  }

  if (!ValidateStateUpdate(uiComponentNetID, uiSenderClientID))
  {
    plLog::Warning("PL Net: Rejected unauthorized state update from client {} for component {}", uiSenderClientID, uiComponentNetID);
    msg.m_uiPosition = msg.m_uiLength;
    return;
  }

  const plUInt32 uiPayloadStart = msg.m_uiPosition;
  pComponent->NetworkDeserialize(msg);
  if (msg.HasReadError() || m_bStopRequested)
    return;

  // An owner's upload reaches only the server; the others get what the owner sent, not the server's copy of it, which
  // may still be blending towards the received value.
  if (!IsClient() && uiSenderClientID != m_uiServerClientID)
    RelayComponentState(pComponent, uiSenderClientID, plArrayPtr<const plUInt8>(msg.m_pBuffer + uiPayloadStart, msg.m_uiLength - uiPayloadStart));
}

void plNetworkWorldModule::RelayComponentState(plNetworkComponent* pComponent, plUInt32 uiSenderClientID, plArrayPtr<const plUInt8> payload)
{
  plHybridArray<plNetworkClient*, 32> clients;
  GetConnectedClients(clients);

  for (plNetworkClient* pClient : clients)
  {
    if (pClient->m_uiClientID == uiSenderClientID)
      continue;

    plNetworkMessage received(const_cast<plUInt8*>(payload.GetPtr()), 0, payload.GetCount(), payload.GetCount(), 0);
    plNetworkMessage relay = m_pPeer->BeginSendMessage();
    WriteReplicationHeader(relay, m_uiServerClientID, pComponent->GetNetID());
    pComponent->NetworkRelayForClient(received, relay, pClient->m_uiClientID);
    relay.SetChannelType(pComponent->GetChannelType());
    m_BandwidthManager.RecordBytesSent(relay.m_uiLength);
    m_pPeer->SendTo(relay, pClient->m_Connection).IgnoreResult();
  }
}

void plNetworkWorldModule::HoldMessage(HeldMessage::Kind kind, plUInt32 uiComponentNetID, plUInt32 uiSenderClientID, plArrayPtr<const plUInt8> data)
{
  if (m_uiHeldBytes + data.GetCount() > MaxHeldBytes)
  {
    plLog::Warning("PL Net: Dropping a message for component {}: {} bytes are already waiting for components to start", uiComponentNetID, m_uiHeldBytes);
    return;
  }

  HeldMessage& held = m_HeldMessages[uiComponentNetID].ExpandAndGetRef();
  held.m_Kind = kind;
  held.m_uiSender = uiSenderClientID;
  held.m_Received = plNetworkClock::Now();
  held.m_Data = data;
  m_uiHeldBytes += data.GetCount();

  // The component may have started since the last update; its held messages are then due now.
  if (FindNetworkComponent(uiComponentNetID) != nullptr && !m_HeldReady.Contains(uiComponentNetID))
    m_HeldReady.PushBack(uiComponentNetID);
}

void plNetworkWorldModule::HoldStateForComponent(plUInt32 uiComponentNetID, plUInt32 uiSenderClientID, plArrayPtr<const plUInt8> state)
{
  HoldMessage(HeldMessage::Kind::State, uiComponentNetID, uiSenderClientID, state);
}

void plNetworkWorldModule::HoldRpcForComponent(plUInt32 uiComponentNetID, plUInt32 uiSenderClientID, plArrayPtr<const plUInt8> rpc)
{
  HoldMessage(HeldMessage::Kind::Rpc, uiComponentNetID, uiSenderClientID, rpc);
}

void plNetworkWorldModule::ReleaseHeldMessages()
{
  // Components that started since the last update get what arrived for them, in the order it arrived.
  while (!m_HeldReady.IsEmpty() && !m_bStopRequested)
  {
    const plUInt32 uiComponentNetID = m_HeldReady[0];
    m_HeldReady.RemoveAtAndCopy(0);

    plDynamicArray<HeldMessage> held;
    if (!m_HeldMessages.Remove(uiComponentNetID, &held))
      continue;

    plNetworkComponent* pComponent = FindNetworkComponent(uiComponentNetID);
    for (HeldMessage& message : held)
    {
      m_uiHeldBytes -= message.m_Data.GetCount();
      if (pComponent == nullptr || m_bStopRequested)
        continue;

      plNetworkMessage msg(message.m_Data.GetData(), 0, message.m_Data.GetCount(), message.m_Data.GetCount(), 0);
      if (message.m_Kind == HeldMessage::Kind::Rpc)
      {
        m_RpcManager.HandleRpcMessage(msg, message.m_uiSender);
      }
      else if (ValidateStateUpdate(uiComponentNetID, message.m_uiSender))
      {
        pComponent->NetworkDeserialize(msg);
      }
    }
  }

  // Messages for a component that never started are dropped once they are old.
  const plTime cutoff = plNetworkClock::Now() - GetMaxHoldTime();
  for (auto it = m_HeldMessages.GetIterator(); it.IsValid();)
  {
    if (!it.Value().IsEmpty() && it.Value()[0].m_Received < cutoff)
    {
      for (const HeldMessage& message : it.Value())
        m_uiHeldBytes -= message.m_Data.GetCount();

      plLog::Dev("PL Net: Dropped {} messages for component {}, which did not start in time", it.Value().GetCount(), it.Key());
      it = m_HeldMessages.Remove(it);
    }
    else
    {
      ++it;
    }
  }
}

bool plNetworkWorldModule::IsHoldingMessagesFor(plUInt32 uiComponentNetID) const
{
  return m_HeldMessages.Contains(uiComponentNetID);
}

bool plNetworkWorldModule::IsReplicationTick(plTime now)
{
  const plTime elapsed = now - m_LastUpdate;
  m_LastUpdate = now;

  if (m_fNetworkTickRate <= 0.0f)
    return true;

  // A fixed-step accumulator in plTime: the rate holds at any uptime, and a slow frame catches up one tick at most.
  const plTime step = plTime::MakeFromSeconds(1.0 / m_fNetworkTickRate);
  m_ReplicationAccumulator = plMath::Min(m_ReplicationAccumulator + elapsed, step * 2.0);
  if (m_ReplicationAccumulator < step)
    return false;

  m_ReplicationAccumulator -= step;
  return true;
}

bool plNetworkWorldModule::ShouldSendComponent(plNetworkComponent* pComponent) const
{
  // Whoever may write the state sends it: the server for its own state and for objects nobody owns, the owner for
  // Client-role state, and that owner may be the host.
  return HasLocalAuthority(pComponent);
}

plResult plNetworkWorldModule::SendComponentState(plNetworkComponent* pComponent)
{
  if (IsClient())
  {
    plNetworkMessage msg = m_pPeer->BeginSendMessage();
    WriteReplicationHeader(msg, m_uiLocalClientID, pComponent->GetNetID());
    pComponent->NetworkSerializeForClient(msg, m_uiServerClientID, false);
    msg.SetChannelType(pComponent->GetChannelType());
    m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
    return m_pPeer->SendToServer(msg);
  }

  if (!pComponent->RequiresPerRecipientSerialization())
  {
    plNetworkMessage msg = m_pPeer->BeginSendMessage();
    WriteReplicationHeader(msg, m_uiLocalClientID, pComponent->GetNetID());
    pComponent->NetworkSerialize(msg);
    msg.SetChannelType(pComponent->GetChannelType());
    return SendToConnectedClients(msg);
  }

  plResult result = PL_SUCCESS;
  plHybridArray<plNetworkClient*, 32> clients;
  GetConnectedClients(clients);
  for (plNetworkClient* pTargetClient : clients)
  {
    plNetworkMessage msg = m_pPeer->BeginSendMessage();
    WriteReplicationHeader(msg, m_uiLocalClientID, pComponent->GetNetID());
    pComponent->NetworkSerializeForClient(msg, pTargetClient->m_uiClientID, false);
    msg.SetChannelType(pComponent->GetChannelType());
    m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
    if (m_pPeer->SendTo(msg, pTargetClient->m_Connection).Failed())
      result = PL_FAILURE;
  }

  return result;
}

void plNetworkWorldModule::ReplicateDirtyComponents()
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    if (!pComponent->NeedReplication() || !ShouldSendComponent(pComponent))
      continue;

    // A priority interval that has not passed yet keeps the component dirty for a later tick.
    if (!m_BandwidthManager.ShouldSendUpdate(pComponent->GetNetID()))
      continue;

    // A state that could not be sent (it does not fit one message) stays dirty instead of being lost.
    if (SendComponentState(pComponent).Failed())
      continue;

    pComponent->ClearDirty();
    m_BandwidthManager.MarkUpdateSent(pComponent->GetNetID());
  }
}

void plNetworkWorldModule::BroadcastNetworkStateChange()
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnNetworkStateChanged(m_Status);
    if (m_bStopRequested)
      return;
  }

  for (plNetworkObject* pNetObject: m_NetworkObjects)
  {
    pNetObject->OnNetworkStateChanged(m_Status);
    if (m_bStopRequested)
      return;
  }
}

void plNetworkWorldModule::BroadcastClientConnected(plNetworkClient* pClient)
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnClientConnected(pClient);
    if (m_bStopRequested)
      return;
  }

  for (plNetworkObject* pNetObject : m_NetworkObjects)
  {
    pNetObject->OnClientConnected(pClient);
    if (m_bStopRequested)
      return;
  }

  // Notify session manager
  m_Session.OnClientConnected(pClient->m_uiClientID);
}

void plNetworkWorldModule::BroadcastClientDisconnected(plNetworkClient* pClient)
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnClientDisconnected(pClient);
  }

  for (plNetworkObject* pNetObject : m_NetworkObjects)
  {
    pNetObject->OnClientDisconnected(pClient);
  }

  // Notify session manager
  m_Session.OnClientDisconnected(pClient->m_uiClientID);
}

void plNetworkWorldModule::HandleHandshake(plNetworkEvent& event, plNetworkClient* pClient)
{
  if (IsClient() || !pClient || pClient->m_ConnectionState != plNetworkConnectionState::Connecting)
    return;
  plLog::Dev("Received Handshake");

  plNetworkClientConnecting msgData;
  msgData.m_uiProtocolVersion = event.m_Message.ReadUInt32();
  msgData.m_uiGameVersion = event.m_Message.ReadUInt32();
  const plString sPlatformName = event.m_Message.ReadString();
  msgData.m_uiPayloadSize = event.m_Message.ReadUInt16();

  if (event.m_Message.HasReadError() || msgData.m_uiPayloadSize > event.m_Message.GetRemainingBytes())
  {
    RecordProtocolViolation(pClient, "malformed handshake");
    return;
  }

  plLog::Dev("Handshake: Protocol={0} (expected {1}), GameVersion={2} (expected {3}), Platform={4}",
    msgData.m_uiProtocolVersion, NETWORK_PROTOCOL_VERSION, 
    msgData.m_uiGameVersion, m_uiGameVersion,
    sPlatformName);

  plNetworkClientConnectionData connectionData;
  connectionData.m_pClient = pClient;
  connectionData.m_Result = PL_SUCCESS;
  connectionData.m_sPlatformName = sPlatformName;
  connectionData.m_pData.SetCountUninitialized(msgData.m_uiPayloadSize);
  for (plUInt32 i = 0; i < msgData.m_uiPayloadSize; i++)
  {
    connectionData.m_pData[i] = event.m_Message.ReadUInt8();
  }

  const bool bVersionMatches = msgData.m_uiProtocolVersion == NETWORK_PROTOCOL_VERSION && msgData.m_uiGameVersion == m_uiGameVersion;
  if (!bVersionMatches)
  {
    plLog::Warning("Handshake version mismatch - rejecting client");
    connectionData.m_Result = PL_FAILURE;
  }

  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnClientConnecting(connectionData);
    if (m_bStopRequested)
      return;
  }

  for (plNetworkObject* pNetObject : m_NetworkObjects)
  {
    pNetObject->OnClientConnecting(connectionData);
    if (m_bStopRequested)
      return;
  }

  plLog::Dev("Handshake result after callbacks: {0}", connectionData.m_Result.Succeeded() ? "SUCCESS" : "FAILURE");

  plNetworkClientConnectingReply replyMessage;
  replyMessage.m_bSuccess = bVersionMatches && connectionData.m_Result.Succeeded();
  replyMessage.m_Reason = msgData.m_uiProtocolVersion != NETWORK_PROTOCOL_VERSION || msgData.m_uiGameVersion != m_uiGameVersion ?
    plNetworkDisconnectReason::VersionMismatch : plNetworkDisconnectReason::AuthenticationFailed;
  replyMessage.m_uiClientID = pClient->m_uiClientID;
  plNetworkMessage msgReply = m_pPeer->BeginSendMessage();
  msgReply.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::HandshakeReply));
  msgReply.WriteUInt8(replyMessage.m_bSuccess ? 1 : 0);
  msgReply.WriteUInt8(static_cast<plUInt8>(replyMessage.m_Reason));
  msgReply.WriteUInt32(NETWORK_PROTOCOL_VERSION);
  msgReply.WriteUInt32(replyMessage.m_uiClientID);
  msgReply.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  m_BandwidthManager.RecordBytesSent(msgReply.m_uiLength);
  m_pPeer->SendTo(msgReply, event.m_Sender).IgnoreResult();

  if (!replyMessage.m_bSuccess)
  {
    plLog::Warning("Client {0} connection failed", pClient->m_uiClientID);
    pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
    m_pPeer->Disconnect(event.m_Sender, replyMessage.m_Reason);
    pClient->m_ConnectionState = plNetworkConnectionState::Disconnected;
  }
  else
  {
    pClient->m_ConnectionState = plNetworkConnectionState::Connected;
    pClient->m_bAdmissionNotified = true;
    plLog::Info("Client {0} connected", pClient->m_uiClientID);

    // Sync existing state FIRST. OnClientConnected listeners typically spawn new
    // objects (e.g. the player), and those broadcasts already reach this client -
    // syncing afterwards would deliver the same spawn a second time.

    // Sync level state to the late-joiner (they need to load the correct level)
    m_LevelManager.SyncLevelToClient(pClient->m_uiClientID);

    // Sync all existing network objects to the late-joiner
    m_ObjectManager.SyncAllObjectsToClient(pClient->m_uiClientID);

    // Also sync current state of all network components
    SyncComponentStatesToClient(pClient);

    BroadcastClientConnected(pClient);
    if (m_bStopRequested)
      return;

    // Broadcast client connected message to all network components
    {
      plMsgNetworkClientConnected msg;
      msg.m_uiClientID = pClient->m_uiClientID;
      BroadcastToNetworkComponents(msg);
    }
  }
}

void plNetworkWorldModule::HandleHandshakeReply(plNetworkEvent& event, plNetworkClient* pClient)
{
  if (!IsClient() || !pClient || pClient->m_ConnectionState != plNetworkConnectionState::Connecting)
  {
    return;
  }

  plLog::Dev("Received Handshake Reply");

  plNetworkClientConnectingReply msgData;
  msgData.m_bSuccess = event.m_Message.ReadUInt8() != 0;
  msgData.m_Reason = static_cast<plNetworkDisconnectReason::Enum>(event.m_Message.ReadUInt8());
  msgData.m_uiProtocolVersion = event.m_Message.ReadUInt32();
  msgData.m_uiClientID = event.m_Message.ReadUInt32();
  if (event.m_Message.HasReadError())
  {
    return;
  }

  plLog::Dev("HandshakeReply: Result={0}, ClientID={1}",
    msgData.m_bSuccess ? "SUCCESS" : "FAILURE", msgData.m_uiClientID);

  if (!msgData.m_bSuccess || msgData.m_uiProtocolVersion != NETWORK_PROTOCOL_VERSION || msgData.m_uiClientID == 0)
  {
    plLog::Warning("Server rejected connection");
    Stop(msgData.m_bSuccess ? plNetworkDisconnectReason::VersionMismatch : msgData.m_Reason);
    return;
  }

  m_uiLocalClientID = msgData.m_uiClientID;
  m_pLocalClient->m_uiClientID = m_uiLocalClientID;
  m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Connected;
  m_Status = plNetworkConnectionState::Connected;
  plLog::Info("Successfully connected with ClientID={0}", m_uiLocalClientID);
  BroadcastNetworkStateChange();
}

void plNetworkWorldModule::GetConnectedClients(plDynamicArray<plNetworkClient*>& out_clients) const
{
  out_clients.Clear();
  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient != nullptr && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
      out_clients.PushBack(pClient);
  }
}

void plNetworkWorldModule::GetConnectedTargets(plDynamicArray<plNetworkConnection>& out_targets, plUInt32 uiExcludeClientID) const
{
  out_targets.Clear();
  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient != nullptr && pClient->m_ConnectionState == plNetworkConnectionState::Connected && pClient->m_uiClientID != uiExcludeClientID)
      out_targets.PushBack(pClient->m_Connection);
  }
}

plResult plNetworkWorldModule::SendToConnectedClients(const plNetworkMessage& msg, plUInt32 uiExcludeClientID)
{
  if (!m_pPeer || IsClient())
    return PL_FAILURE;

  plHybridArray<plNetworkConnection, 32> targets;
  GetConnectedTargets(targets, uiExcludeClientID);
  m_BandwidthManager.RecordBytesSent(msg.m_uiLength * targets.GetCount());
  return m_pPeer->SendTo(msg, targets.GetArrayPtr());
}

plResult plNetworkWorldModule::SendToClient(const plNetworkMessage& msg, plUInt32 uiClientID)
{
  if (!m_pPeer || IsClient())
    return PL_FAILURE;

  plNetworkClient* pClient = GetClient(uiClientID);
  if (pClient == nullptr || pClient->m_ConnectionState != plNetworkConnectionState::Connected)
    return PL_FAILURE;

  m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
  return m_pPeer->SendTo(msg, pClient->m_Connection);
}


bool plNetworkWorldModule::StartPeer()
{
  PL_PROFILE_SCOPE("Network Start Peer");

  if (m_pPeer)
  {
    plLog::Warning("Network Peer already started");
    return false;
  }

  m_Status = plNetworkConnectionState::Connecting;

  BroadcastNetworkStateChange();

  // Apply role-specific config defaults if not explicitly set
  plNetworkConfig config = m_Config;
  if (m_Role == plNetworkRoleEnum::Client)
  {
    config.m_uiConnectionLimit = 1;
  }
  else if (config.m_sServerAddress.IsEmpty() || config.m_sServerAddress == "127.0.0.1")
  {
    // Server/Host should bind to all interfaces by default
    config.m_sServerAddress = "any";
  }

#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
  // Explicit local test runs must not inherit the host's wildcard bind default.
  if (m_Role != plNetworkRoleEnum::Client && plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-Network.LoopbackOnly", false))
    config.m_sServerAddress = "127.0.0.1";
#endif

  m_pPeer = plNetworkPeer::CreatePeer(config);

  if (!m_pPeer)
  {
    plLog::Error("Failed to create network peer");
    m_Status = plNetworkConnectionState::Offline;
    return false;
  }

  return true;
}

void plNetworkWorldModule::StopPeer()
{
  if (!m_pPeer)
  {
    return;
  }

  PL_PROFILE_SCOPE("Network Stop Peer");
  if (m_Role == plNetworkRoleEnum::Client)
  {
    m_pPeer->Disconnect(m_LastDisconnectReason);
  }
  plNetworkPeer::DestroyPeer(m_pPeer);
  m_pPeer = nullptr;
}

void plNetworkWorldModule::CheckClientTimeouts()
{
  if (!m_pPeer)
    return;

  // Iterate backwards to safely remove while iterating
  for (plInt32 i = static_cast<plInt32>(m_pClients.GetCount()) - 1; i >= 0; --i)
  {
    plNetworkClient* pClient = m_pClients[i];
    if (!pClient)
      continue;

    float secondsSinceActivity = pClient->GetSecondsSinceLastActivity();
    const bool bAdmitted = pClient->m_ConnectionState == plNetworkConnectionState::Connected;
    const bool bExpiredAdmission = !bAdmitted &&
      (plNetworkClock::Now() - pClient->m_AdmissionStartTime).GetSeconds() > plMath::Max(m_Config.m_fHandshakeTimeoutSeconds, 0.1f);
    if (pClient->m_ConnectionState == plNetworkConnectionState::Disconnecting ||
        pClient->m_ConnectionState == plNetworkConnectionState::Disconnected || bExpiredAdmission ||
        (bAdmitted && m_fClientTimeoutSeconds > 0.0f && secondsSinceActivity > m_fClientTimeoutSeconds))
    {
      plLog::Warning("PL Net: Client {0} timed out after {1} seconds of inactivity",
        pClient->m_uiClientID, static_cast<plUInt32>(secondsSinceActivity));

      // Disconnect the client
      pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
      m_pPeer->Disconnect(pClient->m_Connection, plNetworkDisconnectReason::Timeout);

      m_pClients.RemoveAtAndCopy(i);
      RemoveClient(pClient, bAdmitted || pClient->m_bAdmissionNotified);
    }
  }
}

void plNetworkWorldModule::RemoveClient(plNetworkClient* pClient, bool bNotify)
{
  // Everything the session keeps for this client goes with it: its objects, its place in a level load, its records.
  pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
  if (bNotify)
    BroadcastClientDisconnected(pClient);

  if (m_Config.m_bDespawnObjectsOfLeavingClients && !IsClient())
    m_ObjectManager.DespawnObjectsOwnedBy(pClient->m_uiClientID);

  m_LevelManager.OnClientDisconnected(pClient->m_uiClientID);
  m_NetworkTime.OnClientDisconnected(pClient->m_uiClientID);

  pClient->m_ConnectionState = plNetworkConnectionState::Disconnected;
  PL_DEFAULT_DELETE(pClient);
}

//////////////////////////////////////////////////////////////////////////
// Authority Validation
//////////////////////////////////////////////////////////////////////////

bool plNetworkWorldModule::HasAuthority(plNetworkComponent* pComponent, plUInt32 uiClientID) const
{
  if (!pComponent)
    return false;

  // Server and Host: only the server writes. Client: only the owner writes, and the server owns what no client owns, so a
  // host's own objects are written by the host. Both: the server and the owner.
  switch (pComponent->GetNetworkRole())
  {
    case plNetworkRoleEnum::Server:
    case plNetworkRoleEnum::Host:
      return uiClientID == m_uiServerClientID;

    case plNetworkRoleEnum::Client:
      return uiClientID == GetOwnerClientID(pComponent);

    case plNetworkRoleEnum::Both:
      return uiClientID == m_uiServerClientID || uiClientID == GetOwnerClientID(pComponent);

    default:
      return false;
  }
}

plUInt32 plNetworkWorldModule::GetOwnerClientID(const plNetworkComponent* pComponent) const
{
  const plNetworkObjectInfo* pInfo = FindOwningObject(pComponent);
  return pInfo != nullptr ? pInfo->m_uiOwnerClientID : m_uiServerClientID;
}

bool plNetworkWorldModule::HasLocalAuthority(plNetworkComponent* pComponent) const
{
  // A client in its handshake has no client ID yet; its placeholder 0 is the server's.
  if (IsClient() && !IsConnected())
    return false;

  return HasAuthority(pComponent, m_uiLocalClientID);
}

plNetworkComponent* plNetworkWorldModule::FindNetworkComponent(plUInt32 uiComponentNetID) const
{
  plNetworkComponent* pResult = nullptr;
  m_ComponentsByNetID.TryGetValue(uiComponentNetID, pResult);
  return pResult;
}

const plNetworkObjectInfo* plNetworkWorldModule::FindOwningObject(const plNetworkComponent* pComponent) const
{
  if (pComponent == nullptr)
    return nullptr;

  // A spawned component names its object in its NetID.
  if (const plUInt32 uiObjectID = plNetworkObjectManager::GetObjectNetID(pComponent->GetNetID()))
  {
    if (const plNetworkObjectInfo* pInfo = m_ObjectManager.GetObjectInfo(uiObjectID))
      return pInfo;
  }

  // A scene object can be given an owner as a whole, which its children share.
  for (const plGameObject* pObject = pComponent->GetOwner(); pObject; pObject = pObject->GetParent())
  {
    const plUInt32 uiObjectID = m_ObjectManager.GetNetworkID(pObject->GetHandle());
    if (uiObjectID != 0)
      return m_ObjectManager.GetObjectInfo(uiObjectID);
  }
  return nullptr;
}

bool plNetworkWorldModule::ValidateStateUpdate(plUInt32 uiComponentNetID, plUInt32 uiSenderClientID) const
{
  plNetworkComponent* pTarget = FindNetworkComponent(uiComponentNetID);
  if (!pTarget)
    return false;
  // Server always accepts its own updates
  if (uiSenderClientID == m_uiServerClientID && (IsServer() || IsHost()))
  {
    return true;
  }

  // On client, accept all updates from server
  if (IsClient() && uiSenderClientID == m_uiServerClientID)
  {
    return true;
  }

  return HasAuthority(pTarget, uiSenderClientID);
}

void plNetworkWorldModule::RecordLagCompensationSnapshots()
{
  const plTime timestamp = m_NetworkTime.GetNetworkTime();
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    if (!pComponent->GetDynamicRTTI()->IsDerivedFrom<plNetworkedTransformComponent>())
      continue;

    const auto* pTransform = static_cast<const plNetworkedTransformComponent*>(pComponent);
    const plBoundingBox bounds = pComponent->GetOwner()->GetGlobalBounds().GetBox();

    plEntityStateSnapshot snapshot;
    snapshot.m_Timestamp = timestamp;
    snapshot.m_vPosition = pComponent->GetOwner()->GetGlobalPosition();
    snapshot.m_qRotation = pComponent->GetOwner()->GetGlobalRotation();
    snapshot.m_vVelocity = pTransform->GetReplicatedVelocity();
    if (bounds.IsValid())
    {
      snapshot.m_vBoundsMin = bounds.m_vMin;
      snapshot.m_vBoundsMax = bounds.m_vMax;
    }
    else
    {
      snapshot.m_vBoundsMin = snapshot.m_vPosition - plVec3(0.5f);
      snapshot.m_vBoundsMax = snapshot.m_vPosition + plVec3(0.5f);
    }
    m_LagCompensation.RecordSnapshot(pComponent->GetNetID(), snapshot);
  }
}

plUInt32 plNetworkWorldModule::LagCompensatedRaycast(plUInt32 uiShooterClientID, plTime claimedRenderTime,
  const plVec3& vOrigin, const plVec3& vDirection, float fMaxDistance, plVec3& out_vHitPoint) const
{
  if (!IsServer() && !IsHost())
    return 0;

  const plTime now = m_NetworkTime.GetNetworkTime();
  plTime maxRewind = plTime::MakeFromMilliseconds(250.0);
  for (const plNetworkClient* pClient : m_pClients)
  {
    if (pClient != nullptr && pClient->m_uiClientID == uiShooterClientID)
    {
      maxRewind = plMath::Min(maxRewind, plTime::MakeFromMilliseconds(pClient->GetRTT() + 50.0f));
      break;
    }
  }

  const plTime queryTime = plMath::Clamp(claimedRenderTime, now - maxRewind, now);
  return m_LagCompensation.RaycastAtTime(queryTime, vOrigin, vDirection, fMaxDistance, out_vHitPoint);
}

//////////////////////////////////////////////////////////////////////////
// Late-Joiner Synchronization
//////////////////////////////////////////////////////////////////////////

void plNetworkWorldModule::SyncComponentStatesToClient(plNetworkClient* pClient)
{
  if (!pClient || !m_pPeer)
    return;

  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    // Spawned components carry their state in the spawn message.
    if (plNetworkObjectManager::GetObjectNetID(pComponent->GetNetID()) != 0)
      continue;

    // The server holds the latest state of every role, owner uploads included.
    if (pComponent->GetNetworkRole() != plNetworkRoleEnum::Undefined)
      SyncComponentStateToClient(pComponent, pClient);
  }
}

void plNetworkWorldModule::SyncComponentStateToClient(plNetworkComponent* pComponent, plNetworkClient* pClient)
{
  if (!pComponent || !pClient || !m_pPeer)
    return;

  plNetworkMessage msg = m_pPeer->BeginSendMessage();
  WriteReplicationHeader(msg, m_uiServerClientID, pComponent->GetNetID());

  // Let the component serialize its current state
  pComponent->NetworkSerializeForClient(msg, pClient->m_uiClientID, true);

  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  SendToClient(msg, pClient->m_uiClientID).IgnoreResult();
}

//////////////////////////////////////////////////////////////////////////
// Graceful Disconnect
//////////////////////////////////////////////////////////////////////////

void plNetworkWorldModule::SendDisconnect(plNetworkDisconnectReason::Enum reason)
{
  if (!m_pPeer)
    return;

  plNetworkMessage msg = m_pPeer->BeginSendMessage();
  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::Disconnect));
  msg.WriteUInt32(m_uiLocalClientID);
  msg.WriteUInt8(static_cast<plUInt8>(reason));
  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
  m_pPeer->SendToServer(msg).IgnoreResult();

  plLog::Dev("PL Net: Sent disconnect (reason: {})", static_cast<plUInt8>(reason));
}

void plNetworkWorldModule::BroadcastDisconnect(plNetworkDisconnectReason::Enum reason)
{
  if (!m_pPeer || (!IsHost() && !IsServer()))
    return;

  plNetworkMessage msg = m_pPeer->BeginSendMessage();
  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::Disconnect));
  msg.WriteUInt32(m_uiServerClientID);
  msg.WriteUInt8(static_cast<plUInt8>(reason));
  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  SendToConnectedClients(msg).IgnoreResult();

  plLog::Dev("PL Net: Broadcast disconnect to all clients (reason: {})", static_cast<plUInt8>(reason));
}

void plNetworkWorldModule::KickClient(plUInt32 uiClientID, plNetworkDisconnectReason::Enum reason)
{
  if (!m_pPeer || (!IsHost() && !IsServer()))
    return;

  plNetworkClient* pClient = GetClient(uiClientID);
  if (!pClient || pClient->m_ConnectionState == plNetworkConnectionState::Disconnecting ||
      pClient->m_ConnectionState == plNetworkConnectionState::Disconnected)
  {
    plLog::Warning("PL Net: Tried to kick non-existent client {}", uiClientID);
    return;
  }

  plLog::Info("PL Net: Kicking client {} (reason: {})", uiClientID, static_cast<plUInt8>(reason));

  // Send disconnect message to the client
  plNetworkMessage msg = m_pPeer->BeginSendMessage();
  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::Disconnect));
  msg.WriteUInt32(m_uiServerClientID);
  msg.WriteUInt8(static_cast<plUInt8>(reason));
  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
  m_pPeer->SendTo(msg, pClient->m_Connection).IgnoreResult();

  // Disconnect sends what is queued, the reason included, before it closes. Admission state is revoked before
  // any subsequent event; the record is reclaimed after dispatch, not while it is in use.
  pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
  m_pPeer->Disconnect(pClient->m_Connection, reason);
  pClient->m_bReservedForReconnect = false;
}

//////////////////////////////////////////////////////////////////////////
// Rate Limiting - These functions are defined inline in the header
//////////////////////////////////////////////////////////////////////////

bool plNetworkWorldModule::CheckRateLimit(plNetworkClient* pClient)
{
  // The limit protects a server from its clients; a client takes whatever its server sends.
  if (!pClient || m_uiRateLimitPerSecond == 0 || IsClient())
    return true;

  // Reset counter each second
  plTime now = plNetworkClock::Now();
  if ((now - pClient->m_LastRateLimitReset).GetSeconds() >= 1.0)
  {
    pClient->m_fMessagesPerSecond = 0.0f;
    pClient->m_LastRateLimitReset = now;
  }

  pClient->m_fMessagesPerSecond += 1.0f;

  if (pClient->m_fMessagesPerSecond > static_cast<float>(m_uiRateLimitPerSecond))
  {
    pClient->m_uiRateLimitViolations++;
    plLog::Warning("PL Net: Client {} exceeded rate limit ({}/{}), violation {}/{}",
      pClient->m_uiClientID,
      static_cast<plUInt32>(pClient->m_fMessagesPerSecond),
      m_uiRateLimitPerSecond,
      pClient->m_uiRateLimitViolations,
      m_uiMaxRateLimitViolations);

    if (m_uiMaxRateLimitViolations > 0 && 
        pClient->m_uiRateLimitViolations >= m_uiMaxRateLimitViolations)
    {
      plLog::Error("PL Net: Client {} exceeded max rate limit violations, kicking",
        pClient->m_uiClientID);
      KickClient(pClient->m_uiClientID, plNetworkDisconnectReason::RateLimited);
      return false;
    }

    return false; // Message should be dropped
  }

  return true; // Message allowed
}

void plNetworkWorldModule::RecordProtocolViolation(plNetworkClient* pClient, const char* szReason)
{
  if (IsClient())
  {
    plLog::Warning("PL Net: Protocol violation from server: {}", szReason);
    Stop(plNetworkDisconnectReason::Kicked);
    return;
  }
  if (pClient == nullptr || pClient->m_ConnectionState == plNetworkConnectionState::Disconnecting ||
      pClient->m_ConnectionState == plNetworkConnectionState::Disconnected)
  {
    return;
  }

  ++pClient->m_uiProtocolViolations;
  const plTime now = plNetworkClock::Now();
  if ((now - pClient->m_LastProtocolViolationLog).GetSeconds() >= 1.0)
  {
    plLog::Warning("PL Net: Protocol violation from client {} ({}/{}): {}",
      pClient->m_uiClientID, pClient->m_uiProtocolViolations, m_uiMaxProtocolViolations, szReason);
    pClient->m_LastProtocolViolationLog = now;
  }

  if (m_uiMaxProtocolViolations > 0 && pClient->m_uiProtocolViolations >= m_uiMaxProtocolViolations)
  {
    KickClient(pClient->m_uiClientID, plNetworkDisconnectReason::Kicked);
  }
}

//////////////////////////////////////////////////////////////////////////
// Reconnection Support
//////////////////////////////////////////////////////////////////////////

plUInt64 plNetworkWorldModule::GenerateSessionToken()
{
  return m_uiNextSessionToken++;
}

plNetworkClient* plNetworkWorldModule::FindClientBySessionToken(plUInt64 uiSessionToken)
{
  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient && pClient->m_uiSessionToken == uiSessionToken)
    {
      return pClient;
    }
  }
  return nullptr;
}

void plNetworkWorldModule::CleanupExpiredReconnectSlots()
{
  plTime now = plNetworkClock::Now();
  
  for (plUInt32 i = m_pClients.GetCount(); i > 0; --i)
  {
    plNetworkClient* pClient = m_pClients[i - 1];
    if (pClient && pClient->m_bReservedForReconnect && now > pClient->m_ReconnectExpiry)
    {
      plLog::Dev("PL Net: Reconnection window expired for client {}", pClient->m_uiClientID);
      m_pClients.RemoveAtAndSwap(i - 1);
      RemoveClient(pClient, true);
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// Engine Message Handling
//////////////////////////////////////////////////////////////////////////

void plNetworkWorldModule::HandleEngineMessage(plUInt8 messageId, plNetworkMessage& msg, plNetworkConnection sender)
{
  if (messageId == plNetworkMessageIDs::PredictedInput)
  {
    m_PredictionManager.HandlePredictedInput(msg, sender);
    return;
  }

  if (messageId == plNetworkMessageIDs::PredictedStateAck)
  {
    m_PredictionManager.HandlePredictedStateAck(msg);
    return;
  }

  // Save original position (after message ID byte)
  plUInt32 originalPosition = msg.m_uiPosition;
  
  // Forward engine messages to all network objects - they filter by NetID internally
  for (plNetworkObject* pNetObject : m_NetworkObjects)
  {
    // Reset position for each handler so they all see the full message content
    msg.m_uiPosition = originalPosition;
    pNetObject->OnNetworkMessage(msg);
    if (m_bStopRequested)
      return;
  }
}

//////////////////////////////////////////////////////////////////////////
// Message Broadcasting
//////////////////////////////////////////////////////////////////////////

void plNetworkWorldModule::BroadcastToNetworkComponents(plMessage& ref_msg)
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    if (pComponent && pComponent->GetOwner())
    {
      pComponent->GetOwner()->SendMessage(ref_msg);
      if (m_bStopRequested)
        return;
    }
  }
}
