#pragma once

#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/NetworkPluginDLL.h>

struct plNetworkConfig;

class PL_NETWORKPLUGIN_DLL plNetworkDriverInterface
{
public:
  virtual ~plNetworkDriverInterface() = default;

  /// \brief Returns the name of the driver as a string
  virtual plStringView GetDriverName() const = 0;

  PL_NODISCARD virtual plResult Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config) = 0;
  virtual void Shutdown() = 0;

  PL_NODISCARD virtual plResult Listen() = 0;

  PL_NODISCARD virtual plResult Connect() = 0;

  virtual void Disconnect() = 0;

  virtual void Disconnect(const plNetworkConnection& connection) = 0;

  PL_NODISCARD virtual plResult PopEvent(plNetworkEvent& event) = 0;

  /// \brief Flush queued outgoing packets to the wire (e.g. enet_host_flush). Called after each send batch.
  virtual void Flush() {}

  virtual void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message) = 0;
  virtual void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target) = 0;
  virtual void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target) = 0;

  virtual plNetworkStats GetStats() const = 0;
  virtual plNetworkStats GetStats(plNetworkConnection target) const = 0;

};