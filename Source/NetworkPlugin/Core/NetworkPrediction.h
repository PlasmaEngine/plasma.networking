#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkEvent.h>

class plNetworkPredictedComponent;
class plNetworkWorldModule;
struct plNetworkMessage;

/// Fixed-size input command used by the generic prediction loop. Games typically derive from
/// plNetworkPredictedComponent and interpret movement, look and button bits in SimulateTick().
struct PL_NETWORKPLUGIN_DLL plNetworkPredictionInput
{
  plUInt32 m_uiTick = 0;
  plVec3 m_vMovement = plVec3::MakeZero();
  plVec2 m_vLook = plVec2::MakeZero();
  plUInt32 m_uiButtons = 0;
};

struct PL_NETWORKPLUGIN_DLL plNetworkPredictionState
{
  plUInt32 m_uiTick = 0;
  plVec3 m_vPosition = plVec3::MakeZero();
  plQuat m_qRotation = plQuat::MakeIdentity();
  plVec3 m_vVelocity = plVec3::MakeZero();
};

/// Runs client prediction and authoritative server simulation independently of the network send rate.
class PL_NETWORKPLUGIN_DLL plNetworkPredictionManager
{
public:
  void Initialize(plNetworkWorldModule* pModule);
  void RegisterComponent(plNetworkPredictedComponent* pComponent);
  void UnregisterComponent(plNetworkPredictedComponent* pComponent);

  void Update(float fDeltaTime);
  void HandlePredictedInput(plNetworkMessage& msg, const plNetworkConnection& sender);
  void HandlePredictedStateAck(plNetworkMessage& msg);

  plUInt32 GetCurrentTick() const { return m_uiCurrentTick; }
  static constexpr float GetFixedDeltaTime() { return 1.0f / 60.0f; }

private:
  void Tick();
  void TickOwner(plNetworkPredictedComponent& component);
  void TickServer(plNetworkPredictedComponent& component);
  void SendInputs(plNetworkPredictedComponent& component);
  void SendStateAck(plNetworkPredictedComponent& component, const plNetworkConnection& target, plUInt32 uiTick);
  plNetworkPredictedComponent* FindComponent(plUInt32 uiNetID) const;

  plNetworkWorldModule* m_pModule = nullptr;
  plDynamicArray<plNetworkPredictedComponent*> m_Components;
  float m_fAccumulator = 0.0f;
  plUInt32 m_uiCurrentTick = 0;
};

