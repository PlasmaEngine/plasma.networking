# NetworkTest

The networking test suite. `NetworkTest.target.cs` and `networktest.module.cs` build the `NetworkTest` executable against
the package SDK, and `plasma.networking.workspace.cs` lists it next to the plugin targets.

## Files

- `NetworkTestHarness.h` holds what every test shares: `NetworkEndpoint` (one world, world module and replicated component,
  any number per process), the waits, `AdmissionObserver`, the `plNetworkTestAccess` seams, and `HostileClient`.
- Every wait goes through `PumpAll`, which services a `Pumped` set until a condition holds or a deadline (5 s) passes. It
  yields instead of sleeping, so no wait is a count of timer-granularity sleeps. A host drops a client it does not hear
  from, so a wait lists every endpoint and hostile client alive in the test, not only the one it waits for. `Settle`
  waits until the host has acknowledged everything the hostile clients sent; `PumpUntil` and `PumpHostile` are shorthands.
- `HostileClient` is a raw ENet client with no application handshake. It sends exact bytes (`SendRaw`), a message with a
  free message ID and payload (`SendWithID`), or a well-formed handshake (`Handshake`), and can pick its own connection ID.
- `NetworkMessageTest.cpp` covers message bounds, quantization and prediction input bounds.
- `NetworkSecurityTest.cpp` covers admission, authority, RPC, level commands, transport limits and variant bounds. Its first
  block, "NW-F01 (M0 gate)", is the M0 gate: a host and two clients in one process replicate a property and tear down
  with no peers left. `DriverIsolation` covers NW-B46: every peer owns its driver, one client can stop and restart while
  the other keeps replicating, and a platform transport belongs to one driver at a time.
- `NetworkReplicationTest.cpp` (`TwoClientReplication`) runs a host and two clients through the shipped replication
  components: `plNetworkedPropertiesComponent` with server authority (only changed entries are sent), a scene
  `plNetworkedTransformComponent` simulated by the host, an owning client's upload relayed by the host to the other client,
  a client that joins late, 200 consecutive changes, and three teardown cases (the host stops with both clients connected,
  a client world is destroyed without `Stop`, ten connect and destroy rounds). The harness adds `AddReplica`, `AddMover`,
  `ConnectAll` and `PumpFor` for these.
- `NetworkBandwidthTest.cpp` (`BandwidthBudget`) measures the bytes each client receives (ENet UDP payload, overhead
  included) for idle traffic, one and sixteen replica changes, unchanged replicas, and a moving and a standing transform,
  and checks them against the ceilings in `NetworkBudgets.h`. Every measurement is printed as a `NetworkBudget:` line.
  To re-measure, run `-filter BandwidthBudget` a few times on Windows and Linux and read those lines; each ceiling is the
  larger platform maximum plus 25 %. The block that checks `plNetworkBandwidthConfig::m_uiMaxBytesPerTick` is disabled,
  because the send loop does not enforce that budget yet.
- `NetworkIsolationTest.cpp` covers the rest of NW-B46. `TwoSessions` runs two hosts with two clients each and checks that
  stopping one host ends only its own session. `SamePortRefused` checks that a second host can bind neither the wildcard
  nor the loopback address on a port a host already holds, in either order. On Windows this relies on the engine's ENet
  setting `SO_EXCLUSIVEADDRUSE`. `StartStopChurn` restarts one client 260 times next to a steady one, and `ParallelPump`
  updates a host and three clients on four threads, one world per thread.
- Shared on purpose between all peers in a process: the ENet clock, the network clock offset, the serializer's per-type
  property cache and the `net_sim_*` / `Networking.*` CVars. Everything else belongs to one world module, its peer and
  its driver; the rules are on `plNetworkDriverInterface`.
- `NetworkFuzzTest.cpp` sweeps every message ID with empty, random, all-0xFF and all-zero payloads from an unadmitted and
  from an admitted client, then checks the host is up and a well-behaved client still replicates. It also feeds random
  buffers to the RPC, spawn, despawn, ownership, variant and string parsers in the host and client roles. The seed is fixed.
- `NetworkTestComponents.cpp` reflects the components the flow tests add: `plNetworkRpcProbeComponent` (one RPC per
  target, counting its calls), `plNetworkRpcTwinComponent` (an RPC named like one on the probe) and
  `plNetworkCounterComponent` (a server value on the default channel and priority that counts what it applies).
- `NetworkSimulatorTest.cpp` (`Simulator`) covers the network simulator: presets set through `net_sim_preset`, reordering
  without jitter, seeded decisions, reliable messages that are delayed but never lost, and per-peer conditions measured as
  round-trip time.
- `NetworkFlowTest.cpp` runs spawn, despawn and ownership transfer (`SpawnAndOwnership`), late joins after runtime spawns
  (`LateJoin`), client-owned transforms and properties (`ClientOwnedState`) and level travel (`LevelTravel`), each on a
  host and on a dedicated server where both apply.
- `NetworkRpcTest.cpp` (`RpcRouting`) checks every RPC target sent from the server and from the owning client against the
  routing table in the file, a networked event fired from a client, RPC IDs of two types, a mistyped argument, the order
  of a burst and message pool use.
- `NetworkBlackboardTest.cpp` (`Blackboard`) covers server-authoritative and owner-writable blackboards, late joins and
  removed entries.
- `NetworkLifecycleTest.cpp` covers what `Stop()` resets, objects of a client that leaves, disconnect reasons, pruning of
  per-client records (`Lifecycle`) and that every networking component keeps its editor values through a scene export
  (`ComponentSerialization`).
- `NetworkConditionsTest.cpp` runs admission under latency (`AdmissionUnderLatency`), the default channel and priority
  (`ReplicationDefaults`), the replication rate after 30 days of uptime (`LongUptime`) and spawns, state, RPCs and
  transforms under every simulator preset (`ReplicationUnderConditions`). Measured rates are printed as `NetworkRate:` lines.
- Test blocks that cover a finding of the 1.4.1 review name it (`D01` and so on).

## Simulated network conditions and time

Every peer sends and receives through `plDriverSimulated`, which wraps the ENet or platform driver. It passes messages
straight through until conditions are set. An endpoint simulates `NetworkEndpoint::m_Simulation` from the moment it
starts, `Simulate()` changes them while it runs, and `plNetworkSimulationSettings::MakePreset` gives the named presets.
The conditions apply to what the endpoint sends and again to what it receives, and the seed makes the decisions repeat.
Reliable messages are delayed by loss, never dropped. Endpoints without settings follow the `net_sim_*` CVars.

`plNetworkClock::SetOffset` moves the clock every networking time stamp is taken from, so a test can run as if the
machine had been up for 30 days. Reset it to zero when the test ends.

## Building and running

Package tests can move the installed packages in the user store, so run them only on a machine where that is acceptable.
Nothing opens a window; the suite takes a few seconds.

### Through the engine scripts

From an engine checkout that is built (Development), in PowerShell 7. The sources directory must be outside the engine
checkout. The script clones the commits pinned in `Utilities/Scripts/TestPackages.json`, so this runs the pinned
networking commit, not local edits:

    ./Utilities/Scripts/PrepareTestPackages.ps1 -Purpose Tests -SourcesDirectory C:\PlasmaTestPackages
    ./Utilities/Scripts/RunUnitTests.ps1 -SkipBuild -PackageTests NetworkTest -PackageSourcesDirectory C:\PlasmaTestPackages

`RunUnitTests.ps1` prints a table with a `NetworkTest` row and writes `Output/UnitTests/NetworkTest/` (console log,
`results.json`). To build a checkout with local changes instead, put it and `plasma.jolt` side by side at the pinned
commits and pass `-UseExistingSources` to `PrepareTestPackages.ps1`.

### By hand

    cd <package directory>
    set PLASMA_SDK_ROOT=<engine checkout with Binaries/Windows_Development built>
    set PLASMA_PACKAGE_PATH=<directory holding plasma.networking and plasma.jolt>
    plasmabuild build NetworkTest -p Windows -a x64 -c Development
    set PATH=%CD%\Bin\Windows_x64_Development;%PLASMA_PACKAGE_PATH%\plasma.jolt\Bin\Windows_x64_Development;%PLASMA_SDK_ROOT%\Binaries\Windows_Development;%PATH%
    Bin\Tests\Windows_x64_Development\NetworkTest.exe -nogui -run -close

The executable loads `plNetworkPlugin.dll`, `plJoltPlugin.dll` and the engine DLLs (`plTestFramework.dll` and others) from
`PATH`; without the `PATH` line it exits immediately with a missing-DLL error. On Linux, build with `-p Linux`, put the
same three directories (`Linux_x64_Development`, `Binaries/Linux_Development`) on `LD_LIBRARY_PATH`, and run
`Bin/Tests/Linux_x64_Development/NetworkTest`.

Exit code 0 means every test passed. `-filter DriverIsolation` (or any test name) runs one test.

### What is normal in the output

Hundreds of `plLog Warning:` lines such as `Protocol violation`, `Message of id N not found` and `Reconnection failed` are
the hostile tests being rejected on purpose. Only lines starting with `Test failed:` are failures. Windows Defender
Firewall may ask about a new `NetworkTest.exe` path the first time; the tests use loopback only, so cancelling is fine.

## CI

`.github/workflows/networktest.yml` in this repository builds the engine SDK targets `GameEngine` and `TestFramework`
at a pinned engine tag, `plasma.jolt` at a pinned commit, then the plugin and `NetworkTest`, and runs the suite on Windows
and Linux for every pull request and every push to `main`. It needs the secret `PLASMA_ENGINE_TOKEN` (or
`PLASMA_PACKAGES_TOKEN`) with read access to the private engine repository. The measured `NetworkBudget:` and
`NetworkRate:` lines go to the job summary. Move `PLASMA_ENGINE_REF` and `PLASMA_JOLT_REF` with the engine release the
package targets.

The GitHub `Build and test` workflow of the engine also builds this suite on Windows and Linux (step "Build package test
suites") and runs it as the `NetworkTest` row of the unit test summary. It uses the commit pinned in the engine's
`Utilities/Scripts/TestPackages.json`, so a change here reaches that workflow only once the pin is moved to it. CI runs
Development; the test framework fails any test that leaks under its allocation tracking there.
