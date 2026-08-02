#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkLevelManager.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/Core/NetworkBandwidth.h>

plNetworkLevelManager::plNetworkLevelManager()
{
}

plNetworkLevelManager::~plNetworkLevelManager()
{
  Shutdown();
}

void plNetworkLevelManager::Initialize(plNetworkWorldModule* pNetworkModule)
{
  PL_ASSERT_DEV(pNetworkModule != nullptr, "NetworkWorldModule cannot be null");
  m_pNetworkModule = pNetworkModule;
}

void plNetworkLevelManager::Shutdown()
{
  m_State = plNetworkLevelState::None;
  m_sCurrentLevelPath.Clear();
  m_PendingClients.Clear();
  m_LoadedClients.Clear();
  m_fTimeoutAccumulator = 0.0f;
  m_pNetworkModule = nullptr;
}

void plNetworkLevelManager::Update(float fDeltaSeconds)
{
  if (!m_pNetworkModule)
    return;

  // Only the server tracks timeout
  if (!(m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost()))
    return;

  if (m_State != plNetworkLevelState::Loading)
    return;

  // Check timeout
  if (m_fTimeoutSeconds > 0.0f)
  {
    m_fTimeoutAccumulator += fDeltaSeconds;
    if (m_fTimeoutAccumulator >= m_fTimeoutSeconds)
    {
      plLog::Warning("PL Net LevelManager: Timeout waiting for clients to load level '{}'. {} client(s) still pending.",
        m_sCurrentLevelPath, m_PendingClients.GetCount());

      // Treat timed-out clients as loaded (move remaining to loaded) and proceed
      m_PendingClients.Clear();

      {
        plMsgNetworkAllClientsLoaded msg;
        m_pNetworkModule->BroadcastToNetworkComponents(msg);
      }

      m_State = plNetworkLevelState::Ready;
      BroadcastLevelStart();
    }
  }

  // If no pending clients remain, all loaded
  if (m_State == plNetworkLevelState::Loading && m_PendingClients.IsEmpty())
  {
    plLog::Dev("PL Net LevelManager: All clients loaded level '{}'.", m_sCurrentLevelPath);

    {
      plMsgNetworkAllClientsLoaded msg;
      m_pNetworkModule->BroadcastToNetworkComponents(msg);
    }

    m_State = plNetworkLevelState::Ready;
    BroadcastLevelStart();
  }
}

//////////////////////////////////////////////////////////////////////////
// Server API
//////////////////////////////////////////////////////////////////////////

void plNetworkLevelManager::RequestLevelLoad(const plString& sLevelPath, float fTimeoutSeconds)
{
  if (!m_pNetworkModule || !(m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost()))
  {
    plLog::Warning("PL Net LevelManager: Only server/host can request level load.");
    return;
  }

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
    return;

  m_sCurrentLevelPath = sLevelPath;
  m_fTimeoutSeconds = fTimeoutSeconds;
  m_fTimeoutAccumulator = 0.0f;
  m_State = plNetworkLevelState::Loading;
  m_PendingClients.Clear();
  m_LoadedClients.Clear();

  // Build list of connected clients that need to load
  for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
  {
    if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      m_PendingClients.Insert(pClient->m_uiClientID);
    }
  }

  // Send LevelLoad to all connected clients
  plDynamicArray<plNetworkConnection> targets;
  for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
  {
    if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      targets.PushBack(pClient->m_Connection);
    }
  }

  if (!targets.IsEmpty())
  {
    plNetworkMessage msg = pPeer->BeginSendMessage();
    msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::LevelLoad));
    msg.WriteString(sLevelPath);
    msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
    m_pNetworkModule->GetBandwidthManager()->RecordBytesSent(msg.m_uiLength * targets.GetCount());
    pPeer->SendMessages(msg, targets);
  }

  plLog::Dev("PL Net LevelManager: Requested level load '{}' for {} client(s).",
    sLevelPath, m_PendingClients.GetCount());

  // If host, the host itself also needs to load.
  // Broadcast so the host's game code can load the level.
  if (m_pNetworkModule->IsHost())
  {
    plMsgNetworkLevelLoadRequested msg;
    msg.m_sLevelPath = sLevelPath;
    m_pNetworkModule->BroadcastToNetworkComponents(msg);
  }

  // If no clients need to ack (host-only with no remote clients)
  if (m_PendingClients.IsEmpty())
  {
    {
      plMsgNetworkAllClientsLoaded msg;
      m_pNetworkModule->BroadcastToNetworkComponents(msg);
    }
    m_State = plNetworkLevelState::Ready;
    // LevelStart not broadcast (no remote clients) but notify locally
    {
      plMsgNetworkLevelStart msg;
      m_pNetworkModule->BroadcastToNetworkComponents(msg);
    }
  }
}

void plNetworkLevelManager::HandleLevelLoadComplete(plUInt32 uiClientID)
{
  if (!m_pNetworkModule || !(m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost()))
    return;

  if (m_State != plNetworkLevelState::Loading)
  {
    // Late ack after we already started - this is a late-joiner finishing load
    // Send them LevelStart directly
    SendLevelStartToClient(uiClientID);
    return;
  }

  if (m_PendingClients.Contains(uiClientID))
  {
    m_PendingClients.Remove(uiClientID);
    m_LoadedClients.Insert(uiClientID);
    plLog::Dev("PL Net LevelManager: Client {} finished loading. {} pending.", uiClientID, m_PendingClients.GetCount());
  }
}

void plNetworkLevelManager::SyncLevelToClient(plUInt32 uiClientID)
{
  if (!m_pNetworkModule || !(m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost()))
    return;

  // Only sync if a level is active
  if (m_sCurrentLevelPath.IsEmpty())
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
    return;

  plNetworkClient* pClient = m_pNetworkModule->GetClient(uiClientID);
  if (!pClient)
    return;

  // Send LevelLoad for the current level
  plNetworkMessage msg = pPeer->BeginSendMessage();
  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::LevelLoad));
  msg.WriteString(m_sCurrentLevelPath);
  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  m_pNetworkModule->GetBandwidthManager()->RecordBytesSent(msg.m_uiLength);
  pPeer->SendMessages(msg, pClient->m_Connection);

  // Track this client as pending (will send LevelStart when they ack)
  if (m_State == plNetworkLevelState::Loading)
  {
    m_PendingClients.Insert(uiClientID);
  }
  // If we're already Ready, the HandleLevelLoadComplete will send LevelStart directly

  plLog::Dev("PL Net LevelManager: Syncing level '{}' to late-joiner client {}.", m_sCurrentLevelPath, uiClientID);
}

//////////////////////////////////////////////////////////////////////////
// Client API
//////////////////////////////////////////////////////////////////////////

void plNetworkLevelManager::HandleLevelLoadMessage(const plString& sLevelPath)
{
  m_sCurrentLevelPath = sLevelPath;
  m_State = plNetworkLevelState::Loading;

  plLog::Dev("PL Net LevelManager: Server requested level load '{}'.", sLevelPath);

  if (m_pNetworkModule)
  {
    plMsgNetworkLevelLoadRequested msg;
    msg.m_sLevelPath = sLevelPath;
    m_pNetworkModule->BroadcastToNetworkComponents(msg);
  }
}

void plNetworkLevelManager::NotifyLoadComplete()
{
  if (!m_pNetworkModule || !m_pNetworkModule->IsClient())
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
    return;

  plNetworkMessage msg = pPeer->BeginSendMessage();
  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::LevelLoadComplete));
  msg.WriteUInt32(m_pNetworkModule->GetLocalClientID());
  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  m_pNetworkModule->GetBandwidthManager()->RecordBytesSent(msg.m_uiLength);
  pPeer->SendMessages(msg);

  plLog::Dev("PL Net LevelManager: Sent LevelLoadComplete to server.");
}

void plNetworkLevelManager::HandleLevelStartMessage()
{
  m_State = plNetworkLevelState::Ready;

  plLog::Dev("PL Net LevelManager: Level start received. Level '{}' is ready.", m_sCurrentLevelPath);

  if (m_pNetworkModule)
  {
    plMsgNetworkLevelStart msg;
    m_pNetworkModule->BroadcastToNetworkComponents(msg);
  }
}

//////////////////////////////////////////////////////////////////////////
// State Queries
//////////////////////////////////////////////////////////////////////////

bool plNetworkLevelManager::AreAllClientsLoaded() const
{
  return m_PendingClients.IsEmpty();
}

//////////////////////////////////////////////////////////////////////////
// Internal
//////////////////////////////////////////////////////////////////////////

void plNetworkLevelManager::BroadcastLevelStart()
{
  if (!m_pNetworkModule)
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
    return;

  plDynamicArray<plNetworkConnection> targets;
  for (plNetworkClient* pClient : m_pNetworkModule->GetClients())
  {
    if (pClient && pClient->m_ConnectionState == plNetworkConnectionState::Connected)
    {
      targets.PushBack(pClient->m_Connection);
    }
  }

  if (!targets.IsEmpty())
  {
    plNetworkMessage msg = pPeer->BeginSendMessage();
    msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::LevelStart));
    msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
    m_pNetworkModule->GetBandwidthManager()->RecordBytesSent(msg.m_uiLength * targets.GetCount());
    pPeer->SendMessages(msg, targets);
  }

  plLog::Dev("PL Net LevelManager: Broadcast LevelStart.");

  // Also notify locally on the server/host
  if (m_pNetworkModule)
  {
    plMsgNetworkLevelStart msg;
    m_pNetworkModule->BroadcastToNetworkComponents(msg);
  }
}

void plNetworkLevelManager::SendLevelStartToClient(plUInt32 uiClientID)
{
  if (!m_pNetworkModule)
    return;

  plNetworkPeer* pPeer = m_pNetworkModule->GetPeer();
  if (!pPeer)
    return;

  plNetworkClient* pClient = m_pNetworkModule->GetClient(uiClientID);
  if (!pClient)
    return;

  plNetworkMessage msg = pPeer->BeginSendMessage();
  msg.WriteUInt8(static_cast<plUInt8>(plNetworkMessageIDs::LevelStart));
  msg.SetChannelType(plNetworkChannelTypeEnum::ReliableOrdered);
  m_pNetworkModule->GetBandwidthManager()->RecordBytesSent(msg.m_uiLength);
  pPeer->SendMessages(msg, pClient->m_Connection);

  plLog::Dev("PL Net LevelManager: Sent LevelStart to client {}.", uiClientID);
}
