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
- `NetworkFuzzTest.cpp` sweeps every message ID with empty, random, all-0xFF and all-zero payloads from an unadmitted and
  from an admitted client, then checks the host is up and a well-behaved client still replicates. It also feeds random
  buffers to the RPC, spawn, despawn, ownership, variant and string parsers in the host and client roles. The seed is fixed.

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

The GitHub `Build and test` workflow of the engine builds this suite on Windows and Linux (step "Build package test
suites") and runs it as the `NetworkTest` row of the unit test summary. It uses the commit pinned in the engine's
`Utilities/Scripts/TestPackages.json`, so a change here reaches CI only once that pin is moved to it. CI runs Development;
the test framework fails any test that leaks under its allocation tracking there.
