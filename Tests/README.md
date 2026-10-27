# NetworkTest

The networking test suite, moved here from the engine's `Code/UnitTests/NetworkTest` when the plugin
became a package. It tests this package's code, so it belongs with it.

`NetworkTest.target.cs` and `networktest.module.cs` build the `NetworkTest` executable against the package SDK
(`NetworkSecurityTest.cpp` holds the admission, authority, RPC, level, transport and variant-bound tests).
No CI lane builds or runs it yet; the engine's Linux CPU gate excludes package suites.

`NetworkTest.target.cs.reference` and `networktest.module.cs.reference` are the original PlasmaBuild
rule files, parked with a suffix so the package build does not try to compile them: they reference
engine-only helpers such as `PlasmaBuildQt` and `TestFrameworkModule` that do not exist outside the
engine tree. They are kept as the starting point for whenever packages can carry their own tests.
