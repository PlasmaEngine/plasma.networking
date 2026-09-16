#include <NetworkPlugin/NetworkPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <NetworkPlugin/Core/Driver/DriverInterface.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Components/NetworkedTransformComponent.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <NetworkPlugin/Core/NetworkObjectManager.h>
#include <NetworkPlugin/Core/NetworkRpc.h>
#include <NetworkPlugin/Core/NetworkBandwidth.h>
#include <NetworkPlugin/Core/NetworkTime.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <Core/GameApplication/GameApplicationBase.h>
#include <Core/GameState/GameStateBase.h>
#include <Core/World/World.h>

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

  plUInt32 ComputeSpawnedComponentNetID(plUInt32 uiObjectNetID, const plNetworkComponent* pComponent)
  {
    plUInt32 uiTypeIndex = 0;
    for (const plComponent* pSibling : pComponent->GetOwner()->GetComponents())
    {
      if (pSibling == pComponent)
        break;
      if (pSibling->GetDynamicRTTI() == pComponent->GetDynamicRTTI())
        ++uiTypeIndex;
    }

    plUInt32 hash = plHashingUtils::CombineHashValues32(uiObjectNetID, plHashingUtils::StringHash(pComponent->GetDynamicRTTI()->GetTypeName()));
    hash = plHashingUtils::CombineHashValues32(hash, uiTypeIndex);
    return hash == 0 ? 1 : hash;
  }
}

plNetworkWorldModule::plNetworkWorldModule(plWorld* pWorld)
  : plWorldModule(pWorld)
{
}

plNetworkWorldModule::~plNetworkWorldModule()
{
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
  PL_PROFILE_SCOPE("Network Start Host");
  Stop();

  m_Role = plNetworkRoleEnum::Host;

  plLog::Dev("Starting Network Host");
  if (!StartPeer())
  {
    m_Status = plNetworkConnectionState::Offline;
    return false;
  }

  if (m_pPeer->Listen().Failed())
  {
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
  BroadcastClientConnected(m_pLocalClient);
  return true;
}

void plNetworkWorldModule::Stop(plNetworkDisconnectReason::Enum reason)
{
  if (m_Status == plNetworkConnectionState::Offline && m_Role == plNetworkRoleEnum::Undefined)
  {
    return;
  }

  PL_PROFILE_SCOPE("Network Stop");
  plLog::Dev("Stopping Network (reason: {})", static_cast<plUInt8>(reason));

  // Broadcast disconnect reason to all connected clients
  if (m_pPeer && m_Role == plNetworkRoleEnum::Host)
  {
    BroadcastDisconnect(reason);
  }
  else if (m_pPeer && m_Role == plNetworkRoleEnum::Client)
  {
    SendDisconnect(reason);
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
  if (m_pLocalClient)
  {
    PL_DEFAULT_DELETE(m_pLocalClient);
    m_pLocalClient = nullptr;
  }

  m_Status = plNetworkConnectionState::Disconnected;
  m_Role = plNetworkRoleEnum::Undefined;
  m_LastUpdate = plTime::Now();
  BroadcastNetworkStateChange();
}

void plNetworkWorldModule::RegisterNetworkComponent(plNetworkComponent* pComponent)
{
  if (pComponent)
  {
    // Check if there's a pending spawn network ID (we're in the middle of spawning a network object)
    plUInt32 pendingID = m_ObjectManager.GetPendingSpawnNetworkID();
    if (pendingID != 0)
    {
      pComponent->SetNetID(ComputeSpawnedComponentNetID(pendingID, pComponent));
    }
    else
    {
      // Check if this component belongs to a network-spawned object
      // If so, use the object's network ID so it matches across server/client
      plGameObjectHandle hOwner = pComponent->GetOwner()->GetHandle();
      plUInt32 networkObjectID = m_ObjectManager.GetNetworkID(hOwner);
      
      if (networkObjectID != 0)
      {
        pComponent->SetNetID(ComputeSpawnedComponentNetID(networkObjectID, pComponent));
      }
      else
      {
        // Scene object - deterministic ID from hierarchical name path + component type.
        plGameObject* pOwner = pComponent->GetOwner();
        const plUInt32 sceneID = ComputeSceneNetID(pComponent);

        for (plNetworkComponent* pExisting : m_NetworkComponents)
        {
          if (pExisting != pComponent && pExisting->GetNetID() == sceneID)
          {
            plLog::Warning(
              "PL Net: Scene NetID collision {} between '{}' and '{}' - replication will alias these objects; give them unique names.",
              sceneID,
              BuildGameObjectPath(pExisting->GetOwner()),
              BuildGameObjectPath(pOwner));
            break;
          }
        }

        pComponent->SetNetID(sceneID);
      }
    }
    
    m_NetworkComponents.PushBack(pComponent);
    
    // If we're already connected, notify the component immediately
    if (m_Status == plNetworkConnectionState::Connected)
    {
      pComponent->OnNetworkStateChanged(m_Status);
    }
  }

  if (m_uiNextNetID > 4000000000)
  {
    plLog::Error("PL Net: Too many networked items in use");
  }
}

void plNetworkWorldModule::RemoveNetworkComponent(plNetworkComponent* pComponent)
{
  if (m_NetworkComponents.Contains(pComponent))
  {
    m_NetworkComponents.RemoveAndSwap(pComponent);
  }
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
  float dt = GetWorld()->GetClock().GetTimeDiff().AsFloatInSeconds();
  plTime timeNow = plTime::Now();
  float minDelta = m_fNetworkTickRate > 0 ? 1.0f / m_fNetworkTickRate : 0.0f;

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
  m_Session.Update();

  if (m_Role == plNetworkRoleEnum::Undefined || !m_pPeer)
  {
    LogInfo();
    return;
  }

  PL_PROFILE_SCOPE("Network Update");

  // Drain incoming events every frame so ENet can flush/receive without tick aliasing.
  plNetworkEvent event;
  while (m_pPeer->PopEvent(event).Succeeded())
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
          }

          for (plNetworkObject* pNetObject : m_NetworkObjects)
          {
            pNetObject->OnClientConnecting(data);
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
          m_pPeer->SendMessages(msg);
        }
        else
        {
          plNetworkClient* pClient = PL_DEFAULT_NEW(plNetworkClient);
          pClient->m_uiClientID = m_uiNextClientID++;
          pClient->m_Connection = event.m_Sender;
          pClient->UpdateActivity(); // Initialize activity timestamp
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
          Stop();
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

          pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
          plLog::Dev("Client Disconnected with ID: {}", event.m_Sender.m_uiConnectionID);
          m_pClients.RemoveAndCopy(pClient);
          BroadcastClientDisconnected(pClient);
          pClient->m_ConnectionState = plNetworkConnectionState::Disconnected;
          PL_DEFAULT_DELETE(pClient);
        }
        break;
      }
      case plNetworkEventTypeEnum::Message:
      {
        plNetworkClient* pClient = GetClient(event.m_Sender);
       if (!pClient && !IsClient())
       {
          plLog::Dev("Unknown Client", event.m_Sender.m_uiConnectionID);
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

        plUInt8 id = *event.m_Message.m_pBuffer;
        plNetworkMessageIDs::Enum msgID = static_cast<plNetworkMessageIDs::Enum>(id);
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
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_ObjectManager.HandleSpawnMessage(event.m_Message);
            break;
          }
          case plNetworkMessageIDs::ObjectDespawn:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_ObjectManager.HandleDespawnMessage(event.m_Message);
            break;
          }
          case plNetworkMessageIDs::ObjectRole:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            m_ObjectManager.HandleOwnershipMessage(event.m_Message);
            break;
          }
          case plNetworkMessageIDs::ObjectRpc:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            plUInt32 senderClientID = pClient ? pClient->m_uiClientID : m_uiServerClientID;
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

            // Validate authority before applying state update
            plUInt32 senderClientID = pClient ? pClient->m_uiClientID : m_uiServerClientID;
            if (!ValidateStateUpdate(msgData.m_uiUUID, senderClientID))
            {
              plLog::Warning("PL Net: Rejected unauthorized state update from client {} for component {}",
                senderClientID, msgData.m_uiUUID);
              break;
            }

            bool bFoundComponent = false;
            plNetworkComponent* pReplicatedComponent = nullptr;
            for (plNetworkComponent* pComponent : m_NetworkComponents)
            {
              if (msgData.m_uiUUID == pComponent->GetNetID())
              {
                bFoundComponent = true;
                pReplicatedComponent = pComponent;
                pComponent->NetworkDeserialize(event.m_Message);
                break; // Found it, no need to continue
              }
            }
            
            if (!bFoundComponent)
            {
              plLog::Warning("PL Net: No component found with NetID {} (total: {})", 
                msgData.m_uiUUID, m_NetworkComponents.GetCount());
            }
              else if ((IsServer() || IsHost()) && pClient != nullptr && pReplicatedComponent != nullptr &&
                      (pReplicatedComponent->GetNetworkRole() == plNetworkRoleEnum::Client ||
                       pReplicatedComponent->GetNetworkRole() == plNetworkRoleEnum::Both))
            {
              // Client-role uploads (e.g. owner-writable blackboard) only arrive at the host — relay to other clients.
              plDynamicArray<plNetworkConnection> connections;
              for (plNetworkClient* pTargetClient : m_pClients)
              {
                if (pTargetClient && pTargetClient->m_ConnectionState == plNetworkConnectionState::Connected &&
                    pTargetClient->m_uiClientID != pClient->m_uiClientID)
                {
                  connections.PushBack(pTargetClient->m_Connection);
                }
              }

              if (connections.GetCount() > 0)
              {
                if (pReplicatedComponent->RequiresPerRecipientSerialization())
                {
                  for (plNetworkClient* pTargetClient : m_pClients)
                  {
                    if (!pTargetClient || pTargetClient->m_ConnectionState != plNetworkConnectionState::Connected ||
                        pTargetClient->m_uiClientID == pClient->m_uiClientID)
                      continue;

                    plNetworkMessage relayMsg = m_pPeer->BeginSendMessage();
                    WriteReplicationHeader(relayMsg, m_uiServerClientID, pReplicatedComponent->GetNetID());
                    pReplicatedComponent->NetworkSerializeForClient(relayMsg, pTargetClient->m_uiClientID, false);
                    relayMsg.SetChannelType(pReplicatedComponent->GetChannelType());
                    m_BandwidthManager.RecordBytesSent(relayMsg.m_uiLength);
                    m_pPeer->SendMessages(relayMsg, pTargetClient->m_Connection);
                  }
                }
                else
                {
                  plNetworkMessage relayMsg = m_pPeer->BeginSendMessage();
                  WriteReplicationHeader(relayMsg, m_uiServerClientID, pReplicatedComponent->GetNetID());
                  pReplicatedComponent->NetworkSerialize(relayMsg);
                  relayMsg.SetChannelType(pReplicatedComponent->GetChannelType());
                  m_BandwidthManager.RecordBytesSent(relayMsg.m_uiLength * connections.GetCount());
                  m_pPeer->SendMessages(relayMsg, connections);
                }
              }
            }

            for (plNetworkObject* pNetObject : m_NetworkObjects)
            {
              if (msgData.m_uiUUID == pNetObject->GetNetID())
              {
                pNetObject->OnNetworkMessage(event.m_Message);
              }
            }

            break;
          }
          case plNetworkMessageIDs::Disconnect:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            const plUInt32 uiSenderClientID = event.m_Message.ReadUInt32();
            plUInt8 reasonByte = event.m_Message.ReadUInt8();
            plNetworkDisconnectReason::Enum reason = static_cast<plNetworkDisconnectReason::Enum>(reasonByte);
            PL_IGNORE_UNUSED(uiSenderClientID);
            
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
              pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
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
                m_pPeer->SendMessages(response, event.m_Sender);
                
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
                m_pPeer->SendMessages(response, event.m_Sender);
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
            event.m_Message.m_uiPosition = 1;
            plString sLevelPath = event.m_Message.ReadString();
            m_LevelManager.HandleLevelLoadMessage(sLevelPath);
            break;
          }
          case plNetworkMessageIDs::LevelLoadComplete:
          {
            event.m_Message.m_uiPosition = 1;
            plUInt32 uiClientID = event.m_Message.ReadUInt32();
            m_LevelManager.HandleLevelLoadComplete(uiClientID);
            break;
          }
          case plNetworkMessageIDs::LevelStart:
          {
            m_LevelManager.HandleLevelStartMessage();
            break;
          }
          case plNetworkMessageIDs::TransformState:
          {
            event.m_Message.m_uiPosition = 1; // Skip the message ID byte
            plUInt32 targetNetID = event.m_Message.ReadUInt32();

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
        if (event.m_Message.HasReadError())
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

  // Dirty replication is gated by network tick rate; transport pump runs every frame.
  const bool bReplicationTick = m_fNetworkTickRate <= 0.0f ||
    (timeNow.AsFloatInSeconds() - m_LastUpdate.AsFloatInSeconds() >= minDelta);

  if (bReplicationTick)
  {
    m_LastUpdate = timeNow;

    if (IsServer() || IsHost())
    {
      RecordLagCompensationSnapshots();
    }

  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    if (pComponent->NeedReplication())
    {
      // Determine if we should send based on our role and the component's network role
      bool bShouldSend = false;
      plNetworkRoleEnum::Enum compRole = pComponent->GetNetworkRole();
      
      if (IsServer() || IsHost())
      {
        // Server/Host sends for Server, Host, or Both roles
        bShouldSend = (compRole == plNetworkRoleEnum::Server || 
                       compRole == plNetworkRoleEnum::Host || 
                       compRole == plNetworkRoleEnum::Both);
      }
      else if (IsClient())
      {
        // Client sends for Client or Both roles
        bShouldSend = (compRole == plNetworkRoleEnum::Client || 
                       compRole == plNetworkRoleEnum::Both);
      }
      
      if (!bShouldSend)
      {
        continue;
      }

      // Check if bandwidth manager allows this update
      if (!m_BandwidthManager.ShouldSendUpdate(pComponent->GetNetID()))
      {
        continue; // Skip this update, will try again next tick
      }

      if (IsClient())
      {
        plNetworkMessage msg = m_pPeer->BeginSendMessage();
        WriteReplicationHeader(msg, m_uiLocalClientID, pComponent->GetNetID());
        pComponent->NetworkSerializeForClient(msg, m_uiServerClientID, false);
        msg.SetChannelType(pComponent->GetChannelType());
        m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
        m_pPeer->SendMessages(msg);
      }
      else
      {
        if (pComponent->RequiresPerRecipientSerialization())
        {
          for (plNetworkClient* pTargetClient : m_pClients)
          {
            if (!pTargetClient || pTargetClient->m_ConnectionState != plNetworkConnectionState::Connected)
              continue;

            plNetworkMessage msg = m_pPeer->BeginSendMessage();
            WriteReplicationHeader(msg, m_uiLocalClientID, pComponent->GetNetID());
            pComponent->NetworkSerializeForClient(msg, pTargetClient->m_uiClientID, false);
            msg.SetChannelType(pComponent->GetChannelType());
            m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
            m_pPeer->SendMessages(msg, pTargetClient->m_Connection);
          }
        }
        else
        {
          plDynamicArray<plNetworkConnection> connections;
          GetTargets(connections);
          if (connections.GetCount() > 0)
          {
            plNetworkMessage msg = m_pPeer->BeginSendMessage();
            WriteReplicationHeader(msg, m_uiLocalClientID, pComponent->GetNetID());
            pComponent->NetworkSerialize(msg);
            msg.SetChannelType(pComponent->GetChannelType());
            m_BandwidthManager.RecordBytesSent(msg.m_uiLength * connections.GetCount());
            m_pPeer->SendMessages(msg, connections);
          }
        }
      }
      
      // Clear the dirty flag after successfully sending
      pComponent->ClearDirty();
      m_BandwidthManager.MarkUpdateSent(pComponent->GetNetID());
    }
  }
  } // bReplicationTick

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

void plNetworkWorldModule::BroadcastNetworkStateChange()
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnNetworkStateChanged(m_Status);
  }

  for (plNetworkObject* pNetObject: m_NetworkObjects)
  {
    pNetObject->OnNetworkStateChanged(m_Status);
  }
}

void plNetworkWorldModule::BroadcastClientConnected(plNetworkClient* pClient)
{
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnClientConnected(pClient);
  }

  for (plNetworkObject* pNetObject : m_NetworkObjects)
  {
    pNetObject->OnClientConnected(pClient);
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

  if (msgData.m_uiProtocolVersion != NETWORK_PROTOCOL_VERSION || msgData.m_uiGameVersion != m_uiGameVersion)
  {
    plLog::Warning("Handshake version mismatch - rejecting client");
    connectionData.m_Result = PL_FAILURE;
  }

  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    pComponent->OnClientConnecting(connectionData);
  }

  for (plNetworkObject* pNetObject : m_NetworkObjects)
  {
    pNetObject->OnClientConnecting(connectionData);
  }

  plLog::Dev("Handshake result after callbacks: {0}", connectionData.m_Result.Succeeded() ? "SUCCESS" : "FAILURE");

  plNetworkClientConnectingReply replyMessage;
  replyMessage.m_bSuccess = connectionData.m_Result.Succeeded();
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
  m_pPeer->SendMessages(msgReply, event.m_Sender);

  if (connectionData.m_Result.Failed())
  {
    plLog::Warning("Client {0} connection failed", pClient->m_uiClientID);
    pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
    m_pPeer->Disconnect(event.m_Sender);
    pClient->m_ConnectionState = plNetworkConnectionState::Disconnected;
  }
  else
  {
    pClient->m_ConnectionState = plNetworkConnectionState::Connected;
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
  if (!IsClient())
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

  if (!msgData.m_bSuccess || msgData.m_uiProtocolVersion != NETWORK_PROTOCOL_VERSION)
  {
    plLog::Warning("Server rejected connection");
    Stop();
    return;
  }

  m_uiLocalClientID = msgData.m_uiClientID;
  m_pLocalClient->m_uiClientID = m_uiLocalClientID;
  m_pLocalClient->m_ConnectionState = plNetworkConnectionState::Connected;
  m_Status = plNetworkConnectionState::Connected;
  plLog::Info("Successfully connected with ClientID={0}", m_uiLocalClientID);
  BroadcastNetworkStateChange();
}

void plNetworkWorldModule::GetTargets(plDynamicArray<plNetworkConnection>& targets)
{
  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      targets.PushBack(pClient->m_Connection);
    }
  }
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
    m_pPeer->Disconnect();
  }
  plNetworkPeer::DestroyPeer(m_pPeer);
  m_pPeer = nullptr;
}

void plNetworkWorldModule::CheckClientTimeouts()
{
  if (!m_pPeer || m_fClientTimeoutSeconds <= 0.0f)
    return;

  // Iterate backwards to safely remove while iterating
  for (plInt32 i = static_cast<plInt32>(m_pClients.GetCount()) - 1; i >= 0; --i)
  {
    plNetworkClient* pClient = m_pClients[i];
    if (!pClient)
      continue;

    // Skip clients that are still connecting
    if (pClient->m_ConnectionState != plNetworkConnectionState::Connected)
      continue;

    float secondsSinceActivity = pClient->GetSecondsSinceLastActivity();
    if (secondsSinceActivity > m_fClientTimeoutSeconds)
    {
      plLog::Warning("PL Net: Client {0} timed out after {1} seconds of inactivity",
        pClient->m_uiClientID, static_cast<plUInt32>(secondsSinceActivity));

      // Disconnect the client
      pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;
      m_pPeer->Disconnect(pClient->m_Connection);

      // Notify and cleanup
      m_pClients.RemoveAtAndCopy(i);
      BroadcastClientDisconnected(pClient);
      pClient->m_ConnectionState = plNetworkConnectionState::Disconnected;
      PL_DEFAULT_DELETE(pClient);
    }
  }
}

//////////////////////////////////////////////////////////////////////////
// Authority Validation
//////////////////////////////////////////////////////////////////////////

bool plNetworkWorldModule::HasAuthority(plNetworkComponent* pComponent, plUInt32 uiClientID) const
{
  if (!pComponent)
    return false;

  // Server always has authority on server-role components
  if (uiClientID == m_uiServerClientID)
  {
    return pComponent->GetNetworkRole() == plNetworkRoleEnum::Server ||
           pComponent->GetNetworkRole() == plNetworkRoleEnum::Host ||
           pComponent->GetNetworkRole() == plNetworkRoleEnum::Both;
  }

  // Check component's network role
  plNetworkRoleEnum::Enum role = pComponent->GetNetworkRole();

  switch (role)
  {
    case plNetworkRoleEnum::Server:
    case plNetworkRoleEnum::Host:
      // Only server can modify server-authoritative components
      return uiClientID == m_uiServerClientID;

    case plNetworkRoleEnum::Client:
      // Check if this client owns this component via ObjectManager
      {
        plUInt32 networkID = pComponent->GetNetID();
        const plNetworkObjectInfo* pInfo = m_ObjectManager.GetObjectInfo(networkID);
        if (pInfo)
        {
          return pInfo->m_uiOwnerClientID == uiClientID;
        }

        const plUInt32 uiObjectNetID = m_ObjectManager.GetNetworkID(pComponent->GetOwner()->GetHandle());
        if (uiObjectNetID != 0)
        {
          pInfo = m_ObjectManager.GetObjectInfo(uiObjectNetID);
          if (pInfo)
          {
            return pInfo->m_uiOwnerClientID == uiClientID;
          }
        }

        // Fallback: any client can modify client-role components
        return true;
      }

    case plNetworkRoleEnum::Both:
      // Both server and owning client can modify
      {
        if (uiClientID == m_uiServerClientID)
          return true;

        plUInt32 networkID = pComponent->GetNetID();
        const plNetworkObjectInfo* pInfo = m_ObjectManager.GetObjectInfo(networkID);
        if (pInfo)
        {
          return pInfo->m_uiOwnerClientID == uiClientID;
        }
        const plUInt32 uiObjectNetID = m_ObjectManager.GetNetworkID(pComponent->GetOwner()->GetHandle());
        if (uiObjectNetID != 0)
        {
          pInfo = m_ObjectManager.GetObjectInfo(uiObjectNetID);
          if (pInfo)
            return pInfo->m_uiOwnerClientID == uiClientID;
        }
        return false;
      }

    default:
      return false;
  }
}

bool plNetworkWorldModule::HasLocalAuthority(plNetworkComponent* pComponent) const
{
  return HasAuthority(pComponent, m_uiLocalClientID);
}

bool plNetworkWorldModule::ValidateStateUpdate(plUInt32 uiComponentNetID, plUInt32 uiSenderClientID) const
{
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

  // Find the component
  for (plNetworkComponent* pComponent : m_NetworkComponents)
  {
    if (pComponent->GetNetID() == uiComponentNetID)
    {
      return HasAuthority(pComponent, uiSenderClientID);
    }
  }

  // Component not found - check object manager
  const plNetworkObjectInfo* pInfo = m_ObjectManager.GetObjectInfo(uiComponentNetID);
  if (pInfo)
  {
    // For network objects, check owner
    return pInfo->m_uiOwnerClientID == uiSenderClientID ||
           uiSenderClientID == m_uiServerClientID;
  }

  // Unknown component - reject by default for security
  plLog::Warning("PL Net: Cannot validate authority for unknown component {}", uiComponentNetID);
  return false;
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
    // Only sync server-authoritative components
    plNetworkRoleEnum::Enum role = pComponent->GetNetworkRole();
    if (role == plNetworkRoleEnum::Server ||
        role == plNetworkRoleEnum::Host ||
        role == plNetworkRoleEnum::Both)
    {
      SyncComponentStateToClient(pComponent, pClient);
    }
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
  m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
  m_pPeer->SendMessages(msg, pClient->m_Connection);
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
  m_pPeer->SendMessages(msg);

  plLog::Dev("PL Net: Sent disconnect (reason: {})", static_cast<plUInt8>(reason));
}

void plNetworkWorldModule::BroadcastDisconnect(plNetworkDisconnectReason::Enum reason)
{
  if (!m_pPeer || (!IsHost() && !IsServer()))
    return;

  for (plNetworkClient* pClient : m_pClients)
  {
    if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      plNetworkMessage msg = m_pPeer->BeginSendMessage();
      msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::Disconnect));
      msg.WriteUInt32(m_uiServerClientID);
      msg.WriteUInt8(static_cast<plUInt8>(reason));
      msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
      m_BandwidthManager.RecordBytesSent(msg.m_uiLength);
      m_pPeer->SendMessages(msg, pClient->m_Connection);
    }
  }

  plLog::Dev("PL Net: Broadcast disconnect to all clients (reason: {})", static_cast<plUInt8>(reason));
}

void plNetworkWorldModule::KickClient(plUInt32 uiClientID, plNetworkDisconnectReason::Enum reason)
{
  if (!m_pPeer || (!IsHost() && !IsServer()))
    return;

  plNetworkClient* pClient = GetClient(uiClientID);
  if (!pClient)
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
  m_pPeer->SendMessages(msg, pClient->m_Connection);

  // Mark for disconnection - will be cleaned up in next update
  pClient->m_ConnectionState = plNetworkConnectionState::Disconnecting;

  // Optionally reserve slot for reconnection if reason allows
  if (reason == plNetworkDisconnectReason::Timeout)
  {
    pClient->m_bReservedForReconnect = true;
    pClient->m_ReconnectExpiry = plTime::Now() + plTime::MakeFromSeconds(m_fReconnectWindowSeconds);
    plLog::Dev("PL Net: Reserved slot for client {} reconnection until {}", 
      uiClientID, (plTime::Now() + plTime::MakeFromSeconds(m_fReconnectWindowSeconds)).GetSeconds());
  }
}

//////////////////////////////////////////////////////////////////////////
// Rate Limiting - These functions are defined inline in the header
//////////////////////////////////////////////////////////////////////////

bool plNetworkWorldModule::CheckRateLimit(plNetworkClient* pClient)
{
  if (!pClient || m_uiRateLimitPerSecond == 0)
    return true; // No rate limiting

  // Reset counter each second
  plTime now = plTime::Now();
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
  if (pClient == nullptr)
  {
    plLog::Warning("PL Net: Protocol violation from server: {}", szReason);
    return;
  }

  ++pClient->m_uiProtocolViolations;
  const plTime now = plTime::Now();
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
  plTime now = plTime::Now();
  
  for (plUInt32 i = m_pClients.GetCount(); i > 0; --i)
  {
    plNetworkClient* pClient = m_pClients[i - 1];
    if (pClient && pClient->m_bReservedForReconnect && now > pClient->m_ReconnectExpiry)
    {
      plLog::Dev("PL Net: Reconnection window expired for client {}", pClient->m_uiClientID);
      BroadcastClientDisconnected(pClient);
      PL_DEFAULT_DELETE(pClient);
      m_pClients.RemoveAtAndSwap(i - 1);
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
    }
  }
}
