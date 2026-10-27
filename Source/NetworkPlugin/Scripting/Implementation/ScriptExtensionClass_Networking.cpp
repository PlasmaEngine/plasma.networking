#include <NetworkPlugin/NetworkPluginPCH.h>

#include <Core/Scripting/ScriptAttributes.h>
#include <Core/World/World.h>
#include <NetworkPlugin/Scripting/ScriptExtensionClass_Networking.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>

namespace
{
  plUInt16 ClampToUInt16(plUInt32 uiValue, plUInt16 uiMinimum = 0)
  {
    return static_cast<plUInt16>(plMath::Min<plUInt32>(plMath::Max<plUInt32>(uiValue, uiMinimum), plMath::MaxValue<plUInt16>()));
  }

  plNetworkWorldModule* GetNetworkModule(plWorld* pWorld, bool bCreate)
  {
    if (pWorld == nullptr)
      return nullptr;

    return bCreate ? pWorld->GetOrCreateModule<plNetworkWorldModule>() : pWorld->GetModule<plNetworkWorldModule>();
  }
}

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plScriptExtensionClass_Networking, plNoBase, 1, plRTTINoAllocator)
{
  PL_BEGIN_FUNCTIONS
  {
    PL_SCRIPT_FUNCTION_PROPERTY(SetServerAddress, In, "World", In, "Address"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetServerAddress, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetServerPort, In, "World", In, "Port"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetServerPort, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetConnectionLimit, In, "World", In, "ConnectionLimit"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetConnectionLimit, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetMessageSize, In, "World", In, "MessageSize"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetMessageSize, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetMessagePoolSize, In, "World", In, "MessagePoolSize"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetMessagePoolSize, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetTickRate, In, "World", In, "TickRate"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetTickRate, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetGameVersion, In, "World", In, "GameVersion"),
    PL_SCRIPT_FUNCTION_PROPERTY(StartServer, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(StartClient, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(StartHost, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(Stop, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(KickClient, In, "World", In, "ClientID"),
    PL_SCRIPT_FUNCTION_PROPERTY(IsClient, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(IsServer, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(IsHost, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(IsConnected, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(IsOffline, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetRole, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetStatus, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetLocalClientID, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetServerClientID, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetSyncedTimeSeconds, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetRTTSeconds, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetRateLimit, In, "World", In, "MaxMessagesPerSecond"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetRateLimit, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetMaxRateLimitViolations, In, "World", In, "MaxViolations"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetLobbyId, In, "World", In, "LobbyId"),
    PL_SCRIPT_FUNCTION_PROPERTY(GetLobbyId, In, "World"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetCreatePlatformLobby, In, "World", In, "Create"),
    PL_SCRIPT_FUNCTION_PROPERTY(SetDriverType, In, "World", In, "DriverType"),
    PL_SCRIPT_FUNCTION_PROPERTY(IsJoiningLobby, In, "World"),
  }
  PL_END_FUNCTIONS;
  PL_BEGIN_ATTRIBUTES
  {
    new plScriptExtensionAttribute("Networking"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_STATIC_REFLECTED_TYPE;
// clang-format on

void plScriptExtensionClass_Networking::SetServerAddress(plWorld* pWorld, plStringView sAddress)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_sServerAddress = sAddress;
    pModule->SetNetworkConfig(config);
  }
}

plString plScriptExtensionClass_Networking::GetServerAddress(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetNetworkConfig().m_sServerAddress;
  }

  return plNetworkConfig().m_sServerAddress;
}

void plScriptExtensionClass_Networking::SetServerPort(plWorld* pWorld, plUInt32 uiPort)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_uiServerPort = ClampToUInt16(uiPort);
    pModule->SetNetworkConfig(config);
  }
}

plUInt16 plScriptExtensionClass_Networking::GetServerPort(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetNetworkConfig().m_uiServerPort;
  }

  return plNetworkConfig().m_uiServerPort;
}

void plScriptExtensionClass_Networking::SetConnectionLimit(plWorld* pWorld, plUInt32 uiConnectionLimit)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_uiConnectionLimit = ClampToUInt16(uiConnectionLimit, 1);
    pModule->SetNetworkConfig(config);
  }
}

plUInt16 plScriptExtensionClass_Networking::GetConnectionLimit(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetNetworkConfig().m_uiConnectionLimit;
  }

  return plNetworkConfig().m_uiConnectionLimit;
}

void plScriptExtensionClass_Networking::SetMessageSize(plWorld* pWorld, plUInt32 uiMessageSize)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_uiMessageSize = ClampToUInt16(uiMessageSize, 1);
    pModule->SetNetworkConfig(config);
  }
}

plUInt16 plScriptExtensionClass_Networking::GetMessageSize(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetNetworkConfig().m_uiMessageSize;
  }

  return plNetworkConfig().m_uiMessageSize;
}

void plScriptExtensionClass_Networking::SetMessagePoolSize(plWorld* pWorld, plUInt32 uiMessagePoolSize)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_uiMessagePoolSize = ClampToUInt16(uiMessagePoolSize, 1);
    pModule->SetNetworkConfig(config);
  }
}

plUInt16 plScriptExtensionClass_Networking::GetMessagePoolSize(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetNetworkConfig().m_uiMessagePoolSize;
  }

  return plNetworkConfig().m_uiMessagePoolSize;
}

void plScriptExtensionClass_Networking::SetTickRate(plWorld* pWorld, float fTickRate)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    pModule->SetTickRate(plMath::Max(fTickRate, 0.0f));
  }
}

float plScriptExtensionClass_Networking::GetTickRate(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetTickRate();
  }

  return 0.0f;
}

void plScriptExtensionClass_Networking::SetGameVersion(plWorld* pWorld, plUInt32 uiGameVersion)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    pModule->SetGameVersion(uiGameVersion);
  }
}

bool plScriptExtensionClass_Networking::StartServer(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    return pModule->StartServer();
  }

  return false;
}

bool plScriptExtensionClass_Networking::StartClient(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    return pModule->StartClient();
  }

  return false;
}

bool plScriptExtensionClass_Networking::StartHost(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    return pModule->StartHost();
  }

  return false;
}

void plScriptExtensionClass_Networking::Stop(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    pModule->Stop();
  }
}

void plScriptExtensionClass_Networking::KickClient(plWorld* pWorld, plUInt32 uiClientID)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    pModule->KickClient(uiClientID);
  }
}

bool plScriptExtensionClass_Networking::IsClient(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->IsClient();
  }

  return false;
}

bool plScriptExtensionClass_Networking::IsServer(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->IsServer();
  }

  return false;
}

bool plScriptExtensionClass_Networking::IsHost(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->IsHost();
  }

  return false;
}

bool plScriptExtensionClass_Networking::IsConnected(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->IsConnected();
  }

  return false;
}

bool plScriptExtensionClass_Networking::IsOffline(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->IsOffline();
  }

  return true;
}

plEnum<plNetworkRoleEnum> plScriptExtensionClass_Networking::GetRole(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetRole();
  }

  return plNetworkRoleEnum::Undefined;
}

plEnum<plNetworkConnectionState> plScriptExtensionClass_Networking::GetStatus(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetStatus();
  }

  return plNetworkConnectionState::Offline;
}

plUInt32 plScriptExtensionClass_Networking::GetLocalClientID(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetLocalClientID();
  }

  return 0;
}

plUInt32 plScriptExtensionClass_Networking::GetServerClientID(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetServerClientID();
  }

  return 0;
}

double plScriptExtensionClass_Networking::GetSyncedTimeSeconds(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetSyncedTime().GetSeconds();
  }

  return 0.0;
}

double plScriptExtensionClass_Networking::GetRTTSeconds(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetRTT().GetSeconds();
  }

  return 0.0;
}

void plScriptExtensionClass_Networking::SetRateLimit(plWorld* pWorld, plUInt32 uiMaxMessagesPerSecond)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    pModule->SetRateLimit(uiMaxMessagesPerSecond);
  }
}

plUInt32 plScriptExtensionClass_Networking::GetRateLimit(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetRateLimit();
  }

  return 0;
}

void plScriptExtensionClass_Networking::SetMaxRateLimitViolations(plWorld* pWorld, plUInt32 uiMaxViolations)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    pModule->SetMaxRateLimitViolations(uiMaxViolations);
  }
}

void plScriptExtensionClass_Networking::SetLobbyId(plWorld* pWorld, plStringView sLobbyId)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_sLobbyId = sLobbyId;
    pModule->SetNetworkConfig(config);
  }
}

plString plScriptExtensionClass_Networking::GetLobbyId(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    const plPlatformLobbyId lobbyId = pModule->GetSession()->GetLobbyId();
    if (lobbyId.IsValid())
    {
      return lobbyId.m_sId;
    }

    return pModule->GetNetworkConfig().m_sLobbyId;
  }

  return plString();
}

void plScriptExtensionClass_Networking::SetCreatePlatformLobby(plWorld* pWorld, bool bCreate)
{
  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_bCreatePlatformLobby = bCreate;
    pModule->SetNetworkConfig(config);
  }
}

void plScriptExtensionClass_Networking::SetDriverType(plWorld* pWorld, plStringView sDriverType)
{
  plDriverTypeEnum::Enum driverType;
  if (sDriverType.IsEqual_NoCase("ENet"))
  {
    driverType = plDriverTypeEnum::ENet;
  }
  else if (sDriverType.IsEqual_NoCase("Platform"))
  {
    driverType = plDriverTypeEnum::Platform;
  }
  else
  {
    plLog::Warning("Networking.SetDriverType: unknown driver '{}' (expected 'ENet' or 'Platform')", sDriverType);
    return;
  }

  if (auto pModule = GetNetworkModule(pWorld, true))
  {
    plNetworkConfig config = pModule->GetNetworkConfig();
    config.m_DriverType = driverType;
    pModule->SetNetworkConfig(config);
  }
}

bool plScriptExtensionClass_Networking::IsJoiningLobby(plWorld* pWorld)
{
  if (auto pModule = GetNetworkModule(pWorld, false))
  {
    return pModule->GetSession()->IsJoiningLobby();
  }

  return false;
}
