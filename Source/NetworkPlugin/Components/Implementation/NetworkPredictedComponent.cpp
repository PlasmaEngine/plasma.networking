#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Components/NetworkPredictedComponent.h>
#include <RendererCore/Debug/DebugRenderer.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plNetworkPredictedComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("HardSnapDistance", m_fHardSnapDistance)->AddAttributes(new plDefaultValueAttribute(2.0f), new plClampValueAttribute(0.1f, 100.0f)),
    PL_MEMBER_PROPERTY("VisualSmoothingHalfLife", m_fVisualSmoothingHalfLife)->AddAttributes(new plDefaultValueAttribute(0.1f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("ShowDebugGhosts", m_bShowDebugGhosts)->AddAttributes(new plDefaultValueAttribute(false)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Networking"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_COMPONENT_TYPE
// clang-format on

void plNetworkPredictedComponent::OnSimulationStarted()
{
  plNetworkComponent::OnSimulationStarted();
  m_vPreviousTickPosition = GetOwner()->GetGlobalPosition();

  if (m_pNetworkModule)
  {
    m_pNetworkModule->GetPredictionManager()->RegisterComponent(this);
  }
}

void plNetworkPredictedComponent::Deinitialize()
{
  if (m_pNetworkModule)
  {
    m_pNetworkModule->GetPredictionManager()->UnregisterComponent(this);
  }

  plNetworkComponent::Deinitialize();
}

void plNetworkPredictedComponent::Update()
{
  if (!m_vVisualSmoothingOffset.IsZero())
  {
    const float fDeltaTime = GetWorld()->GetClock().GetTimeDiff().AsFloatInSeconds();
    if (m_fVisualSmoothingHalfLife <= 0.0f)
    {
      m_vVisualSmoothingOffset.SetZero();
    }
    else
    {
      const float fDecay = plMath::Pow(0.5f, fDeltaTime / m_fVisualSmoothingHalfLife);
      m_vVisualSmoothingOffset *= fDecay;
      if (m_vVisualSmoothingOffset.GetLengthSquared() < 0.000001f)
        m_vVisualSmoothingOffset.SetZero();
    }
    ApplyVisualSmoothingOffset(m_vVisualSmoothingOffset);
  }

  if (m_bShowDebugGhosts && m_LastServerState.m_uiTick != 0)
  {
    plDebugRenderer::DrawCross(GetWorld(), m_LastServerState.m_vPosition, 0.25f, plColor::OrangeRed);
    plDebugRenderer::DrawCross(GetWorld(), GetOwner()->GetGlobalPosition(), 0.2f, plColor::LimeGreen);
  }
}

void plNetworkPredictedComponent::CapturePredictionState(plNetworkPredictionState& out_state) const
{
  out_state.m_vPosition = GetOwner()->GetGlobalPosition();
  out_state.m_qRotation = GetOwner()->GetGlobalRotation();
}

void plNetworkPredictedComponent::RestorePredictionState(const plNetworkPredictionState& state)
{
  GetOwner()->SetGlobalPosition(state.m_vPosition);
  GetOwner()->SetGlobalRotation(state.m_qRotation);
  m_vPreviousTickPosition = state.m_vPosition;
}

void plNetworkPredictedComponent::PruneHistoryThrough(plUInt32 uiTick)
{
  while (!m_PredictedFrames.IsEmpty() && m_PredictedFrames[0].m_Input.m_uiTick <= uiTick)
  {
    m_PredictedFrames.RemoveAtAndCopy(0);
  }
}

void plNetworkPredictedComponent::Reconcile(const plNetworkPredictionState& authoritativeState)
{
  m_LastServerState = authoritativeState;

  plUInt32 uiFrameIndex = plInvalidIndex;
  for (plUInt32 i = 0; i < m_PredictedFrames.GetCount(); ++i)
  {
    if (m_PredictedFrames[i].m_Input.m_uiTick == authoritativeState.m_uiTick)
    {
      uiFrameIndex = i;
      break;
    }
  }

  if (uiFrameIndex == plInvalidIndex)
    return;

  const float fError = (m_PredictedFrames[uiFrameIndex].m_State.m_vPosition - authoritativeState.m_vPosition).GetLength();
  if (fError < 0.02f)
  {
    PruneHistoryThrough(authoritativeState.m_uiTick);
    return;
  }

  if (fError >= m_fHardSnapDistance)
  {
    RestorePredictionState(authoritativeState);
    m_PredictedFrames.Clear();
    m_vVisualSmoothingOffset.SetZero();
    ApplyVisualSmoothingOffset(m_vVisualSmoothingOffset);
    return;
  }

  plNetworkPredictionState oldCurrent;
  CapturePredictionState(oldCurrent);
  RestorePredictionState(authoritativeState);

  for (plUInt32 i = uiFrameIndex + 1; i < m_PredictedFrames.GetCount(); ++i)
  {
    SimulateTick(m_PredictedFrames[i].m_Input, plNetworkPredictionManager::GetFixedDeltaTime());
    m_PredictedFrames[i].m_State.m_uiTick = m_PredictedFrames[i].m_Input.m_uiTick;
    CapturePredictionState(m_PredictedFrames[i].m_State);
  }

  plNetworkPredictionState correctedCurrent;
  CapturePredictionState(correctedCurrent);
  m_vVisualSmoothingOffset += oldCurrent.m_vPosition - correctedCurrent.m_vPosition;
  ApplyVisualSmoothingOffset(m_vVisualSmoothingOffset);
  PruneHistoryThrough(authoritativeState.m_uiTick);
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Components_Implementation_NetworkPredictedComponent);
