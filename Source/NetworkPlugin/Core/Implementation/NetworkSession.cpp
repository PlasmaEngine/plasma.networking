#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkSession.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>

plNetworkSession::plNetworkSession()
{
}

plNetworkSession::~plNetworkSession()
{
  Shutdown();
}

void plNetworkSession::Initialize(plNetworkWorldModule* pNetworkModule)
{
  PL_ASSERT_DEV(pNetworkModule != nullptr, "NetworkWorldModule cannot be null");
  m_pNetworkModule = pNetworkModule;
}

void plNetworkSession::Shutdown()
{
  m_State = plNetworkSessionState::None;
  m_PlayerList.Clear();
  m_ReadyStates.Clear();
  m_pNetworkModule = nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Session Lifecycle
//////////////////////////////////////////////////////////////////////////

bool plNetworkSession::CreateSession(const plNetworkConfig& config)
{
  if (!m_pNetworkModule)
    return false;

  m_pNetworkModule->SetNetworkConfig(config);

  bool bStarted = m_pNetworkModule->StartHost();
  if (!bStarted)
  {
    plLog::Error("PL Net Session: Failed to create session (StartHost failed).");
    return false;
  }

  SetSessionState(plNetworkSessionState::Lobby);

  // Add the host as the first player
  plUInt32 hostClientID = m_pNetworkModule->GetLocalClientID();
  OnClientConnected(hostClientID);

  plLog::Info("PL Net Session: Session created. Host client ID: {}", hostClientID);
  return true;
}

bool plNetworkSession::JoinSession(const plNetworkConfig& config)
{
  if (!m_pNetworkModule)
    return false;

  m_pNetworkModule->SetNetworkConfig(config);

  bool bStarted = m_pNetworkModule->StartClient();
  if (!bStarted)
  {
    plLog::Error("PL Net Session: Failed to join session (StartClient failed).");
    return false;
  }

  // State will be set when we receive session state from the host
  // For now, mark as Lobby (clients assume lobby until told otherwise)
  SetSessionState(plNetworkSessionState::Lobby);

  plLog::Info("PL Net Session: Joining session at {}:{}",
    config.m_sServerAddress, config.m_uiServerPort);
  return true;
}

void plNetworkSession::LeaveSession()
{
  if (!m_pNetworkModule)
    return;

  plLog::Info("PL Net Session: Leaving session.");

  m_pNetworkModule->Stop();

  m_PlayerList.Clear();
  m_ReadyStates.Clear();
  SetSessionState(plNetworkSessionState::None);
}

//////////////////////////////////////////////////////////////////////////
// Session State
//////////////////////////////////////////////////////////////////////////

void plNetworkSession::SetSessionState(plNetworkSessionState::Enum state)
{
  if (m_State == state)
    return;

  plNetworkSessionState::Enum oldState = m_State;
  m_State = state;

  plLog::Dev("PL Net Session: State changed from {} to {}.",
    static_cast<plUInt8>(oldState), static_cast<plUInt8>(state));

  if (m_pNetworkModule)
  {
    plMsgNetworkSessionStateChanged msg;
    msg.m_NewState = static_cast<plUInt8>(state);
    m_pNetworkModule->BroadcastToNetworkComponents(msg);
  }
}

//////////////////////////////////////////////////////////////////////////
// Ready State
//////////////////////////////////////////////////////////////////////////

void plNetworkSession::SetReady(plUInt32 uiClientID, bool bReady)
{
  m_ReadyStates[uiClientID] = bReady;

  plLog::Dev("PL Net Session: Client {} ready state = {}.", uiClientID, bReady);

  // Check if all players are now ready
  if (bReady && AreAllPlayersReady())
  {
    if (m_pNetworkModule)
    {
      plMsgNetworkAllPlayersReady msg;
      m_pNetworkModule->BroadcastToNetworkComponents(msg);
    }
  }
}

void plNetworkSession::SetLocalReady(bool bReady)
{
  if (!m_pNetworkModule)
    return;

  SetReady(m_pNetworkModule->GetLocalClientID(), bReady);
}

bool plNetworkSession::IsPlayerReady(plUInt32 uiClientID) const
{
  bool bReady = false;
  m_ReadyStates.TryGetValue(uiClientID, bReady);
  return bReady;
}

bool plNetworkSession::AreAllPlayersReady() const
{
  if (m_PlayerList.IsEmpty())
    return false;

  for (plUInt32 clientID : m_PlayerList)
  {
    if (!IsPlayerReady(clientID))
      return false;
  }

  return true;
}

bool plNetworkSession::IsSessionLeader() const
{
  if (!m_pNetworkModule)
    return false;

  return m_pNetworkModule->IsServer() || m_pNetworkModule->IsHost();
}

//////////////////////////////////////////////////////////////////////////
// Game Flow
//////////////////////////////////////////////////////////////////////////

bool plNetworkSession::StartGame(const plString& sLevelPath)
{
  if (!IsSessionLeader())
  {
    plLog::Warning("PL Net Session: Only the session leader can start the game.");
    return false;
  }

  if (m_State != plNetworkSessionState::Lobby)
  {
    plLog::Warning("PL Net Session: Cannot start game from state {}. Must be in Lobby.",
      static_cast<plUInt8>(m_State));
    return false;
  }

  SetSessionState(plNetworkSessionState::Loading);

  // If a level path is provided and a level manager is available, trigger level load
  if (!sLevelPath.IsEmpty() && m_pNetworkModule)
  {
    plNetworkLevelManager* pLevelMgr = m_pNetworkModule->GetLevelManager();
    if (pLevelMgr)
    {
      pLevelMgr->RequestLevelLoad(sLevelPath);
    }
  }

  plLog::Info("PL Net Session: Game starting. Loading level: '{}'",
    sLevelPath.IsEmpty() ? "<none>" : sLevelPath.GetData());
  return true;
}

//////////////////////////////////////////////////////////////////////////
// Client tracking
//////////////////////////////////////////////////////////////////////////

void plNetworkSession::OnClientConnected(plUInt32 uiClientID)
{
  if (!m_PlayerList.Contains(uiClientID))
  {
    m_PlayerList.PushBack(uiClientID);
    m_ReadyStates[uiClientID] = false;

    plLog::Dev("PL Net Session: Player {} joined. Total players: {}", uiClientID, m_PlayerList.GetCount());

    if (m_pNetworkModule)
    {
      plMsgNetworkPlayerJoined msg;
      msg.m_uiClientID = uiClientID;
      m_pNetworkModule->BroadcastToNetworkComponents(msg);
    }
  }
}

void plNetworkSession::OnClientDisconnected(plUInt32 uiClientID)
{
  if (m_PlayerList.Contains(uiClientID))
  {
    m_PlayerList.RemoveAndCopy(uiClientID);
    m_ReadyStates.Remove(uiClientID);

    plLog::Dev("PL Net Session: Player {} left. Total players: {}", uiClientID, m_PlayerList.GetCount());

    if (m_pNetworkModule)
    {
      plMsgNetworkPlayerLeft msg;
      msg.m_uiClientID = uiClientID;
      m_pNetworkModule->BroadcastToNetworkComponents(msg);
    }
  }
}
