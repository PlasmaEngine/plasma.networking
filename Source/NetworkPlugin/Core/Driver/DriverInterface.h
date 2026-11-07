#pragma once

#include <NetworkPlugin/Core/NetworkPeer.h>
#include <NetworkPlugin/NetworkPluginDLL.h>

struct plNetworkConfig;

/// \brief One transport connection owned by one plNetworkPeer.
///
/// Any number of drivers can live in one process (a host and its clients, or several sessions), because none of them
/// share connection state:
/// - A plNetworkWorldModule has at most one peer, a peer owns exactly one driver, and a driver owns one ENet host or one
///   platform transport claim. The peer is created by StartHost/StartClient and destroyed by Stop() or the module's
///   destructor, so it never outlives its world. After Stop() the module can start again on a new driver.
/// - Initalize pairs with Shutdown. Both can be called again safely, and the destructor releases what Shutdown did not.
/// - Only the thread that holds the owning world's write lock uses a driver. The peer registry and the platform
///   transport claims are the only state shared between drivers, and both are mutex guarded.
/// - A host binds the configured port (0 picks a free one). A port another socket holds makes Listen fail; there is no
///   fallback port and no socket sharing. A platform transport belongs to one driver at a time.
/// - Process-wide on purpose: the ENet clock (monotonic, never reset), the serializer's per-type property cache and the
///   net_sim_* / Networking.* CVars, which apply to every peer.
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