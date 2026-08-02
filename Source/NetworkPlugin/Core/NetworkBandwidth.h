#pragma once

#include <NetworkPlugin/NetworkPluginDLL.h>
#include <NetworkPlugin/Core/NetworkTypes.h>
#include <NetworkPlugin/Core/NetworkMessage.h>

#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/Deque.h>
#include <Foundation/Containers/StaticArray.h>
#include <Foundation/Time/Time.h>

class plNetworkWorldModule;
class plNetworkComponent;
class plNetworkPeer;
class plWorld;

/// @brief Stores the last known state of a component for delta compression
struct PL_NETWORKPLUGIN_DLL plNetworkComponentState
{
  plUInt32 m_uiNetworkID = 0;
  plTime m_LastUpdateTime;
  plTime m_LastSendTime;

  // Transform state for delta detection
  plVec3 m_vLastPosition = plVec3::MakeZero();
  plQuat m_qLastRotation = plQuat::MakeIdentity();
  plVec3 m_vLastScale = plVec3(1.0f);

  // Generic state buffer for other component data
  plDynamicArray<plUInt8> m_LastStateBuffer;

  // Priority and relevancy tracking
  plNetworkPriority::Enum m_Priority = plNetworkPriority::Normal;
  float m_fRelevancy = 1.0f;
};

/// @brief Configuration for bandwidth optimization
struct PL_NETWORKPLUGIN_DLL plNetworkBandwidthConfig
{
  /// @brief Maximum bytes per second to send
  plUInt32 m_uiMaxBytesPerSecond = 50000; // ~50 KB/s default

  /// @brief Maximum bytes per update tick
  plUInt32 m_uiMaxBytesPerTick = 4000;

  /// @brief Minimum update interval for low-priority components (seconds)
  float m_fMinUpdateIntervalLow = 0.5f;

  /// @brief Minimum update interval for normal-priority components (seconds)
  float m_fMinUpdateIntervalNormal = 0.1f;

  /// @brief Minimum update interval for high-priority components (seconds)
  float m_fMinUpdateIntervalHigh = 0.033f;

  /// @brief Distance threshold for position delta (below this, don't send)
  float m_fPositionDeltaThreshold = 0.01f;

  /// @brief Angle threshold for rotation delta in degrees (below this, don't send)
  float m_fRotationDeltaThreshold = 0.5f;

  /// @brief Enable delta compression
  bool m_bEnableDeltaCompression = true;

  /// @brief Enable priority-based scheduling
  bool m_bEnablePriorityScheduling = true;

  /// @brief Enable interest management (distance-based relevancy)
  bool m_bEnableInterestManagement = true;

  /// @brief Maximum distance for relevancy (objects beyond this are not replicated)
  float m_fMaxRelevancyDistance = 500.0f;
};

/// @brief Queued update for priority scheduling
struct PL_NETWORKPLUGIN_DLL plQueuedNetworkUpdate
{
  plUInt32 m_uiComponentNetID = 0;
  plNetworkPriority::Enum m_Priority = plNetworkPriority::Normal;
  float m_fRelevancy = 1.0f;
  plTime m_QueueTime;
  plUInt32 m_uiEstimatedSize = 0;

  /// @brief Calculate effective priority (lower = more important)
  float GetEffectivePriority() const
  {
    float timeFactor = (plTime::Now() - m_QueueTime).AsFloatInSeconds();
    return static_cast<float>(m_Priority) - timeFactor * 0.1f - m_fRelevancy;
  }

  bool operator<(const plQueuedNetworkUpdate& other) const
  {
    return GetEffectivePriority() < other.GetEffectivePriority();
  }
};

/// @brief Manages bandwidth optimization through delta compression, priority scheduling, and interest management
class PL_NETWORKPLUGIN_DLL plNetworkBandwidthManager
{
public:
  plNetworkBandwidthManager();
  ~plNetworkBandwidthManager();

  /// @brief Initialize with network module
  void Initialize(plNetworkWorldModule* pNetworkModule);

  /// @brief Shutdown and cleanup
  void Shutdown();

  /// @brief Set bandwidth configuration
  void SetConfig(const plNetworkBandwidthConfig& config) { m_Config = config; }
  const plNetworkBandwidthConfig& GetConfig() const { return m_Config; }

  //////////////////////////////////////////////////////////////////////////
  // Delta Compression
  //////////////////////////////////////////////////////////////////////////

  /// @brief Check if a component's transform has changed significantly
  bool HasTransformChanged(plUInt32 uiNetworkID, const plVec3& position, const plQuat& rotation, const plVec3& scale);

  /// @brief Update stored state after sending
  void UpdateStoredState(plUInt32 uiNetworkID, const plVec3& position, const plQuat& rotation, const plVec3& scale);

  /// @brief Get the delta between current and last known transform
  void GetTransformDelta(plUInt32 uiNetworkID, const plVec3& currentPos, const plQuat& currentRot,
                         plVec3& out_deltaPos, bool& out_hasRotationChanged);

  /// @brief Store arbitrary component state for delta comparison
  void StoreComponentState(plUInt32 uiNetworkID, plArrayPtr<const plUInt8> stateData);

  /// @brief Check if component state has changed
  bool HasStateChanged(plUInt32 uiNetworkID, plArrayPtr<const plUInt8> newStateData);

  //////////////////////////////////////////////////////////////////////////
  // Priority Scheduling
  //////////////////////////////////////////////////////////////////////////

  /// @brief Set the priority for a component
  void SetComponentPriority(plUInt32 uiNetworkID, plNetworkPriority::Enum priority);

  /// @brief Get the priority for a component
  plNetworkPriority::Enum GetComponentPriority(plUInt32 uiNetworkID) const;

  /// @brief Queue a component update for priority-based sending
  void QueueUpdate(plNetworkComponent* pComponent, plUInt32 uiEstimatedSize);

  /// @brief Process queued updates respecting bandwidth limits
  /// @return Number of updates processed
  plUInt32 ProcessUpdateQueue();

  /// @brief Check if we should send an update for this component now
  bool ShouldSendUpdate(plUInt32 uiNetworkID);

  /// @brief Mark that an update was sent
  void MarkUpdateSent(plUInt32 uiNetworkID);

  //////////////////////////////////////////////////////////////////////////
  // Interest Management (Relevancy)
  //////////////////////////////////////////////////////////////////////////

  /// @brief Calculate relevancy for a component relative to a client's view position
  float CalculateRelevancy(const plVec3& componentPosition, const plVec3& viewerPosition);

  /// @brief Check if a component is relevant to a specific client
  bool IsRelevantToClient(plUInt32 uiComponentNetID, plUInt32 uiClientID);

  /// @brief Set the view position for a client (for relevancy calculations)
  void SetClientViewPosition(plUInt32 uiClientID, const plVec3& position);

  /// @brief Get components relevant to a client, sorted by relevancy
  void GetRelevantComponents(plUInt32 uiClientID, plDynamicArray<plUInt32>& out_componentIDs);

  //////////////////////////////////////////////////////////////////////////
  // Statistics
  //////////////////////////////////////////////////////////////////////////

  /// @brief Record bytes sent (call this when sending network messages)
  void RecordBytesSent(plUInt32 uiBytes);

  /// @brief Get bytes sent this second
  plUInt32 GetBytesSentThisSecond() const { return m_uiBytesSentThisSecond; }

  /// @brief Get bytes remaining in budget
  plUInt32 GetBytesRemainingInBudget() const;

  /// @brief Get number of updates skipped due to bandwidth limits
  plUInt32 GetUpdatesSkipped() const { return m_uiUpdatesSkipped; }

  /// @brief Get total updates sent
  plUInt32 GetTotalUpdatesSent() const { return m_uiTotalUpdatesSent; }

  /// @brief Get delta compression savings in bytes
  plUInt32 GetDeltaCompressionSavings() const { return m_uiDeltaCompressionSavings; }

  /// @brief Reset statistics
  void ResetStatistics();

  /// @brief Called each frame to update timing
  void Update(float dt);

  /// @brief Draw bandwidth statistics to screen using debug renderer
  void DrawDebugStats(plWorld* pWorld);

private:
  /// @brief Draw the bandwidth history graph
  void DrawBandwidthGraph(plWorld* pWorld);

  /// @brief Get or create component state
  plNetworkComponentState& GetOrCreateState(plUInt32 uiNetworkID);

  /// @brief Get minimum update interval for priority
  float GetMinUpdateInterval(plNetworkPriority::Enum priority) const;

private:
  plNetworkWorldModule* m_pNetworkModule = nullptr;
  plNetworkBandwidthConfig m_Config;

  /// @brief Stored component states for delta compression
  plHashTable<plUInt32, plNetworkComponentState> m_ComponentStates;

  /// @brief Client view positions for relevancy calculations
  plHashTable<plUInt32, plVec3> m_ClientViewPositions;

  /// @brief Priority queue of pending updates
  plDeque<plQueuedNetworkUpdate> m_UpdateQueue;

  /// @brief Bandwidth tracking
  plUInt32 m_uiBytesSentThisSecond = 0;
  plUInt32 m_uiBytesSentThisTick = 0;
  float m_fSecondAccumulator = 0.0f;

  /// @brief Statistics
  plUInt32 m_uiUpdatesSkipped = 0;
  plUInt32 m_uiDeltaCompressionSavings = 0;
  plUInt32 m_uiTotalUpdatesSent = 0;
  plUInt32 m_uiPeakBytesPerSecond = 0;
  float m_fAverageBytesPerSecond = 0.0f;
  plUInt32 m_uiSampleCount = 0;

  /// @brief History buffer for graph visualization
  static constexpr plUInt32 s_uiGraphHistorySize = 120; // 2 seconds at 60fps
  plStaticArray<float, 120> m_BandwidthHistory;
  plUInt32 m_uiHistoryWriteIndex = 0;
  bool m_bHistoryFilled = false;
};
