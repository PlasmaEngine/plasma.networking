#pragma once

#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Containers/Map.h>
#include <GameEngine/PlatformServices/PlatformTypes.h>
#include <NetworkPlugin/Core/Driver/DriverInterface.h>
#include <NetworkPlugin/NetworkPluginDLL.h>

class plPlatformTransport;

/// Rides the platform backend's relay/P2P transport (plPlatformTransport). Remote peers are
/// plPlatformUserIds; each user gets one plNetworkConnection id for the lifetime of the driver
/// (0 = the server on a client, 1.. = clients on a host). m_uiServerPort is ignored.
class PL_NETWORKPLUGIN_DLL plDriverPlatform : public plNetworkDriverInterface
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plDriverPlatform, plNetworkDriverInterface);

public:
  plDriverPlatform();
  virtual ~plDriverPlatform() = default;

  plStringView GetDriverName() const override { return "Platform"; }

  /// Fails when there is no platform services hub or the backend has no transport.
  PL_NODISCARD plResult Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config) override;
  void Shutdown() override;

  PL_NODISCARD plResult Listen() override;

  /// Connects to the user named by m_sServerAddress; see ParseUserAddress for the accepted forms.
  PL_NODISCARD plResult Connect() override;

  void Disconnect() override;
  void Disconnect(const plNetworkConnection& connection) override;

  PL_NODISCARD plResult PopEvent(plNetworkEvent& event) override;

  void Flush() override;

  void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message) override;
  void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target) override;
  void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target) override;

  plNetworkStats GetStats() const override;
  plNetworkStats GetStats(plNetworkConnection target) const override;

  /// Accepted forms: "user:<canonical id>", "<backend>:<native id>", or a bare native id (resolved
  /// against the hub's backend id). Returns an invalid id when nothing matches.
  static plPlatformUserId ParseUserAddress(plStringView sAddress);

  /// "user:<canonical id>" of the signed-in local user; empty without a hub.
  static plString GetLocalUserAddress();

private:
  plUInt32 GetOrAllocateConnection(const plPlatformUserId& user);
  void RemoveConnection(plUInt32 uiConnectionID);
  const plPlatformUserId* FindUser(plUInt32 uiConnectionID) const;
  void SendToUser(const plPlatformUserId& user, plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message);

  plNetworkConfig m_Config;
  plNetworkPeer* m_pNetworkPeer = nullptr;
  plPlatformTransport* m_pTransport = nullptr;
  bool m_bListening = false;
  plPlatformUserId m_ServerUser; ///< Client side: who Connect() dialled.
  plUInt32 m_uiNextConnectionID = 1;
  plMap<plUInt64, plUInt32> m_UserToConnection; ///< plPlatformUserId::m_uiHash -> connection id
  plMap<plUInt32, plPlatformUserId> m_ConnectionToUser;

  plUInt64 m_uiMessagesSent = 0;
  plUInt64 m_uiMessagesReceived = 0;
  plUInt64 m_uiBytesSent = 0;
  plUInt64 m_uiBytesReceived = 0;
};
