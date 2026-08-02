#pragma once

#include <Foundation/Configuration/Singleton.h>
#include "NetworkPlugin/Core/Driver/DriverInterface.h"
#include <NetworkPlugin/NetworkPluginDLL.h>

#define ENET_DLL
#include <enet/enet.h>

class PL_NETWORKPLUGIN_DLL plDriverEnet : public plNetworkDriverInterface
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plDriverEnet, plNetworkDriverInterface);
public:
  plDriverEnet();
  virtual ~plDriverEnet() = default;

  /// \brief Returns the name of the driver as a string
  plStringView GetDriverName() const override { return "ENet"; }

  PL_NODISCARD plResult Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config)  override;
  void Shutdown() override;

  PL_NODISCARD plResult Listen() override;

  PL_NODISCARD plResult Connect() override;

  void Disconnect() override;

  void Disconnect(const plNetworkConnection& connection) override;

  PL_NODISCARD plResult PopEvent(plNetworkEvent& event) override;

  void Flush() override;

  virtual void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message) override;
  virtual void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target) override;
  virtual void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target) override;

  virtual plNetworkStats GetStats() const override;
  virtual plNetworkStats GetStats(plNetworkConnection target) const override;

private:
  bool IsServer() const;
private:
  plNetworkConfig m_Config;
  plNetworkPeer* m_pNetworkPeer = nullptr;
  ENetHost* m_pEnetHost = nullptr;
  ENetPeer* m_pEnetPeer = nullptr;
  plMap<plUInt32, ENetPeer*> m_PeerMap;
};