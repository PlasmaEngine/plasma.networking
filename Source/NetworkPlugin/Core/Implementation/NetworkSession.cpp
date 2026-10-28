#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/Driver/platform/DriverPlatform.h>
#include <NetworkPlugin/Core/NetworkLevelManager.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <NetworkPlugin/Core/NetworkSession.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>

#include <Foundation/Utilities/ConversionUtils.h>
#include <GameEngine/PlatformServices/PlatformServices.h>

namespace
{
  constexpr const char* s_szLobbyAddressKey = "session_address";
  constexpr const char* s_szLobbyDriverKey = "session_driver";

  plStringView DriverName(plDriverTypeEnum::Enum driverType)
  {
    return driverType == plDriverTypeEnum::Platform ? "Platform" : "ENet";
  }
} // namespace

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
  m_bJoiningLobby = false;
  m_bPendingClientStart = false;
  m_uiLobbyOperationId = 0;
  LeavePlatformLobby();

  if (m_pLiveness != nullptr)
  {
    m_pLiveness->m_bAlive = false;
    m_pLiveness = nullptr;
  }

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

  if (config.m_bCreatePlatformLobby)
  {
    CreatePlatformLobby(config);
  }

  return true;
}

bool plNetworkSession::JoinSession(const plNetworkConfig& config)
{
  if (!m_pNetworkModule)
    return false;

  if (!config.m_sLobbyId.IsEmpty())
  {
    return JoinViaLobby(config);
  }

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

  m_bJoiningLobby = false;
  m_bPendingClientStart = false;
  m_uiLobbyOperationId = 0;
  LeavePlatformLobby();
}

void plNetworkSession::Update()
{
  if (!m_bPendingClientStart || !m_pNetworkModule)
    return;

  // Runs inside the world module's update, so StartClient holds the world write lock.
  m_bPendingClientStart = false;
  m_bJoiningLobby = false;

  m_pNetworkModule->SetNetworkConfig(m_PendingConfig);

  if (!m_pNetworkModule->StartClient())
  {
    plLog::Error("PL Net Session: Failed to join session from lobby '{}' (StartClient failed).", m_LobbyId.m_sId);
    LeavePlatformLobby();
    SetSessionState(plNetworkSessionState::None);
    return;
  }

  SetSessionState(plNetworkSessionState::Lobby);

  plLog::Info("PL Net Session: Joining session from lobby '{}' at '{}' via {}.", m_LobbyId.m_sId, m_PendingConfig.m_sServerAddress, DriverName(m_PendingConfig.m_DriverType));
}

//////////////////////////////////////////////////////////////////////////
// Platform lobbies
//////////////////////////////////////////////////////////////////////////

bool plNetworkSession::JoinViaLobby(const plNetworkConfig& config)
{
  plPlatformServices* pHub = plPlatformServices::GetInstance();
  if (pHub == nullptr || !pHub->Lobbies().IsSupported())
  {
    plLog::Error("PL Net Session: Cannot join lobby '{}' - no platform backend with lobby support.", config.m_sLobbyId);
    return false;
  }

  if (m_bJoiningLobby)
  {
    plLog::Warning("PL Net Session: A lobby join is already pending.");
    return false;
  }

  m_PendingConfig = config;
  m_bJoiningLobby = true;
  m_bPendingClientStart = false;
  SetSessionState(plNetworkSessionState::Lobby);

  plPlatformLobbyId lobbyId;
  lobbyId.m_sId = config.m_sLobbyId;

  plSharedPtr<Liveness> pLiveness = GetLiveness();
  plPlatformOperationPtr pOp = pHub->Lobbies().Join(lobbyId, [this, pLiveness](const plPlatformOperation& op) {
    if (pLiveness->m_bAlive)
    {
      OnLobbyJoined(op);
    }
  });
  m_uiLobbyOperationId = pOp != nullptr ? pOp->GetId() : 0;

  plLog::Info("PL Net Session: Joining platform lobby '{}'...", config.m_sLobbyId);

  return true;
}

void plNetworkSession::CreatePlatformLobby(const plNetworkConfig& config)
{
  plPlatformServices* pHub = plPlatformServices::GetInstance();
  if (pHub == nullptr || !pHub->Lobbies().IsSupported())
  {
    plLog::Warning("PL Net Session: CreatePlatformLobby requested but no platform backend with lobby support is active.");
    return;
  }

  plPlatformLobbyDesc desc;
  desc.m_uiMaxMembers = config.m_uiLobbyMaxMembers != 0 ? config.m_uiLobbyMaxMembers : config.m_uiConnectionLimit;
  desc.m_Visibility = config.m_bLobbyPublic ? plPlatformLobbyVisibility::Public : plPlatformLobbyVisibility::Private;

  plSharedPtr<Liveness> pLiveness = GetLiveness();
  plPlatformOperationPtr pOp = pHub->Lobbies().Create(desc, [this, pLiveness](const plPlatformOperation& op) {
    if (pLiveness->m_bAlive)
    {
      OnLobbyCreated(op);
    }
  });
  m_uiLobbyOperationId = pOp != nullptr ? pOp->GetId() : 0;

  plLog::Info("PL Net Session: Creating platform lobby ({} members, {})...", desc.m_uiMaxMembers, config.m_bLobbyPublic ? "public" : "private");
}

void plNetworkSession::OnLobbyCreated(const plPlatformOperation& op)
{
  // Delivered from plPlatformServices::Update(), outside the world update: only hub and session-local state here.
  plPlatformServices* pHub = plPlatformServices::GetInstance();
  const bool bStale = op.GetId() != m_uiLobbyOperationId;

  if (!bStale)
  {
    m_uiLobbyOperationId = 0;
  }

  if (!op.Succeeded())
  {
    plLog::Error("PL Net Session: Platform lobby creation failed: {}", plPlatformErrorName(op.GetError()));
    return;
  }

  const plPlatformLobbyId& lobbyId = op.GetResult().m_Lobby.m_Id;

  // The session ended (or was replaced) before the lobby existed: do not keep it.
  if (bStale || m_State == plNetworkSessionState::None || !IsSessionLeader())
  {
    if (pHub != nullptr && pHub->Lobbies().GetCurrent() == lobbyId)
    {
      pHub->Lobbies().Leave();
    }
    return;
  }

  m_LobbyId = lobbyId;
  m_bInPlatformLobby = true;

  if (pHub != nullptr)
  {
    const plString sAddress = BuildLocalSessionAddress();
    const plDriverTypeEnum::Enum driverType = m_pNetworkModule && m_pNetworkModule->GetPeer() ? m_pNetworkModule->GetPeer()->m_Config.m_DriverType : plDriverTypeEnum::ENet;

    pHub->Lobbies().SetAttribute(s_szLobbyAddressKey, sAddress);
    pHub->Lobbies().SetAttribute(s_szLobbyDriverKey, DriverName(driverType));

    plLog::Info("PL Net Session: Platform lobby '{}' created, advertising '{}' via {}.", m_LobbyId.m_sId, sAddress, DriverName(driverType));
  }
}

void plNetworkSession::OnLobbyJoined(const plPlatformOperation& op)
{
  // Delivered from plPlatformServices::Update(), outside the world update: StartClient is deferred to Update().
  plPlatformServices* pHub = plPlatformServices::GetInstance();
  const bool bStale = op.GetId() != m_uiLobbyOperationId;

  if (!bStale)
  {
    m_uiLobbyOperationId = 0;
  }

  if (bStale || !m_bJoiningLobby)
  {
    // LeaveSession ran while the join was pending; undo a join that still went through.
    if (op.Succeeded() && pHub != nullptr && pHub->Lobbies().GetCurrent() == op.GetResult().m_Lobby.m_Id)
    {
      pHub->Lobbies().Leave();
    }
    return;
  }

  if (!op.Succeeded())
  {
    plLog::Error("PL Net Session: Joining platform lobby '{}' failed: {}", m_PendingConfig.m_sLobbyId, plPlatformErrorName(op.GetError()));
    m_bJoiningLobby = false;
    SetSessionState(plNetworkSessionState::None);
    return;
  }

  const plPlatformLobbyInfo& lobby = op.GetResult().m_Lobby;

  m_LobbyId = lobby.m_Id;
  m_bInPlatformLobby = true;

  const plStringView sAddress = lobby.GetAttribute(s_szLobbyAddressKey);
  const plStringView sDriver = lobby.GetAttribute(s_szLobbyDriverKey);

  if (sAddress.IsEmpty())
  {
    plLog::Error("PL Net Session: Lobby '{}' carries no '{}' attribute - the host has not published a session yet.", m_LobbyId.m_sId, s_szLobbyAddressKey);
    m_bJoiningLobby = false;
    LeavePlatformLobby();
    SetSessionState(plNetworkSessionState::None);
    return;
  }

  if (sDriver.IsEqual_NoCase("Platform"))
  {
    m_PendingConfig.m_DriverType = plDriverTypeEnum::Platform;
    m_PendingConfig.m_sServerAddress = sAddress;
  }
  else
  {
    m_PendingConfig.m_DriverType = plDriverTypeEnum::ENet;

    // "<host>:<port>"; a missing or unparsable port keeps the configured one.
    const char* szPortSeparator = sAddress.FindLastSubString(":");
    if (szPortSeparator != nullptr)
    {
      const plStringView sHost(sAddress.GetStartPointer(), szPortSeparator);
      const plStringView sPort(szPortSeparator + 1, sAddress.GetEndPointer());
      plUInt32 uiPort = 0;

      if (!sHost.IsEmpty() && plConversionUtils::StringToUInt(sPort, uiPort).Succeeded() && uiPort <= plMath::MaxValue<plUInt16>())
      {
        m_PendingConfig.m_sServerAddress = sHost;
        m_PendingConfig.m_uiServerPort = static_cast<plUInt16>(uiPort);
      }
      else
      {
        m_PendingConfig.m_sServerAddress = sAddress;
      }
    }
    else
    {
      m_PendingConfig.m_sServerAddress = sAddress;
    }
  }

  m_bPendingClientStart = true;
  plLog::Dev("PL Net Session: Lobby '{}' resolved to '{}' ({}); client starts on the next update.", m_LobbyId.m_sId, sAddress, sDriver);
}

void plNetworkSession::LeavePlatformLobby()
{
  if (!m_bInPlatformLobby)
  {
    m_LobbyId = plPlatformLobbyId();
    return;
  }

  m_bInPlatformLobby = false;

  plPlatformServices* pHub = plPlatformServices::GetInstance();
  if (pHub != nullptr && pHub->Lobbies().IsSupported() && pHub->Lobbies().GetCurrent() == m_LobbyId)
  {
    plLog::Info("PL Net Session: Leaving platform lobby '{}'.", m_LobbyId.m_sId);
    pHub->Lobbies().Leave();
  }

  m_LobbyId = plPlatformLobbyId();
}

plString plNetworkSession::BuildLocalSessionAddress() const
{
  const plNetworkPeer* pPeer = m_pNetworkModule ? m_pNetworkModule->GetPeer() : nullptr;
  if (pPeer != nullptr && pPeer->m_Config.m_DriverType == plDriverTypeEnum::Platform)
  {
    return plDriverPlatform::GetLocalUserAddress();
  }

  // The configured bind address as given; "any"/"127.0.0.1" is not reachable by remote clients.
  if (m_pNetworkModule == nullptr)
    return plString();

  const plNetworkConfig& config = m_pNetworkModule->GetNetworkConfig();
  plStringBuilder sAddress;
  sAddress.SetFormat("{}:{}", config.m_sServerAddress, config.m_uiServerPort);
  return sAddress;
}

plSharedPtr<plNetworkSession::Liveness> plNetworkSession::GetLiveness()
{
  if (m_pLiveness == nullptr)
  {
    m_pLiveness = PL_DEFAULT_NEW(Liveness);
  }

  return m_pLiveness;
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
