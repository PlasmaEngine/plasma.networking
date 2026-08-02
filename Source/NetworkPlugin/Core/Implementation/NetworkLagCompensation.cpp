#include "NetworkPlugin/Core/NetworkLagCompensation.h"
#include <Foundation/Logging/Log.h>

//////////////////////////////////////////////////////////////////////////
// plEntityHistory Implementation
//////////////////////////////////////////////////////////////////////////

void plEntityHistory::AddSnapshot(const plEntityStateSnapshot& snapshot)
{
  // Keep snapshots sorted by time (newest at back)
  m_Snapshots.PushBack(snapshot);
}

bool plEntityHistory::GetStateAtTime(plTime time, plEntityStateSnapshot& outState) const
{
  if (m_Snapshots.IsEmpty())
    return false;

  // If time is before our earliest snapshot, return earliest
  if (time <= m_Snapshots[0].m_Timestamp)
  {
    outState = m_Snapshots[0];
    return true;
  }

  // If time is after our latest snapshot, return latest
  if (time >= m_Snapshots.PeekBack().m_Timestamp)
  {
    outState = m_Snapshots.PeekBack();
    return true;
  }

  // Find the two snapshots that bracket the requested time
  for (plUInt32 i = 0; i < m_Snapshots.GetCount() - 1; ++i)
  {
    const plEntityStateSnapshot& before = m_Snapshots[i];
    const plEntityStateSnapshot& after = m_Snapshots[i + 1];

    if (time >= before.m_Timestamp && time <= after.m_Timestamp)
    {
      // Interpolate between the two
      double totalDuration = (after.m_Timestamp - before.m_Timestamp).GetSeconds();
      if (totalDuration <= 0.0001)
      {
        outState = before;
        return true;
      }

      double elapsed = (time - before.m_Timestamp).GetSeconds();
      float t = static_cast<float>(elapsed / totalDuration);
      t = plMath::Clamp(t, 0.0f, 1.0f);

      outState = plEntityStateSnapshot::Lerp(before, after, t);
      return true;
    }
  }

  // Fallback - shouldn't reach here
  outState = m_Snapshots.PeekBack();
  return true;
}

void plEntityHistory::PruneOldSnapshots(plTime currentTime)
{
  plTime cutoffTime = currentTime - plTime::MakeFromSeconds(m_fHistoryDuration);

  while (!m_Snapshots.IsEmpty() && m_Snapshots[0].m_Timestamp < cutoffTime)
  {
    m_Snapshots.PopFront();
  }
}

plTime plEntityHistory::GetOldestTime() const
{
  if (m_Snapshots.IsEmpty())
    return plTime::MakeZero();
  return m_Snapshots[0].m_Timestamp;
}

plTime plEntityHistory::GetNewestTime() const
{
  if (m_Snapshots.IsEmpty())
    return plTime::MakeZero();
  return m_Snapshots.PeekBack().m_Timestamp;
}

//////////////////////////////////////////////////////////////////////////
// plNetworkLagCompensation Implementation
//////////////////////////////////////////////////////////////////////////

void plNetworkLagCompensation::Initialize()
{
  m_EntityHistories.Clear();
  plLog::Dev("PL Net: Lag compensation initialized (history: {}s)", m_fDefaultHistoryDuration);
}

void plNetworkLagCompensation::Shutdown()
{
  m_EntityHistories.Clear();
  plLog::Dev("PL Net: Lag compensation shutdown");
}

void plNetworkLagCompensation::Update(plTime currentTime)
{
  // Prune old snapshots from all entities
  for (auto& pair : m_EntityHistories)
  {
    pair.Value().PruneOldSnapshots(currentTime);
  }
}

void plNetworkLagCompensation::RegisterEntity(plUInt32 entityID)
{
  if (!m_EntityHistories.Contains(entityID))
  {
    plEntityHistory history;
    history.SetHistoryDuration(m_fDefaultHistoryDuration);
    m_EntityHistories[entityID] = history;
    plLog::Dev("PL Net: Registered entity {} for lag compensation", entityID);
  }
}

void plNetworkLagCompensation::UnregisterEntity(plUInt32 entityID)
{
  if (m_EntityHistories.Remove(entityID))
  {
    plLog::Dev("PL Net: Unregistered entity {} from lag compensation", entityID);
  }
}

bool plNetworkLagCompensation::IsEntityRegistered(plUInt32 entityID) const
{
  return m_EntityHistories.Contains(entityID);
}

void plNetworkLagCompensation::RecordSnapshot(plUInt32 entityID, const plEntityStateSnapshot& snapshot)
{
  auto it = m_EntityHistories.Find(entityID);
  if (it.IsValid())
  {
    it.Value().AddSnapshot(snapshot);
  }
  else
  {
    // Auto-register if not already registered
    plEntityHistory history;
    history.SetHistoryDuration(m_fDefaultHistoryDuration);
    history.AddSnapshot(snapshot);
    m_EntityHistories[entityID] = history;
  }
}

void plNetworkLagCompensation::RecordSnapshot(plUInt32 entityID, plTime timestamp,
                                               const plVec3& position, const plQuat& rotation,
                                               const plVec3& velocity,
                                               const plVec3& boundsMin, const plVec3& boundsMax)
{
  plEntityStateSnapshot snapshot;
  snapshot.m_Timestamp = timestamp;
  snapshot.m_vPosition = position;
  snapshot.m_qRotation = rotation;
  snapshot.m_vVelocity = velocity;
  snapshot.m_vBoundsMin = boundsMin + position; // Transform to world space
  snapshot.m_vBoundsMax = boundsMax + position;
  RecordSnapshot(entityID, snapshot);
}

bool plNetworkLagCompensation::GetEntityStateAtTime(plUInt32 entityID, plTime time, plEntityStateSnapshot& outState) const
{
  auto it = m_EntityHistories.Find(entityID);
  if (!it.IsValid())
    return false;

  return it.Value().GetStateAtTime(time, outState);
}

void plNetworkLagCompensation::GetAllStatesAtTime(plTime time, plMap<plUInt32, plEntityStateSnapshot>& outStates) const
{
  outStates.Clear();

  for (auto& pair : m_EntityHistories)
  {
    plEntityStateSnapshot state;
    if (pair.Value().GetStateAtTime(time, state))
    {
      outStates[pair.Key()] = state;
    }
  }
}

plUInt32 plNetworkLagCompensation::RaycastAtTime(plTime time, const plVec3& origin, const plVec3& direction,
                                                  float maxDistance, plVec3& outHitPoint) const
{
  plVec3 normalizedDir = direction.IsZero() ? plVec3::MakeAxisX() : direction.GetNormalized();
  float closestDistance = maxDistance;
  plUInt32 hitEntityID = 0;

  for (auto& pair : m_EntityHistories)
  {
    plEntityStateSnapshot state;
    if (!pair.Value().GetStateAtTime(time, state))
      continue;

    // Simple AABB ray intersection test
    // Using slab method
    plVec3 invDir(
      normalizedDir.x != 0.0f ? 1.0f / normalizedDir.x : 1e30f,
      normalizedDir.y != 0.0f ? 1.0f / normalizedDir.y : 1e30f,
      normalizedDir.z != 0.0f ? 1.0f / normalizedDir.z : 1e30f
    );

    plVec3 t1 = (state.m_vBoundsMin - origin).CompMul(invDir);
    plVec3 t2 = (state.m_vBoundsMax - origin).CompMul(invDir);

    float tmin = plMath::Max(
      plMath::Max(plMath::Min(t1.x, t2.x), plMath::Min(t1.y, t2.y)),
      plMath::Min(t1.z, t2.z)
    );
    float tmax = plMath::Min(
      plMath::Min(plMath::Max(t1.x, t2.x), plMath::Max(t1.y, t2.y)),
      plMath::Max(t1.z, t2.z)
    );

    // If tmax < 0, ray is intersecting AABB but behind us
    // If tmin > tmax, ray doesn't intersect AABB
    if (tmax < 0 || tmin > tmax)
      continue;

    float hitDist = tmin >= 0 ? tmin : tmax;
    if (hitDist >= 0 && hitDist < closestDistance)
    {
      closestDistance = hitDist;
      hitEntityID = pair.Key();
      outHitPoint = origin + normalizedDir * hitDist;
    }
  }

  return hitEntityID;
}

bool plNetworkLagCompensation::PointInEntityAtTime(plUInt32 entityID, plTime time, const plVec3& point) const
{
  plEntityStateSnapshot state;
  if (!GetEntityStateAtTime(entityID, time, state))
    return false;

  return state.ContainsPoint(point);
}

bool plNetworkLagCompensation::SphereOverlapAtTime(plUInt32 entityID, plTime time, const plVec3& center, float radius) const
{
  plEntityStateSnapshot state;
  if (!GetEntityStateAtTime(entityID, time, state))
    return false;

  // Find closest point on AABB to sphere center
  plVec3 closestPoint(
    plMath::Clamp(center.x, state.m_vBoundsMin.x, state.m_vBoundsMax.x),
    plMath::Clamp(center.y, state.m_vBoundsMin.y, state.m_vBoundsMax.y),
    plMath::Clamp(center.z, state.m_vBoundsMin.z, state.m_vBoundsMax.z)
  );

  // Check if distance to closest point is within radius
  float distSq = (closestPoint - center).GetLengthSquared();
  return distSq <= radius * radius;
}

plUInt32 plNetworkLagCompensation::GetTotalSnapshotCount() const
{
  plUInt32 count = 0;
  for (auto& pair : m_EntityHistories)
  {
    // Can't easily get count from plEntityHistory without exposing internals
    // Just estimate based on typical recording rate
    if (pair.Value().HasHistory())
    {
      double duration = (pair.Value().GetNewestTime() - pair.Value().GetOldestTime()).GetSeconds();
      count += static_cast<plUInt32>(duration * 60.0); // Assume 60 snapshots per second
    }
  }
  return count;
}
