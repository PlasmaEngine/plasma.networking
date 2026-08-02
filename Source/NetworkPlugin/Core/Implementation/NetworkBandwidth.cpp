#include <NetworkPlugin/NetworkPluginPCH.h>

#include <NetworkPlugin/Core/NetworkBandwidth.h>
#include <NetworkPlugin/WorldModule/NetworkWorldModule.h>
#include <NetworkPlugin/Components/NetworkComponent.h>

#include <Foundation/Math/Math.h>
#include <Foundation/Math/Rect.h>
#include <Foundation/Configuration/CVar.h>
#include <RendererCore/Debug/DebugRenderer.h>
#include <Core/World/World.h>
#include <algorithm>

// CVars for bandwidth debugging
plCVarBool cvar_DrawBandwidthStats("Networking.DrawBandwidthStats", false, plCVarFlags::Default, "Draw network bandwidth statistics to screen");
plCVarBool cvar_DrawBandwidthGraph("Networking.DrawBandwidthGraph", false, plCVarFlags::Default, "Draw network bandwidth usage graph");

// Helper struct for relevancy sorting
struct plRelevantComponentInfo
{
  PL_DECLARE_POD_TYPE();
  
  plUInt32 m_uiNetworkID;
  float m_fRelevancy;
};

plNetworkBandwidthManager::plNetworkBandwidthManager()
{
  // Initialize bandwidth history buffer with zeros
  m_BandwidthHistory.SetCount(s_uiGraphHistorySize);
  for (plUInt32 i = 0; i < s_uiGraphHistorySize; ++i)
  {
    m_BandwidthHistory[i] = 0.0f;
  }
}

plNetworkBandwidthManager::~plNetworkBandwidthManager()
{
  Shutdown();
}

void plNetworkBandwidthManager::Initialize(plNetworkWorldModule* pNetworkModule)
{
  PL_ASSERT_DEV(pNetworkModule != nullptr, "NetworkWorldModule cannot be null");
  m_pNetworkModule = pNetworkModule;
  
  // Ensure history buffer is properly sized
  if (m_BandwidthHistory.GetCount() != s_uiGraphHistorySize)
  {
    m_BandwidthHistory.SetCount(s_uiGraphHistorySize);
    for (plUInt32 i = 0; i < s_uiGraphHistorySize; ++i)
    {
      m_BandwidthHistory[i] = 0.0f;
    }
  }
  
  ResetStatistics();
}

void plNetworkBandwidthManager::Shutdown()
{
  m_ComponentStates.Clear();
  m_ClientViewPositions.Clear();
  m_UpdateQueue.Clear();
  m_pNetworkModule = nullptr;
}

void plNetworkBandwidthManager::Update(float dt)
{
  // Safety: clamp dt to reasonable values to prevent accumulator issues
  if (dt <= 0.0f || dt > 1.0f)
    dt = 0.016f; // Default to ~60fps if dt is invalid

  m_fSecondAccumulator += dt;
  
  // Safety: clamp accumulator to prevent runaway values
  if (m_fSecondAccumulator > 10.0f)
    m_fSecondAccumulator = 0.0f;

  // Draw debug stats BEFORE resetting counters (so we see actual values)
  if (m_pNetworkModule != nullptr)
  {
    DrawDebugStats(m_pNetworkModule->GetWorld());
  }

  // Record bandwidth sample for graph visualization (with safety check)
  if (m_BandwidthHistory.GetCount() == s_uiGraphHistorySize && m_uiHistoryWriteIndex < s_uiGraphHistorySize)
  {
    float currentBandwidthKB = static_cast<float>(m_uiBytesSentThisSecond) / 1024.0f;
    m_BandwidthHistory[m_uiHistoryWriteIndex] = currentBandwidthKB;
    m_uiHistoryWriteIndex = (m_uiHistoryWriteIndex + 1) % s_uiGraphHistorySize;
    if (m_uiHistoryWriteIndex == 0)
      m_bHistoryFilled = true;
  }

  // Reset per-tick counter
  m_uiBytesSentThisTick = 0;

  // Reset per-second counter and update statistics
  if (m_fSecondAccumulator >= 1.0f)
  {
    // Track peak
    if (m_uiBytesSentThisSecond > m_uiPeakBytesPerSecond)
    {
      m_uiPeakBytesPerSecond = m_uiBytesSentThisSecond;
    }

    // Update running average (with overflow protection)
    if (m_uiSampleCount < 0xFFFFFFFF)
      m_uiSampleCount++;
    m_fAverageBytesPerSecond = m_fAverageBytesPerSecond + 
      (static_cast<float>(m_uiBytesSentThisSecond) - m_fAverageBytesPerSecond) / m_uiSampleCount;

    m_uiBytesSentThisSecond = 0;
    m_fSecondAccumulator -= 1.0f;
  }
}

//////////////////////////////////////////////////////////////////////////
// Delta Compression
//////////////////////////////////////////////////////////////////////////

plNetworkComponentState& plNetworkBandwidthManager::GetOrCreateState(plUInt32 uiNetworkID)
{
  plNetworkComponentState* pState = nullptr;
  if (!m_ComponentStates.TryGetValue(uiNetworkID, pState))
  {
    plNetworkComponentState newState;
    newState.m_uiNetworkID = uiNetworkID;
    newState.m_LastUpdateTime = plTime::Now();
    m_ComponentStates[uiNetworkID] = newState;
    pState = &m_ComponentStates[uiNetworkID];
  }
  return *pState;
}

bool plNetworkBandwidthManager::HasTransformChanged(
  plUInt32 uiNetworkID,
  const plVec3& position,
  const plQuat& rotation,
  const plVec3& scale)
{
  if (!m_Config.m_bEnableDeltaCompression)
    return true;

  plNetworkComponentState& state = GetOrCreateState(uiNetworkID);

  // Check position delta
  plVec3 posDelta = position - state.m_vLastPosition;
  if (posDelta.GetLengthSquared() > m_Config.m_fPositionDeltaThreshold * m_Config.m_fPositionDeltaThreshold)
  {
    return true;
  }

  // Check rotation delta using IsEqualRotation
  // Convert degree threshold to a reasonable epsilon value
  float rotEpsilon = m_Config.m_fRotationDeltaThreshold * 0.01745f; // Convert degrees to approximate radians for epsilon
  if (!state.m_qLastRotation.IsEqualRotation(rotation, rotEpsilon))
  {
    return true;
  }

  // Check scale delta
  plVec3 scaleDelta = scale - state.m_vLastScale;
  if (scaleDelta.GetLengthSquared() > 0.0001f)
  {
    return true;
  }

  return false;
}

void plNetworkBandwidthManager::UpdateStoredState(
  plUInt32 uiNetworkID,
  const plVec3& position,
  const plQuat& rotation,
  const plVec3& scale)
{
  plNetworkComponentState& state = GetOrCreateState(uiNetworkID);
  state.m_vLastPosition = position;
  state.m_qLastRotation = rotation;
  state.m_vLastScale = scale;
  state.m_LastUpdateTime = plTime::Now();
}

void plNetworkBandwidthManager::GetTransformDelta(
  plUInt32 uiNetworkID,
  const plVec3& currentPos,
  const plQuat& currentRot,
  plVec3& out_deltaPos,
  bool& out_hasRotationChanged)
{
  plNetworkComponentState& state = GetOrCreateState(uiNetworkID);

  out_deltaPos = currentPos - state.m_vLastPosition;

  // Check if rotation changed significantly
  float rotEpsilon = m_Config.m_fRotationDeltaThreshold * 0.01745f; // Convert degrees to approximate radians for epsilon
  out_hasRotationChanged = !state.m_qLastRotation.IsEqualRotation(currentRot, rotEpsilon);
}

void plNetworkBandwidthManager::StoreComponentState(plUInt32 uiNetworkID, plArrayPtr<const plUInt8> stateData)
{
  plNetworkComponentState& state = GetOrCreateState(uiNetworkID);
  state.m_LastStateBuffer.SetCount(stateData.GetCount());
  plMemoryUtils::Copy(state.m_LastStateBuffer.GetData(), stateData.GetPtr(), stateData.GetCount());
  state.m_LastUpdateTime = plTime::Now();
}

bool plNetworkBandwidthManager::HasStateChanged(plUInt32 uiNetworkID, plArrayPtr<const plUInt8> newStateData)
{
  if (!m_Config.m_bEnableDeltaCompression)
    return true;

  plNetworkComponentState* pState = nullptr;
  if (!m_ComponentStates.TryGetValue(uiNetworkID, pState))
  {
    return true; // No previous state, definitely changed
  }

  if (pState->m_LastStateBuffer.GetCount() != newStateData.GetCount())
  {
    return true;
  }

  // Compare byte by byte
  return plMemoryUtils::Compare(pState->m_LastStateBuffer.GetData(), newStateData.GetPtr(), newStateData.GetCount()) != 0;
}

//////////////////////////////////////////////////////////////////////////
// Priority Scheduling
//////////////////////////////////////////////////////////////////////////

void plNetworkBandwidthManager::SetComponentPriority(plUInt32 uiNetworkID, plNetworkPriority::Enum priority)
{
  plNetworkComponentState& state = GetOrCreateState(uiNetworkID);
  state.m_Priority = priority;
}

plNetworkPriority::Enum plNetworkBandwidthManager::GetComponentPriority(plUInt32 uiNetworkID) const
{
  const plNetworkComponentState* pState = nullptr;
  if (m_ComponentStates.TryGetValue(uiNetworkID, pState))
  {
    return pState->m_Priority;
  }
  return plNetworkPriority::Normal;
}

void plNetworkBandwidthManager::QueueUpdate(plNetworkComponent* pComponent, plUInt32 uiEstimatedSize)
{
  if (!pComponent || !m_Config.m_bEnablePriorityScheduling)
    return;

  plQueuedNetworkUpdate update;
  update.m_uiComponentNetID = pComponent->GetNetID();
  update.m_Priority = GetComponentPriority(pComponent->GetNetID());
  update.m_QueueTime = plTime::Now();
  update.m_uiEstimatedSize = uiEstimatedSize;

  // Calculate relevancy (simplified - would need client position)
  update.m_fRelevancy = 1.0f;

  m_UpdateQueue.PushBack(update);
}

plUInt32 plNetworkBandwidthManager::ProcessUpdateQueue()
{
  if (m_UpdateQueue.IsEmpty())
    return 0;

  // Sort queue by effective priority
  // Copy to array for sorting since plDeque doesn't support random access
  plDynamicArray<plQueuedNetworkUpdate> sortedQueue;
  sortedQueue.Reserve(m_UpdateQueue.GetCount());
  while (!m_UpdateQueue.IsEmpty())
  {
    sortedQueue.PushBack(m_UpdateQueue.PeekFront());
    m_UpdateQueue.PopFront();
  }
  sortedQueue.Sort([](const plQueuedNetworkUpdate& a, const plQueuedNetworkUpdate& b) {
    return a.GetEffectivePriority() < b.GetEffectivePriority();
  });
  // Push back to queue in sorted order
  for (const auto& update : sortedQueue)
  {
    m_UpdateQueue.PushBack(update);
  }

  plUInt32 processedCount = 0;
  plUInt32 bytesRemaining = GetBytesRemainingInBudget();

  while (!m_UpdateQueue.IsEmpty() && bytesRemaining > 0)
  {
    const plQueuedNetworkUpdate& update = m_UpdateQueue.PeekFront();

    if (update.m_uiEstimatedSize > bytesRemaining)
    {
      // Not enough budget for this update
      if (update.m_Priority == plNetworkPriority::Critical)
      {
        // Critical updates always go through
        // Fall through
      }
      else
      {
        m_uiUpdatesSkipped++;
        m_UpdateQueue.PopFront();
        continue;
      }
    }

    // Process this update
    bytesRemaining -= plMath::Min(bytesRemaining, update.m_uiEstimatedSize);
    m_uiBytesSentThisTick += update.m_uiEstimatedSize;
    m_uiBytesSentThisSecond += update.m_uiEstimatedSize;
    m_uiTotalUpdatesSent++;

    MarkUpdateSent(update.m_uiComponentNetID);

    m_UpdateQueue.PopFront();
    processedCount++;
  }

  return processedCount;
}

bool plNetworkBandwidthManager::ShouldSendUpdate(plUInt32 uiNetworkID)
{
  if (!m_Config.m_bEnablePriorityScheduling)
    return true;

  const plNetworkComponentState* pState = nullptr;
  if (!m_ComponentStates.TryGetValue(uiNetworkID, pState))
  {
    return true; // No state yet, should send
  }

  // Check minimum update interval based on priority
  float minInterval = GetMinUpdateInterval(pState->m_Priority);
  float timeSinceLastSend = (plTime::Now() - pState->m_LastSendTime).AsFloatInSeconds();

  if (timeSinceLastSend < minInterval)
  {
    return false;
  }

  // Check bandwidth budget
  if (m_uiBytesSentThisTick >= m_Config.m_uiMaxBytesPerTick)
  {
    // Critical priority bypasses limits
    return pState->m_Priority == plNetworkPriority::Critical;
  }

  return true;
}

void plNetworkBandwidthManager::MarkUpdateSent(plUInt32 uiNetworkID)
{
  plNetworkComponentState& state = GetOrCreateState(uiNetworkID);
  state.m_LastSendTime = plTime::Now();
}

float plNetworkBandwidthManager::GetMinUpdateInterval(plNetworkPriority::Enum priority) const
{
  switch (priority)
  {
    case plNetworkPriority::Critical:
      return 0.0f; // Always send immediately

    case plNetworkPriority::High:
      return m_Config.m_fMinUpdateIntervalHigh;

    case plNetworkPriority::Normal:
      return m_Config.m_fMinUpdateIntervalNormal;

    case plNetworkPriority::Low:
    case plNetworkPriority::VeryLow:
      return m_Config.m_fMinUpdateIntervalLow;

    default:
      return m_Config.m_fMinUpdateIntervalNormal;
  }
}

//////////////////////////////////////////////////////////////////////////
// Interest Management
//////////////////////////////////////////////////////////////////////////

float plNetworkBandwidthManager::CalculateRelevancy(const plVec3& componentPosition, const plVec3& viewerPosition)
{
  if (!m_Config.m_bEnableInterestManagement)
    return 1.0f;

  float distance = (componentPosition - viewerPosition).GetLength();

  if (distance >= m_Config.m_fMaxRelevancyDistance)
  {
    return 0.0f; // Not relevant
  }

  // Linear falloff from 1.0 at distance 0 to 0.0 at max distance
  return 1.0f - (distance / m_Config.m_fMaxRelevancyDistance);
}

bool plNetworkBandwidthManager::IsRelevantToClient(plUInt32 uiComponentNetID, plUInt32 uiClientID)
{
  if (!m_Config.m_bEnableInterestManagement)
    return true;

  plVec3* pViewPos = nullptr;
  if (!m_ClientViewPositions.TryGetValue(uiClientID, pViewPos))
  {
    return true; // No view position set, assume relevant
  }

  plNetworkComponentState* pState = nullptr;
  if (!m_ComponentStates.TryGetValue(uiComponentNetID, pState))
  {
    return true; // No state, assume relevant
  }

  float relevancy = CalculateRelevancy(pState->m_vLastPosition, *pViewPos);
  return relevancy > 0.01f; // Small threshold to avoid floating point issues
}

void plNetworkBandwidthManager::SetClientViewPosition(plUInt32 uiClientID, const plVec3& position)
{
  m_ClientViewPositions[uiClientID] = position;
}

void plNetworkBandwidthManager::GetRelevantComponents(plUInt32 uiClientID, plDynamicArray<plUInt32>& out_componentIDs)
{
  out_componentIDs.Clear();

  plVec3* pViewPos = nullptr;
  if (!m_ClientViewPositions.TryGetValue(uiClientID, pViewPos))
  {
    // No view position, return all components
    for (auto it = m_ComponentStates.GetIterator(); it.IsValid(); ++it)
    {
      out_componentIDs.PushBack(it.Key());
    }
    return;
  }

  // Collect relevant components with their relevancy values
  plHybridArray<plRelevantComponentInfo, 64> relevant;

  for (auto it = m_ComponentStates.GetIterator(); it.IsValid(); ++it)
  {
    float relevancy = CalculateRelevancy(it.Value().m_vLastPosition, *pViewPos);
    if (relevancy > 0.01f)
    {
      relevant.PushBack({it.Key(), relevancy});
    }
  }

  // Sort by relevancy (highest first)
  std::sort(relevant.GetData(), relevant.GetData() + relevant.GetCount(),
    [](const plRelevantComponentInfo& a, const plRelevantComponentInfo& b) {
      return a.m_fRelevancy > b.m_fRelevancy;
    });

  // Output sorted IDs
  for (const auto& comp : relevant)
  {
    out_componentIDs.PushBack(comp.m_uiNetworkID);
  }
}

//////////////////////////////////////////////////////////////////////////
// Statistics
//////////////////////////////////////////////////////////////////////////

plUInt32 plNetworkBandwidthManager::GetBytesRemainingInBudget() const
{
  plUInt32 tickBudget = m_Config.m_uiMaxBytesPerTick > m_uiBytesSentThisTick ?
                        m_Config.m_uiMaxBytesPerTick - m_uiBytesSentThisTick : 0;

  plUInt32 secondBudget = m_Config.m_uiMaxBytesPerSecond > m_uiBytesSentThisSecond ?
                          m_Config.m_uiMaxBytesPerSecond - m_uiBytesSentThisSecond : 0;

  return plMath::Min(tickBudget, secondBudget);
}

void plNetworkBandwidthManager::RecordBytesSent(plUInt32 uiBytes)
{
  m_uiBytesSentThisTick += uiBytes;
  m_uiBytesSentThisSecond += uiBytes;
  m_uiTotalUpdatesSent++;
  
  // Debug: uncomment to verify bytes are being recorded
  // plLog::Dev("RecordBytesSent: {0} bytes (tick total: {1}, sec total: {2})", 
  //   uiBytes, m_uiBytesSentThisTick, m_uiBytesSentThisSecond);
}

void plNetworkBandwidthManager::ResetStatistics()
{
  m_uiBytesSentThisSecond = 0;
  m_uiBytesSentThisTick = 0;
  m_uiUpdatesSkipped = 0;
  m_uiDeltaCompressionSavings = 0;
  m_uiTotalUpdatesSent = 0;
  m_uiPeakBytesPerSecond = 0;
  m_fAverageBytesPerSecond = 0.0f;
  m_uiSampleCount = 0;
  m_fSecondAccumulator = 0.0f;
}

void plNetworkBandwidthManager::DrawDebugStats(plWorld* pWorld)
{
  if (!cvar_DrawBandwidthStats.GetValue())
    return;
    
  if (pWorld == nullptr)
    return;

  // Header
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", 
    "=== Network Bandwidth Stats ===", plColor::Cyan);

  // Current bandwidth usage - show bytes directly for accuracy
  plUInt32 usagePercent = m_Config.m_uiMaxBytesPerSecond > 0 ? 
    (m_uiBytesSentThisSecond * 100) / m_Config.m_uiMaxBytesPerSecond : 0;

  plStringBuilder sb;

  // Show raw bytes for better visibility of small values
  sb.Clear();
  sb.Append("Bandwidth: ");
  sb.AppendFormat("{0}", m_uiBytesSentThisSecond);
  sb.Append(" B/s / ");
  sb.AppendFormat("{0}", m_Config.m_uiMaxBytesPerSecond);
  sb.Append(" B/s (");
  sb.AppendFormat("{0}", usagePercent);
  sb.Append("%)");
  
  plColor usageColor = plColor::Green;
  if (usagePercent > 80)
    usageColor = plColor::Red;
  else if (usagePercent > 50)
    usageColor = plColor::Yellow;
  
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, usageColor);

  // Peak and average
  sb.Clear();
  sb.Append("Peak: ");
  sb.AppendFormat("{0}", m_uiPeakBytesPerSecond);
  sb.Append(" B/s | Avg: ");
  sb.AppendFormat("{0}", static_cast<plUInt32>(m_fAverageBytesPerSecond));
  sb.Append(" B/s");
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, plColor::White);

  // Tick budget
  plUInt32 tickBudgetPercent = m_Config.m_uiMaxBytesPerTick > 0 ? 
    (m_uiBytesSentThisTick * 100) / m_Config.m_uiMaxBytesPerTick : 0;
  sb.Clear();
  sb.Append("Tick Budget: ");
  sb.AppendFormat("{0}", m_uiBytesSentThisTick);
  sb.Append(" / ");
  sb.AppendFormat("{0}", m_Config.m_uiMaxBytesPerTick);
  sb.Append(" bytes (");
  sb.AppendFormat("{0}", tickBudgetPercent);
  sb.Append("%)");
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, plColor::White);

  // Updates sent vs skipped
  sb.Clear();
  sb.Append("Updates Sent: ");
  sb.AppendFormat("{0}", m_uiTotalUpdatesSent);
  sb.Append(" | Skipped: ");
  sb.AppendFormat("{0}", m_uiUpdatesSkipped);
  plColor skipColor = m_uiUpdatesSkipped > 0 ? plColor::Yellow : plColor::Green;
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, skipColor);

  // Delta compression savings
  if (m_Config.m_bEnableDeltaCompression)
  {
    sb.Clear();
    sb.Append("Delta Compression Savings: ");
    sb.AppendFormat("{0}", m_uiDeltaCompressionSavings / 1024);
    sb.Append(" KB");
    plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, plColor::LightGreen);
  }

  // Component states tracked
  sb.Clear();
  sb.Append("Components Tracked: ");
  sb.AppendFormat("{0}", m_ComponentStates.GetCount());
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, plColor::White);

  // Queue status
  sb.Clear();
  sb.Append("Update Queue: ");
  sb.AppendFormat("{0}", m_UpdateQueue.GetCount());
  sb.Append(" pending");
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, plColor::White);

  // Configuration status
  sb.Clear();
  sb.Append("Features: ");
  bool hasFeature = false;
  if (m_Config.m_bEnableDeltaCompression)
  {
    sb.Append("[Delta] ");
    hasFeature = true;
  }
  if (m_Config.m_bEnablePriorityScheduling)
  {
    sb.Append("[Priority] ");
    hasFeature = true;
  }
  if (m_Config.m_bEnableInterestManagement)
  {
    sb.Append("[Interest] ");
    hasFeature = true;
  }
  if (!hasFeature)
    sb.Append("None");
  plDebugRenderer::DrawInfoText(pWorld, plDebugTextPlacement::TopLeft, "NetBandwidth", sb, plColor::LightBlue);

  // Draw bandwidth graph if enabled
  if (cvar_DrawBandwidthGraph.GetValue())
  {
    DrawBandwidthGraph(pWorld);
  }
}

void plNetworkBandwidthManager::DrawBandwidthGraph(plWorld* pWorld)
{
  if (!pWorld)
    return;

  // Safety check - ensure history buffer is valid
  if (m_BandwidthHistory.GetCount() != s_uiGraphHistorySize)
    return;

  // Calculate number of samples to draw
  plUInt32 sampleCount = m_bHistoryFilled ? s_uiGraphHistorySize : m_uiHistoryWriteIndex;
  if (sampleCount < 2)
    return;

  // Graph configuration
  const float graphWidth = 300.0f;
  const float graphHeight = 100.0f;
  const float graphX = 10.0f;
  const float graphY = 200.0f; // Below the text stats
  float maxBandwidthKB = m_Config.m_uiMaxBytesPerSecond / 1024.0f;
  
  // Avoid division by zero
  if (maxBandwidthKB <= 0.0f)
    maxBandwidthKB = 50.0f; // Default fallback

  // Draw graph background using 2D rectangle
  plRectFloat bgRect(graphX, graphY, graphWidth, graphHeight);
  plDebugRenderer::Draw2DRectangle(pWorld, bgRect, 0.0f, plColor(0.0f, 0.0f, 0.0f, 0.5f));

  // Build lines for the graph border and reference lines
  plHybridArray<plDebugRenderer::Line, 4> borderLines;
  
  // Top border (max bandwidth line)
  plDebugRenderer::Line topLine;
  topLine.m_start = plVec3(graphX, graphY, 0);
  topLine.m_end = plVec3(graphX + graphWidth, graphY, 0);
  topLine.m_startColor = plColor::Red;
  topLine.m_endColor = plColor::Red;
  borderLines.PushBack(topLine);
  
  // 50% reference line
  plDebugRenderer::Line midLine;
  midLine.m_start = plVec3(graphX, graphY + graphHeight * 0.5f, 0);
  midLine.m_end = plVec3(graphX + graphWidth, graphY + graphHeight * 0.5f, 0);
  midLine.m_startColor = plColor(0.5f, 0.5f, 0.5f, 0.5f);
  midLine.m_endColor = plColor(0.5f, 0.5f, 0.5f, 0.5f);
  borderLines.PushBack(midLine);
  
  plDebugRenderer::Draw2DLines(pWorld, borderLines, plColor::White);

  // Draw bandwidth history as line graph
  float stepX = graphWidth / static_cast<float>(s_uiGraphHistorySize - 1);
  
  plHybridArray<plDebugRenderer::Line, 128> graphLines;

  for (plUInt32 i = 1; i < sampleCount; ++i)
  {
    // Calculate indices wrapping around the circular buffer
    plUInt32 prevIdx = m_bHistoryFilled ? 
      (m_uiHistoryWriteIndex + i - 1) % s_uiGraphHistorySize :
      i - 1;
    plUInt32 currIdx = m_bHistoryFilled ? 
      (m_uiHistoryWriteIndex + i) % s_uiGraphHistorySize :
      i;

    float prevValue = m_BandwidthHistory[prevIdx];
    float currValue = m_BandwidthHistory[currIdx];

    // Normalize to graph height (0 = bottom, 1 = top)
    float prevNorm = plMath::Clamp(prevValue / maxBandwidthKB, 0.0f, 1.0f);
    float currNorm = plMath::Clamp(currValue / maxBandwidthKB, 0.0f, 1.0f);

    // Convert to screen coordinates (Y is inverted - 0 at top)
    float x1 = graphX + (i - 1) * stepX;
    float y1 = graphY + graphHeight - (prevNorm * graphHeight);
    float x2 = graphX + i * stepX;
    float y2 = graphY + graphHeight - (currNorm * graphHeight);

    // Color based on bandwidth usage
    plColor lineColor = plColor::Green;
    float avgUsage = (prevNorm + currNorm) * 0.5f;
    if (avgUsage > 0.8f)
      lineColor = plColor::Red;
    else if (avgUsage > 0.5f)
      lineColor = plColor::Yellow;

    plDebugRenderer::Line line;
    line.m_start = plVec3(x1, y1, 0);
    line.m_end = plVec3(x2, y2, 0);
    line.m_startColor = lineColor;
    line.m_endColor = lineColor;
    graphLines.PushBack(line);
  }
  
  plDebugRenderer::Draw2DLines(pWorld, graphLines, plColor::White);

  // Draw axis labels using integer pixel positions
  plUInt32 maxBandwidthKBInt = static_cast<plUInt32>(maxBandwidthKB);
  plStringBuilder label;
  label.Clear();
  label.AppendFormat("{0}", maxBandwidthKBInt);
  label.Append(" KB/s");
  plDebugRenderer::Draw2DText(pWorld, label, plVec2I32(static_cast<plInt32>(graphX + graphWidth + 5), static_cast<plInt32>(graphY)), plColor::White, 10);

  label.Clear();
  label.AppendFormat("{0}", maxBandwidthKBInt / 2);
  plDebugRenderer::Draw2DText(pWorld, label, plVec2I32(static_cast<plInt32>(graphX + graphWidth + 5), static_cast<plInt32>(graphY + graphHeight * 0.5f)), plColor::Gray, 10);

  plDebugRenderer::Draw2DText(pWorld, "0", plVec2I32(static_cast<plInt32>(graphX + graphWidth + 5), static_cast<plInt32>(graphY + graphHeight)), plColor::Gray, 10);

  // Draw title
  plDebugRenderer::Draw2DText(pWorld, "Bandwidth (2s)", plVec2I32(static_cast<plInt32>(graphX), static_cast<plInt32>(graphY - 15)), plColor::Cyan, 12);
}
