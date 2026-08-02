#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTime.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Math/Quat.h>

/// @brief A single transform snapshot with timestamp for interpolation
struct PL_NETWORKPLUGIN_DLL plNetworkTransformSnapshot
{
  plTransform m_Transform;        ///< The transform state
  plVec3 m_vVelocity;             ///< Linear velocity at snapshot time
  plVec3 m_vAngularVelocity;      ///< Angular velocity at snapshot time
  plNetworkTimestamp m_Timestamp; ///< When this snapshot was created/received

  plNetworkTransformSnapshot()
    : m_vVelocity(plVec3::MakeZero())
    , m_vAngularVelocity(plVec3::MakeZero())
  {
    m_Transform.SetIdentity();
  }

  plNetworkTransformSnapshot(const plTransform& transform, plTime networkTime, plTime localTime, plUInt32 sequence)
    : m_Transform(transform)
    , m_vVelocity(plVec3::MakeZero())
    , m_vAngularVelocity(plVec3::MakeZero())
    , m_Timestamp(networkTime, localTime, sequence)
  {
  }
};

/// @brief Configuration for snapshot interpolation with adaptive-rate playback.
///
/// Instead of extrapolating when data is late or using complex curve interpolation,
/// this system controls the playback speed of a remote timeline:
///   - When the buffer has more data than needed: speed up slightly (consume excess)
///   - When the buffer is thin: slow down slightly (buy time for next packet)
/// This produces extremely smooth, consistent-speed movement with no overshoot.
struct PL_NETWORKPLUGIN_DLL plSnapshotInterpolationConfig
{
  /// @brief Target interpolation delay (how far behind the newest snapshot to render).
  /// Higher = smoother but more latency, Lower = responsive but may stutter.
  /// Should be at least 2x the send interval (e.g. 100ms for 20Hz sends).
  plTime m_InterpolationDelay = plTime::MakeFromMilliseconds(100.0);

  /// @brief Maximum playback speed deviation from 1.0x (default 5%).
  /// Higher values absorb more jitter but may produce barely perceptible speed changes.
  /// At 0.05 (5%), a character moving at 3 m/s would vary between 2.85 and 3.15 m/s.
  float m_fMaxPlaybackDrift = 0.05f;

  /// @brief How aggressively the playback rate adapts to buffer changes (per second).
  /// Higher = faster adaptation, lower = smoother rate transitions.
  float m_fPlaybackAdaptSpeed = 2.0f;

  /// @brief Teleport threshold - if position jumps more than this, snap instead of interpolate
  float m_fTeleportThreshold = 5.0f;
};

/// @brief Ring buffer of snapshots for a single networked entity.
/// Provides smooth interpolation via adaptive-rate linear playback.
///
/// Rather than picking a render time from network time minus delay, this buffer
/// maintains its own continuously advancing timeline. The playback rate adjusts
/// slightly based on buffer fullness to absorb jitter without extrapolation.
template <plUInt32 BufferSize = 16>
class plSnapshotBuffer
{
public:
  plSnapshotBuffer()
    : m_uiHead(0)
    , m_uiCount(0)
    , m_fPlaybackRate(1.0f)
    , m_bTimelineInitialized(false)
    , m_LastJitter(plTime::MakeZero())
  {
  }

  /// @brief Add a new snapshot to the buffer
  void PushSnapshot(const plNetworkTransformSnapshot& snapshot)
  {
    // Calculate jitter from timing variance
    if (m_uiCount > 0)
    {
      const plNetworkTransformSnapshot& lastSnapshot = GetNewestSnapshot();
      plTime expectedInterval = snapshot.m_Timestamp.m_NetworkTime - lastSnapshot.m_Timestamp.m_NetworkTime;
      plTime actualInterval = snapshot.m_Timestamp.m_LocalTime - lastSnapshot.m_Timestamp.m_LocalTime;
      m_LastJitter = plTime::MakeFromSeconds(plMath::Abs((actualInterval - expectedInterval).GetSeconds()));
    }

    m_Snapshots[m_uiHead] = snapshot;
    m_uiHead = (m_uiHead + 1) % BufferSize;
    if (m_uiCount < BufferSize)
    {
      m_uiCount++;
    }
  }

  /// @brief Clear all snapshots and reset timeline
  void Clear()
  {
    m_uiCount = 0;
    m_uiHead = 0;
    m_bTimelineInitialized = false;
    m_fPlaybackRate = 1.0f;
  }

  /// @brief Get the number of snapshots in the buffer
  plUInt32 GetCount() const { return m_uiCount; }

  /// @brief Check if buffer has enough snapshots for interpolation
  bool HasEnoughSnapshots() const { return m_uiCount >= 2; }

  /// @brief Get the newest snapshot
  const plNetworkTransformSnapshot& GetNewestSnapshot() const
  {
    PL_ASSERT_DEV(m_uiCount > 0, "No snapshots available");
    plUInt32 index = (m_uiHead + BufferSize - 1) % BufferSize;
    return m_Snapshots[index];
  }

  /// @brief Get the oldest snapshot
  const plNetworkTransformSnapshot& GetOldestSnapshot() const
  {
    PL_ASSERT_DEV(m_uiCount > 0, "No snapshots available");
    plUInt32 index = (m_uiHead + BufferSize - m_uiCount) % BufferSize;
    return m_Snapshots[index];
  }

  /// @brief Get a snapshot by index (0 = oldest)
  const plNetworkTransformSnapshot& GetSnapshot(plUInt32 index) const
  {
    PL_ASSERT_DEV(index < m_uiCount, "Snapshot index out of range");
    plUInt32 bufferIndex = (m_uiHead + BufferSize - m_uiCount + index) % BufferSize;
    return m_Snapshots[bufferIndex];
  }

  /// @brief Advance the playback timeline by one frame.
  ///
  /// Adjusts the playback rate based on how much data is buffered ahead of
  /// the current timeline position relative to the target delay. This keeps
  /// the timeline advancing at near-1.0x speed while absorbing jitter.
  void AdvanceTimeline(float frameDt, const plSnapshotInterpolationConfig& config)
  {
    if (m_uiCount == 0)
      return;

    // Initialize the timeline once we have at least 2 snapshots to interpolate
    // between. Start at the oldest snapshot's time so the timeline is always
    // within actual data (not before it, which would cause snap-to-nearest).
    // The adaptive rate will naturally build up toward the target delay.
    if (!m_bTimelineInitialized)
    {
      if (m_uiCount < 2)
        return; // Wait for enough data to interpolate
      m_RemoteTimeline = GetOldestSnapshot().m_Timestamp.m_NetworkTime;
      m_fPlaybackRate = 1.0f;
      m_bTimelineInitialized = true;
    }

    // How much data is buffered ahead of the current timeline position
    plTime newestTime = GetNewestSnapshot().m_Timestamp.m_NetworkTime;
    double bufferedTime = (newestTime - m_RemoteTimeline).GetSeconds();
    double targetBuffer = config.m_InterpolationDelay.GetSeconds();

    // Error: positive = too much buffered (play faster), negative = too little (play slower)
    double error = bufferedTime - targetBuffer;

    // After a transport burst, snap back to target delay instead of slowly catching up at 5% drift.
    static constexpr double kMaxBufferAheadSeconds = 0.5;
    if (error > kMaxBufferAheadSeconds)
    {
      m_RemoteTimeline = newestTime - config.m_InterpolationDelay;
      m_fPlaybackRate = 1.0f;
    }

    // Compute target playback rate from error
    float drift = plMath::Clamp(
      static_cast<float>(error) * config.m_fPlaybackAdaptSpeed,
      -config.m_fMaxPlaybackDrift,
      config.m_fMaxPlaybackDrift);
    float targetRate = 1.0f + drift;

    // Smooth rate transitions to avoid sudden speed changes
    float rateLerp = plMath::Min(1.0f, frameDt * 5.0f);
    m_fPlaybackRate = plMath::Lerp(m_fPlaybackRate, targetRate, rateLerp);

    // Advance the timeline
    m_RemoteTimeline += plTime::MakeFromSeconds(static_cast<double>(frameDt) * m_fPlaybackRate);

    // Safety: don't advance past the newest snapshot. If we run out of data,
    // the entity simply holds position until the next snapshot arrives.
    // The speed control should prevent this in normal conditions.
    if (m_RemoteTimeline > newestTime)
    {
      m_RemoteTimeline = newestTime;
    }
  }

  /// @brief Sample the interpolated transform at the current timeline position.
  ///
  /// Uses pure linear interpolation between the two snapshots that bracket
  /// the timeline. Linear interp is monotonic (no overshoot, no oscillation)
  /// which eliminates the ping-pong and elastic bounce artifacts.
  bool SampleAtTimeline(
    const plSnapshotInterpolationConfig& config,
    plTransform& out_transform,
    plVec3* out_velocity = nullptr) const
  {
    if (m_uiCount == 0 || !m_bTimelineInitialized)
    {
      return false;
    }

    plTime renderTime = m_RemoteTimeline;

    // Find the two snapshots bracketing the current timeline
    const plNetworkTransformSnapshot* pBefore = nullptr;
    const plNetworkTransformSnapshot* pAfter = nullptr;

    for (plUInt32 i = 0; i < m_uiCount; ++i)
    {
      const plNetworkTransformSnapshot& snap = GetSnapshot(i);
      if (snap.m_Timestamp.m_NetworkTime <= renderTime)
      {
        pBefore = &snap;
      }
      else
      {
        pAfter = &snap;
        break;
      }
    }

    // Edge cases
    if (!pBefore && !pAfter)
    {
      return false;
    }

    if (!pBefore)
    {
      // Timeline is before the oldest snapshot (startup / large time jump)
      out_transform = pAfter->m_Transform;
      if (out_velocity)
        *out_velocity = pAfter->m_vVelocity;
      return true;
    }

    if (!pAfter)
    {
      // Timeline is past the newest snapshot (speed control should prevent this,
      // but handle gracefully by holding at the newest known position)
      out_transform = pBefore->m_Transform;
      if (out_velocity)
        *out_velocity = pBefore->m_vVelocity;
      return true;
    }

    // Compute interpolation parameter
    plTime interval = pAfter->m_Timestamp.m_NetworkTime - pBefore->m_Timestamp.m_NetworkTime;
    if (interval.GetSeconds() <= 0.0001)
    {
      out_transform = pAfter->m_Transform;
      if (out_velocity)
        *out_velocity = pAfter->m_vVelocity;
      return true;
    }

    float t = static_cast<float>((renderTime - pBefore->m_Timestamp.m_NetworkTime).GetSeconds() / interval.GetSeconds());
    t = plMath::Clamp(t, 0.0f, 1.0f);

    // Teleport check
    float distSq = (pAfter->m_Transform.m_vPosition - pBefore->m_Transform.m_vPosition).GetLengthSquared();
    if (distSq > config.m_fTeleportThreshold * config.m_fTeleportThreshold)
    {
      out_transform = pAfter->m_Transform;
      if (out_velocity)
        *out_velocity = pAfter->m_vVelocity;
      return true;
    }

    // Linear interpolation -- simple, monotonic, zero overshoot
    out_transform.m_vPosition = plMath::Lerp(pBefore->m_Transform.m_vPosition, pAfter->m_Transform.m_vPosition, t);
    out_transform.m_qRotation = plQuat::MakeSlerp(pBefore->m_Transform.m_qRotation, pAfter->m_Transform.m_qRotation, t);
    out_transform.m_vScale = plMath::Lerp(pBefore->m_Transform.m_vScale, pAfter->m_Transform.m_vScale, t);

    if (out_velocity)
    {
      *out_velocity = plMath::Lerp(pBefore->m_vVelocity, pAfter->m_vVelocity, t);
    }

    return true;
  }

  /// @brief Get the current timeline position
  plTime GetTimeline() const { return m_RemoteTimeline; }

  /// @brief Get the current playback rate (1.0 = normal, >1 = catching up, <1 = slowing down)
  float GetPlaybackRate() const { return m_fPlaybackRate; }

  /// @brief Check if the timeline has been initialized
  bool IsTimelineInitialized() const { return m_bTimelineInitialized; }

  /// @brief Get the effective delay (newest snapshot time - timeline). Useful for debug.
  plTime GetCurrentDelay() const
  {
    if (m_uiCount == 0 || !m_bTimelineInitialized)
      return plTime::MakeZero();
    return GetNewestSnapshot().m_Timestamp.m_NetworkTime - m_RemoteTimeline;
  }

  /// @brief Get last measured jitter
  plTime GetLastJitter() const { return m_LastJitter; }

private:
  plNetworkTransformSnapshot m_Snapshots[BufferSize];
  plUInt32 m_uiHead;
  plUInt32 m_uiCount;

  // Adaptive-rate playback state
  plTime m_RemoteTimeline;     ///< Current playback position in network time
  float m_fPlaybackRate;       ///< Current speed multiplier (around 1.0)
  bool m_bTimelineInitialized; ///< Has the timeline been set from first snapshot

  plTime m_LastJitter;
};

/// @brief Helper class for snapshot-based transform interpolation.
///
/// Wraps plSnapshotBuffer with a simple API. Supports two usage patterns:
///   1. Explicit: call Update(dt) each frame, then GetInterpolatedTransform()
///   2. Legacy:   call GetInterpolatedTransform(networkTime) directly (auto-advances)
class PL_NETWORKPLUGIN_DLL plNetworkInterpolator
{
public:
  plNetworkInterpolator();

  /// @brief Configure the interpolation settings
  void SetConfig(const plSnapshotInterpolationConfig& config) { m_Config = config; }
  const plSnapshotInterpolationConfig& GetConfig() const { return m_Config; }

  /// @brief Add a new snapshot received from the network
  void AddSnapshot(const plTransform& transform, plTime networkTime, plTime localTime, plUInt32 sequence);

  /// @brief Add a snapshot with velocity
  void AddSnapshot(const plTransform& transform, const plVec3& velocity, plTime networkTime, plTime localTime, plUInt32 sequence);

  /// @brief Advance the playback timeline by one frame (call before GetInterpolatedTransform).
  /// If not called, GetInterpolatedTransform will auto-advance using time deltas.
  void Update(float frameDt);

  /// @brief Get the interpolated transform at the current timeline position.
  /// @param currentNetworkTime Used for auto-advance when Update() isn't called.
  bool GetInterpolatedTransform(plTime currentNetworkTime, plTransform& out_transform, plVec3* out_velocity = nullptr);

  /// @brief Clear all snapshots (e.g., on teleport or respawn)
  void Reset();

  /// @brief Get debug info
  plUInt32 GetSnapshotCount() const { return m_SnapshotBuffer.GetCount(); }
  plTime GetCurrentDelay() const { return m_SnapshotBuffer.GetCurrentDelay(); }
  plTime GetJitter() const { return m_SnapshotBuffer.GetLastJitter(); }
  float GetPlaybackRate() const { return m_SnapshotBuffer.GetPlaybackRate(); }
  plTime GetTimeline() const { return m_SnapshotBuffer.GetTimeline(); }

private:
  plSnapshotBuffer<16> m_SnapshotBuffer;
  plSnapshotInterpolationConfig m_Config;

  // For auto-advance when Update(dt) is not called
  plTime m_LastNetworkTime;
  bool m_bHasLastTime = false;
  bool m_bUpdatedThisFrame = false;
};
