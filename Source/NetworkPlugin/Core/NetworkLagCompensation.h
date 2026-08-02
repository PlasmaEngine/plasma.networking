#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Math/Quat.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/Map.h>
#include <Core/World/Declarations.h>

/// @brief A snapshot of an entity's state at a specific time
/// Used for lag compensation (server rewind)
struct PL_NETWORKPLUGIN_DLL plEntityStateSnapshot
{
  plTime m_Timestamp;
  plVec3 m_vPosition;
  plQuat m_qRotation;
  plVec3 m_vVelocity;
  plVec3 m_vBoundsMin;  ///< AABB for hit detection
  plVec3 m_vBoundsMax;

  /// @brief Get the center of the bounds
  plVec3 GetBoundsCenter() const { return (m_vBoundsMin + m_vBoundsMax) * 0.5f; }

  /// @brief Get the extents of the bounds
  plVec3 GetBoundsExtents() const { return (m_vBoundsMax - m_vBoundsMin) * 0.5f; }

  /// @brief Check if a point is inside the bounds
  bool ContainsPoint(const plVec3& point) const
  {
    return point.x >= m_vBoundsMin.x && point.x <= m_vBoundsMax.x &&
           point.y >= m_vBoundsMin.y && point.y <= m_vBoundsMax.y &&
           point.z >= m_vBoundsMin.z && point.z <= m_vBoundsMax.z;
  }

  /// @brief Interpolate between two snapshots
  static plEntityStateSnapshot Lerp(const plEntityStateSnapshot& a, const plEntityStateSnapshot& b, float t)
  {
    plEntityStateSnapshot result;
    result.m_Timestamp = plTime::MakeFromSeconds(
      plMath::Lerp(a.m_Timestamp.GetSeconds(), b.m_Timestamp.GetSeconds(), t));
    result.m_vPosition = plMath::Lerp(a.m_vPosition, b.m_vPosition, t);
    result.m_qRotation = plQuat::MakeSlerp(a.m_qRotation, b.m_qRotation, t);
    result.m_vVelocity = plMath::Lerp(a.m_vVelocity, b.m_vVelocity, t);
    result.m_vBoundsMin = plMath::Lerp(a.m_vBoundsMin, b.m_vBoundsMin, t);
    result.m_vBoundsMax = plMath::Lerp(a.m_vBoundsMax, b.m_vBoundsMax, t);
    return result;
  }
};

/// @brief History buffer for a single entity
class PL_NETWORKPLUGIN_DLL plEntityHistory
{
public:
  plEntityHistory() = default;

  /// @brief Set how long to keep history (default 1 second)
  void SetHistoryDuration(float seconds) { m_fHistoryDuration = seconds; }

  /// @brief Add a snapshot to the history
  void AddSnapshot(const plEntityStateSnapshot& snapshot);

  /// @brief Get the state at a specific time (interpolated)
  bool GetStateAtTime(plTime time, plEntityStateSnapshot& outState) const;

  /// @brief Remove old snapshots beyond the history duration
  void PruneOldSnapshots(plTime currentTime);

  /// @brief Clear all history
  void Clear() { m_Snapshots.Clear(); }

  /// @brief Check if we have any history
  bool HasHistory() const { return !m_Snapshots.IsEmpty(); }

  /// @brief Get the oldest timestamp in history
  plTime GetOldestTime() const;

  /// @brief Get the newest timestamp in history
  plTime GetNewestTime() const;

private:
  plDeque<plEntityStateSnapshot> m_Snapshots;
  float m_fHistoryDuration = 1.0f; ///< Seconds of history to keep
};

/// @brief Manages lag compensation for all entities
/// Server-side system that stores position history and can rewind for hit validation
class PL_NETWORKPLUGIN_DLL plNetworkLagCompensation
{
public:
  plNetworkLagCompensation() = default;
  ~plNetworkLagCompensation() = default;

  /// @brief Initialize the lag compensation system
  void Initialize();

  /// @brief Shutdown and clear all history
  void Shutdown();

  /// @brief Update - prune old snapshots
  void Update(plTime currentTime);

  /// @brief Set global history duration for all entities
  void SetHistoryDuration(float seconds) { m_fDefaultHistoryDuration = seconds; }

  /// @brief Get history duration
  float GetHistoryDuration() const { return m_fDefaultHistoryDuration; }

  //////////////////////////////////////////////////////////////////////////
  // Entity Registration
  //////////////////////////////////////////////////////////////////////////

  /// @brief Register an entity for lag compensation
  void RegisterEntity(plUInt32 entityID);

  /// @brief Unregister an entity
  void UnregisterEntity(plUInt32 entityID);

  /// @brief Check if an entity is registered
  bool IsEntityRegistered(plUInt32 entityID) const;

  //////////////////////////////////////////////////////////////////////////
  // Snapshot Recording
  //////////////////////////////////////////////////////////////////////////

  /// @brief Record a snapshot for an entity
  void RecordSnapshot(plUInt32 entityID, const plEntityStateSnapshot& snapshot);

  /// @brief Record a snapshot with individual components
  void RecordSnapshot(plUInt32 entityID, plTime timestamp,
                      const plVec3& position, const plQuat& rotation,
                      const plVec3& velocity = plVec3::MakeZero(),
                      const plVec3& boundsMin = plVec3(-0.5f),
                      const plVec3& boundsMax = plVec3(0.5f));

  //////////////////////////////////////////////////////////////////////////
  // Time Rewind
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get the state of an entity at a specific time
  bool GetEntityStateAtTime(plUInt32 entityID, plTime time, plEntityStateSnapshot& outState) const;

  /// @brief Get states of all registered entities at a specific time
  void GetAllStatesAtTime(plTime time, plMap<plUInt32, plEntityStateSnapshot>& outStates) const;

  //////////////////////////////////////////////////////////////////////////
  // Hit Detection Helpers
  //////////////////////////////////////////////////////////////////////////

  /// @brief Perform a raycast against rewound entity positions
  /// @return Entity ID of first hit, or 0 if no hit
  plUInt32 RaycastAtTime(plTime time, const plVec3& origin, const plVec3& direction, 
                         float maxDistance, plVec3& outHitPoint) const;

  /// @brief Check if a point was inside an entity's bounds at a specific time
  bool PointInEntityAtTime(plUInt32 entityID, plTime time, const plVec3& point) const;

  /// @brief Check if a sphere overlapped an entity at a specific time
  bool SphereOverlapAtTime(plUInt32 entityID, plTime time, const plVec3& center, float radius) const;

  //////////////////////////////////////////////////////////////////////////
  // Statistics
  //////////////////////////////////////////////////////////////////////////

  /// @brief Get the number of registered entities
  plUInt32 GetEntityCount() const { return m_EntityHistories.GetCount(); }

  /// @brief Get the total number of snapshots stored
  plUInt32 GetTotalSnapshotCount() const;

private:
  float m_fDefaultHistoryDuration = 1.0f;
  plMap<plUInt32, plEntityHistory> m_EntityHistories;
};
