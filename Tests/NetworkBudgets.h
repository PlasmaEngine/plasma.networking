#pragma once

#include <Foundation/Basics.h>

/// \brief Byte ceilings for NetworkBandwidthTest.cpp. Every value is ENet UDP payload (protocol headers, acks and pings
/// included) received by one client over loopback.
///
/// Each ceiling is the larger of the Windows and Linux maximum over 20 runs, plus 25 %, plus 64 bytes so that one extra
/// ping or time-sync round on a slow runner does not fail the small budgets. The test prints every measured value as a
/// "NetworkBudget:" line. A failure means replication became more expensive: fix the regression, or re-measure and raise
/// the ceiling in a commit that says why.
namespace NetworkBudgets
{
  /// Two clients connected, nothing changing, over 2 s. Pings and time sync only. Measured max 119 (Linux).
  constexpr plUInt64 kIdlePerClient2s = 220;

  /// One plNetworkedPropertiesComponent with an int and a vec3 changed once. Measured max 39.
  constexpr plUInt64 kOneReplicaUpdatePerClient = 120;

  /// Sixteen such components changed in the same frame. Measured max 580 (Linux).
  constexpr plUInt64 kSixteenReplicaUpdatePerClient = 790;

  /// Sixteen unchanged replicas over 1 s. OnChange entries cost nothing beyond the idle traffic. Measured max 36.
  constexpr plUInt64 kUnchangedReplicasPerClient1s = 110;

  /// One plNetworkedTransformComponent moving every frame, over 1 s, at its default 20 Hz send rate. Measured max 906 (Windows).
  constexpr plUInt64 kMovingTransformPerClient1s = 1200;

  /// The same transform standing still over 1 s. The component sends at its send rate whether or not it moved, so this
  /// costs as much as moving. Measured max 943.
  constexpr plUInt64 kStationaryTransformPerClient1s = 1250;
} // namespace NetworkBudgets
