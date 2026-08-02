#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/NetworkMessages.h>
#include <Foundation/Types/Variant.h>

using plNetworkedEventComponentManager = plComponentManagerSimple<class plNetworkedEventComponent, plComponentUpdateType::Always>;

/// @brief Callback invoked when a networked event is received
/// @param sEventName Name of the event
/// @param param1 First parameter (may be invalid if not provided)
/// @param param2 Second parameter (may be invalid if not provided)
using plNetworkedEventCallback = plDelegate<void(const plString& sEventName, const plVariant& param1, const plVariant& param2)>;

/// @brief A lightweight component for broadcasting named events to all clients.
///
/// Game code calls FireEvent() on the authoritative machine (typically the server).
/// The event is replicated to all clients via a reliable ordered RPC.
/// Register a callback with SetEventReceivedCallback() to handle events on each machine.
///
/// Supported plVariant types for parameters: Bool, Int32, UInt32, Float, Double, String, Vec3.
///
/// Usage:
/// \code
///   // On server:
///   pEventComp->FireEvent("PlayerKilled", plVariant(victimID), plVariant(isHeadshot));
///
///   // On all machines (server + clients):
///   pEventComp->SetEventReceivedCallback([](const plString& name, const plVariant& p1, const plVariant& p2) {
///     if (name == "PlayerKilled") { /* update kill feed */ }
///   });
/// \endcode
class PL_NETWORKPLUGIN_DLL plNetworkedEventComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkedEventComponent, plNetworkComponent, plNetworkedEventComponentManager);

public:
  plNetworkedEventComponent() = default;
  virtual ~plNetworkedEventComponent() = default;

  /// @brief Fire a named event that will be broadcast to all clients (and server).
  /// Call this on the authoritative machine (server/host).
  /// @param sEventName The name of the event
  /// @param param1 Optional first parameter
  /// @param param2 Optional second parameter
  void FireEvent(const plString& sEventName, const plVariant& param1 = plVariant(), const plVariant& param2 = plVariant());

  void FireEventNoParam(plStringView sEventName);
  void FireEventBool(plStringView sEventName, bool bValue);
  void FireEventInt(plStringView sEventName, plInt32 iValue);
  void FireEventUInt(plStringView sEventName, plUInt32 uiValue);
  void FireEventFloat(plStringView sEventName, float fValue);
  void FireEventDouble(plStringView sEventName, double fValue);
  void FireEventString(plStringView sEventName, plStringView sValue);
  void FireEventVec3(plStringView sEventName, const plVec3& vValue);

  /// @brief Set the callback invoked when an event is received on this machine
  void SetEventReceivedCallback(plNetworkedEventCallback callback) { m_EventReceivedCallback = callback; }

  // RPC: executed on all clients and server when an event is fired
  void AllClients_OnEvent(plString sEventName, plInt32 iType1, plString sParam1Raw, plInt32 iType2, plString sParam2Raw);

  // plNetworkComponent interface - no replicated properties, events use RPCs
  virtual void NetworkSerialize(plNetworkMessage& msg) override {}
  virtual void NetworkDeserialize(plNetworkMessage& msg) override {}

protected:
  virtual void OnSimulationStarted() override;

private:
  /// @brief Encode a variant into a type tag + string representation for RPC transport
  static void EncodeVariant(const plVariant& value, plInt32& out_iType, plString& out_sRaw);

  /// @brief Decode a type tag + string representation back into a variant
  static plVariant DecodeVariant(plInt32 iType, const plString& sRaw);

  plEventMessageSender<plMsgNetworkedEvent> m_NetworkedEventSender; // [ event ]
  plNetworkedEventCallback m_EventReceivedCallback;
};
