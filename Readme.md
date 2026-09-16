# plasma.networking

Multiplayer networking for PlasmaEngine — client/server sessions, replicated components and property
replication. Moved out of the engine tree to be built and distributed as a package.

Published as [`plasma.networking`](https://plasmaengine.github.io/PlasmaPackages/index.plPackageIndex)
in the Plasma package registry.

## Layout

```
Package.plPackage           the manifest - identity, compatibility, components
NetworkPlugin.plPluginBundle legacy descriptor, kept until the engine reads manifests directly
Source/
  NetworkPlugin/            runtime -> plNetworkPlugin
  EditorPluginNetworking/   editor  -> plEditorPluginNetworking  (Qt)
Tests/                      the network test suite, moved here with the code it exercises
Configs/                    build rules for building against an installed engine SDK
Branding/                   icon and banner
```

## Nothing is vendored

Both of this plugin's third-party dependencies stay in the engine:

- **enet** — `Foundation` depends on `EnetModule`, so enet is compiled into `plFoundation.dll` and
  its symbols are exported from there. An earlier attempt to vendor a copy here linked two of
  everything and failed with `LNK2005` on every `enet_*` symbol. The headers are reachable through
  the SDK's `Code/ThirdParty` root, so all this package adds is the feature define.
- **Jolt** — a template default that stays in the engine. This package reads its headers out of the
  SDK and links `plJoltPlugin.lib`, exactly as the in-engine build did. `BUILDSYSTEM_ENABLE_JOLT_SUPPORT`
  and `JPH_DEBUG_RENDERER` are restated here because they are part of the binary interface:
  `JPH_DEBUG_RENDERER` changes the layout of Jolt's types.

## Dependencies

None resolvable through the registry. The bundle declares `RequiredPlugins{"Jolt"}`, and unlike
`AudioSystem` that really is a plugin rather than an engine module — but it is not a package, so
there is nothing to resolve. If Jolt is ever packaged, this manifest gains a `Requires` line.

## Platform transport and lobbies

Sessions can ride on a platform backend's relay/P2P transport instead of ENet sockets, and be found
through platform lobbies. Both need a platform backend package (Steam, EOS, ...) that provides
`plPlatformTransport` and lobby support through `plPlatformServices`; without one the `Platform`
driver logs a warning and the peer falls back to ENet.

`plNetworkConfig` gains four fields: `m_sLobbyId`, `m_bCreatePlatformLobby`, `m_uiLobbyMaxMembers`
(0 = connection limit) and `m_bLobbyPublic`. `plDriverTypeEnum::Platform` selects the transport
driver; its server address is a user, written as `user:steam:7656...`, `steam:7656...` or a bare
native id of the active backend. The port is ignored.

Host:

```cpp
plNetworkConfig config;
config.m_DriverType = plDriverTypeEnum::Platform;
config.m_bCreatePlatformLobby = true;
config.m_uiConnectionLimit = 4;

plNetworkSession* pSession = pWorld->GetOrCreateModule<plNetworkWorldModule>()->GetSession();
pSession->CreateSession(config);
// The lobby is created asynchronously. Once pSession->GetLobbyId().IsValid(), the lobby carries the
// attributes "session_address" (this host's user id, or host:port on ENet) and "session_driver".
```

Join by lobby id:

```cpp
plNetworkConfig config;
config.m_sLobbyId = "<lobby id from Find, an invite or a friend>";

pSession->JoinSession(config);
// pSession->IsJoiningLobby() is true and GetSessionState() reports Lobby while the platform join is
// pending. The address and driver are read from the lobby; StartClient runs on the next world update.
```

`LeaveSession()` also leaves the platform lobby the session created or joined. From scripts the same
fields are exposed as `Networking.SetLobbyId`, `GetLobbyId`, `SetCreatePlatformLobby`,
`SetDriverType("ENet"|"Platform")` and `IsJoiningLobby`.

## Building

Requires an engine SDK — a built engine checkout — named by `PLASMA_SDK_ROOT`:

```bash
PLASMA_SDK_ROOT=/path/to/engine plasmabuild build EditorPluginNetworking
```

Output lands in `Bin/Windows_x64_Development/`.

## Publishing a new version

Staging and publishing are driven from the engine and registry repositories:

```bash
# engine repo - stage and validate, emitting machine-readable output
python Utilities/PackageInspector.py <staged-package-dir> --json <out>.json

# registry repo - upload blobs and record the release
python tools/publish_package.py <out>.json
```

Versions are immutable. A mistake is fixed by publishing a new patch version, never by replacing an
existing one — every lock file that already trusts a version must keep resolving to the same bytes.
