#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkSnapshot.h>

plNetworkInterpolator::plNetworkInterpolator()
{
}

void plNetworkInterpolator::AddSnapshot(const plTransform& transform, plTime networkTime, plTime localTime, plUInt32 sequence)
{
  plNetworkTransformSnapshot snapshot(transform, networkTime, localTime, sequence);
  m_SnapshotBuffer.PushSnapshot(snapshot);
}

void plNetworkInterpolator::AddSnapshot(const plTransform& transform, const plVec3& velocity, plTime networkTime, plTime localTime, plUInt32 sequence)
{
  plNetworkTransformSnapshot snapshot(transform, networkTime, localTime, sequence);
  snapshot.m_vVelocity = velocity;
  m_SnapshotBuffer.PushSnapshot(snapshot);
}

void plNetworkInterpolator::Update(float frameDt)
{
  m_SnapshotBuffer.AdvanceTimeline(frameDt, m_Config);
  m_bUpdatedThisFrame = true;
}

bool plNetworkInterpolator::GetInterpolatedTransform(plTime currentNetworkTime, plTransform& out_transform, plVec3* out_velocity)
{
  if (m_SnapshotBuffer.GetCount() == 0)
    return false;

  // If Update(dt) was not called this frame, auto-advance the timeline
  // using the time delta between consecutive GetInterpolatedTransform calls.
  // This provides backward compatibility with code that doesn't call Update().
  if (!m_bUpdatedThisFrame)
  {
    float dt = 0.0f;
    if (m_bHasLastTime)
    {
      dt = static_cast<float>((currentNetworkTime - m_LastNetworkTime).GetSeconds());
      dt = plMath::Clamp(dt, 0.0f, 0.1f); // Safety cap at 100ms
    }
    m_SnapshotBuffer.AdvanceTimeline(dt, m_Config);
  }
  m_bUpdatedThisFrame = false;
  m_LastNetworkTime = currentNetworkTime;
  m_bHasLastTime = true;

  return m_SnapshotBuffer.SampleAtTimeline(m_Config, out_transform, out_velocity);
}

void plNetworkInterpolator::Reset()
{
  m_SnapshotBuffer.Clear();
  m_bHasLastTime = false;
  m_bUpdatedThisFrame = false;
}

PL_STATICLINK_FILE(NetworkPlugin, NetworkPlugin_Core_Implementation_NetworkSnapshot);
