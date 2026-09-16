#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>

class plWorld;

class PL_NETWORKPLUGIN_DLL plScriptExtensionClass_Networking
{
public:
  static void SetServerAddress(plWorld* pWorld, plStringView sAddress);
  static plString GetServerAddress(plWorld* pWorld);

  static void SetServerPort(plWorld* pWorld, plUInt32 uiPort);
  static plUInt16 GetServerPort(plWorld* pWorld);

  static void SetConnectionLimit(plWorld* pWorld, plUInt32 uiConnectionLimit);
  static plUInt16 GetConnectionLimit(plWorld* pWorld);

  static void SetMessageSize(plWorld* pWorld, plUInt32 uiMessageSize);
  static plUInt16 GetMessageSize(plWorld* pWorld);

  static void SetMessagePoolSize(plWorld* pWorld, plUInt32 uiMessagePoolSize);
  static plUInt16 GetMessagePoolSize(plWorld* pWorld);

  static void SetTickRate(plWorld* pWorld, float fTickRate);
  static float GetTickRate(plWorld* pWorld);

  static void SetGameVersion(plWorld* pWorld, plUInt32 uiGameVersion);

  static bool StartServer(plWorld* pWorld);
  static bool StartClient(plWorld* pWorld);
  static bool StartHost(plWorld* pWorld);
  static void Stop(plWorld* pWorld);
  static void KickClient(plWorld* pWorld, plUInt32 uiClientID);

  static bool IsClient(plWorld* pWorld);
  static bool IsServer(plWorld* pWorld);
  static bool IsHost(plWorld* pWorld);
  static bool IsConnected(plWorld* pWorld);
  static bool IsOffline(plWorld* pWorld);

  static plEnum<plNetworkRoleEnum> GetRole(plWorld* pWorld);
  static plEnum<plNetworkConnectionState> GetStatus(plWorld* pWorld);
  static plUInt32 GetLocalClientID(plWorld* pWorld);
  static plUInt32 GetServerClientID(plWorld* pWorld);

  static double GetSyncedTimeSeconds(plWorld* pWorld);
  static double GetRTTSeconds(plWorld* pWorld);

  static void SetRateLimit(plWorld* pWorld, plUInt32 uiMaxMessagesPerSecond);
  static plUInt32 GetRateLimit(plWorld* pWorld);
  static void SetMaxRateLimitViolations(plWorld* pWorld, plUInt32 uiMaxViolations);

  /// Platform lobby to join through plNetworkSession::JoinSession.
  static void SetLobbyId(plWorld* pWorld, plStringView sLobbyId);
  /// The session's active lobby id, else the configured one.
  static plString GetLobbyId(plWorld* pWorld);
  static void SetCreatePlatformLobby(plWorld* pWorld, bool bCreate);
  /// "ENet" or "Platform".
  static void SetDriverType(plWorld* pWorld, plStringView sDriverType);
  static bool IsJoiningLobby(plWorld* pWorld);
};

PL_DECLARE_REFLECTABLE_TYPE(PL_NETWORKPLUGIN_DLL, plScriptExtensionClass_Networking);