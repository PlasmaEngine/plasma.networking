#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>

#include <Foundation/Containers/HashSet.h>
#include <Foundation/Containers/HashTable.h>
#include <NetworkPlugin/Core/Driver/DriverInterface.h>
#include <NetworkPlugin/Core/NetworkSimulation.h>

/// \brief Wraps the transport driver of a peer and applies plNetworkSimulator conditions to what it sends and receives.
///
/// Every peer runs through one. With the simulation disabled a message goes straight through, unless messages delayed
/// while it was enabled are still queued; those keep their place in line. Outgoing messages wait in this driver and are
/// handed to the transport in Flush(); incoming ones are copied out of the message pool on arrival and only take a pool
/// message again when they are due, so a long delay never holds the pool.
class PL_NETWORKPLUGIN_DLL plDriverSimulated : public plNetworkDriverInterface
{
public:
  /// \brief Takes ownership of the transport driver.
  explicit plDriverSimulated(plNetworkDriverInterface* pTransport);
  ~plDriverSimulated() override;

  plStringView GetDriverName() const override { return m_pTransport->GetDriverName(); }

  /// \brief Initializes only this wrapper; the transport is initialized before it is wrapped.
  PL_NODISCARD plResult Initalize(plNetworkPeer* pNetworkPeer, const plNetworkConfig& config) override;
  void Shutdown() override;

  PL_NODISCARD plResult Listen() override { return m_pTransport->Listen(); }
  PL_NODISCARD plResult Connect() override { return m_pTransport->Connect(); }

  void Disconnect() override;
  void Disconnect(const plNetworkConnection& connection) override;

  PL_NODISCARD plResult PopEvent(plNetworkEvent& event) override;
  void Flush() override;

  void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message) override;
  void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plNetworkConnection target) override;
  void SendMessages(plEnum<plNetworkChannelTypeEnum> channelType, const plNetworkMessage& message, plDynamicArray<plNetworkConnection> target) override;

  plNetworkStats GetStats() const override { return m_pTransport->GetStats(); }
  plNetworkStats GetStats(plNetworkConnection target) const override { return m_pTransport->GetStats(target); }

  plNetworkDriverInterface* GetTransport() const { return m_pTransport; }
  plNetworkSimulator& GetSimulator() { return m_Simulator; }

  /// \brief Messages waiting in this driver in both directions.
  plUInt32 GetQueuedCount() const { return m_Outgoing.GetCount() + m_Incoming.GetCount(); }

private:
  struct Queued
  {
    plTime m_DeliveryTime;
    plUInt64 m_uiOrder = 0;
    plNetworkEventTypeEnum::Enum m_EventType = plNetworkEventTypeEnum::Message;
    plNetworkChannelTypeEnum::Enum m_Channel = plNetworkChannelTypeEnum::None;
    plNetworkConnection m_Connection;
    bool m_bClientForm = false; ///< Sent with the no-target overload.
    plUInt32 m_uiSequence = 0;  ///< Order of unreliable sequenced messages on their link.
    plDynamicArray<plUInt8> m_Data;
  };

  struct LinkState
  {
    plNetworkSimulator::Link m_Link;
    plUInt32 m_uiNextSequence = 1;
    plUInt32 m_uiLastDeliveredSequence = 0;
  };

  void Enqueue(plDynamicArray<Queued>& ref_queue, Queued&& item);
  void ScheduleOutgoing(plNetworkChannelTypeEnum::Enum channel, const plNetworkMessage& message, plNetworkConnection target, bool bClientForm);
  void ScheduleIncoming(const plNetworkEvent& event);
  void DispatchOutgoing(Queued& item);
  void FlushOutgoingFor(const plNetworkConnection* pConnection);
  bool IsStaleSequenced(plHashTable<plUInt32, LinkState>& ref_links, const Queued& item);

  plNetworkDriverInterface* m_pTransport = nullptr;
  plNetworkPeer* m_pPeer = nullptr;
  plNetworkSimulator m_Simulator;

  plDynamicArray<Queued> m_Outgoing; ///< Sorted by delivery time, then by the order messages were queued.
  plDynamicArray<Queued> m_Incoming;
  plHashTable<plUInt32, LinkState> m_OutgoingLinks;
  plHashTable<plUInt32, LinkState> m_IncomingLinks;

  /// \brief Connections the transport reported as open. A message for any other is dropped, as the transport would.
  plHashSet<plUInt32> m_OpenConnections;
  plUInt64 m_uiNextOrder = 0;
};
