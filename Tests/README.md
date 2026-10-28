# NetworkTest

The networking test suite. `NetworkTest.target.cs` and `networktest.module.cs` build the `NetworkTest` executable against
the package SDK, and `plasma.networking.workspace.cs` lists it next to the plugin targets.

## Files

- `NetworkTestHarness.h` holds what every test shares: `NetworkEndpoint` (one world, world module and replicated component,
  any number per process), `PumpUntil`, `AdmissionObserver`, the `plNetworkTestAccess` seams, and `HostileClient`.
- `HostileClient` is a raw ENet client with no application handshake. It sends exact bytes (`SendRaw`), a message with a
  free message ID and payload (`SendWithID`), or a well-formed handshake (`Handshake`), and can pick its own connection ID.
- `NetworkMessageTest.cpp` covers message bounds, quantization and prediction input bounds.
- `NetworkSecurityTest.cpp` covers admission, authority, RPC, level commands, transport limits and variant bounds. Its first
  block is the M0 gate: a host and two clients in one process replicate a property and tear down with no peers left.
- `NetworkFuzzTest.cpp` sweeps every message ID with empty, random, all-0xFF and all-zero payloads from an unadmitted and
  from an admitted client, then checks the host is up and a well-behaved client still replicates. It also feeds random
  buffers to the RPC, spawn, despawn, ownership, variant and string parsers in the host and client roles. The seed is fixed.

## Building and running

Package tests can move the installed packages in the user store, so run them only on a machine where that is acceptable.

    cd <package directory>
    set PLASMA_SDK_ROOT=<engine checkout with Binaries/Windows_Development built>
    set PLASMA_PACKAGE_PATH=<directory holding plasma.networking and plasma.jolt>
    plasmabuild build NetworkTest
    Bin\Tests\Windows_x64_Development\NetworkTest.exe -nogui -run

The test framework reports leaks per test under the Debug allocator. No CI lane builds or runs this suite yet; the
engine's Linux CPU gate excludes package suites.
