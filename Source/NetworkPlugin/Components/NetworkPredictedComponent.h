#pragma once

#include <NetworkPlugin/Components/NetworkComponent.h>
#include <NetworkPlugin/Core/NetworkPrediction.h>

using plNetworkPredictedComponentManager = plComponentManagerSimple<class plNetworkPredictedComponent, plComponentUpdateType::Always>;

/// Base component for owner-predicted, server-authoritative movement.
/// Derive from this component and override GatherInput() and SimulateTick() for game movement.
class PL_NETWORKPLUGIN_DLL plNetworkPredictedComponent : public plNetworkComponent
{
  PL_DECLARE_COMPONENT_TYPE(plNetworkPredictedComponent, plNetworkComponent, plNetworkPredictedComponentManager);

public:
  float m_fHardSnapDistance = 2.0f;
  float m_fVisualSmoothingHalfLife = 0.1f;
  bool m_bShowDebugGhosts = false;

  const plVec3& GetVisualSmoothingOffset() const { return m_vVisualSmoothingOffset; }
  const plNetworkPredictionState& GetLastServerState() const { return m_LastServerState; }

protected:
  void OnSimulationStarted() override;
  void Deinitialize() override;
  void Update() override;

  /// Fill the command for the next local prediction tick.
  virtual void GatherInput(plNetworkPredictionInput& out_input) { PL_IGNORE_UNUSED(out_input); }

  /// Advance gameplay by exactly one 60 Hz tick. The same method runs on owner and server.
  virtual void SimulateTick(const plNetworkPredictionInput& input, float fFixedDeltaTime)
  {
    PL_IGNORE_UNUSED(input);
    PL_IGNORE_UNUSED(fFixedDeltaTime);
  }

  /// Override when the authoritative state is not represented by the owner's global transform.
  virtual void CapturePredictionState(plNetworkPredictionState& out_state) const;
  virtual void RestorePredictionState(const plNetworkPredictionState& state);

  /// Called whenever the smoothing offset changes. Render code can apply this offset to a visual child.
  virtual void ApplyVisualSmoothingOffset(const plVec3& vOffset) { PL_IGNORE_UNUSED(vOffset); }

private:
  friend class plNetworkPredictionManager;

  struct PredictedFrame
  {
    plNetworkPredictionInput m_Input;
    plNetworkPredictionState m_State;
  };

  struct BufferedInput
  {
    plNetworkPredictionInput m_Input;
    plNetworkConnection m_Sender;
  };

  void Reconcile(const plNetworkPredictionState& authoritativeState);
  void PruneHistoryThrough(plUInt32 uiTick);

  plHybridArray<PredictedFrame, 128> m_PredictedFrames;
  plHybridArray<BufferedInput, 32> m_ServerInputBuffer;
  plNetworkPredictionInput m_LastServerInput;
  plNetworkPredictionState m_LastServerState;
  plVec3 m_vVisualSmoothingOffset = plVec3::MakeZero();
  plVec3 m_vPreviousTickPosition = plVec3::MakeZero();
  plUInt32 m_uiLastServerProcessedTick = 0;
  plUInt8 m_uiServerWaitTicks = 0;
  bool m_bHasServerInput = false;
};
