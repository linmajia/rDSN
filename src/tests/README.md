
This directory contains shared test infrastructure and standalone integration tests.

* `support` provides the test-only rDSN application and GoogleTest runner used by
  component test executables.
* `idl` tests code generation atop rDSN using Apache Thrift and Protocol Buffers.

Component unit tests remain beside their implementations as `*.test.cpp`, but they
are excluded from production libraries and built into one executable per component.
CTest registers the runtime configurations and serializes tests that use the global
rDSN runtime. Set `BUILD_TESTING=OFF` (or run `./run.sh build --skip_tests`) to omit
the migrated component test targets.

External plugin repositories still use their legacy `gtests` manifests until they
are migrated independently. Every test-enabled SDK installs the test-only
`dsn.legacy.tests` host, GoogleTest headers and libraries, and a shared GoogleTest
registry for those manifests, including standalone external-plugin builds. With
`BUILD_TESTING=OFF`, those artifacts, embedded `*.test.cpp` sources, and GoogleTest
linkage are omitted, and legacy test-only executables are excluded from the default
build.

On Windows, the emulator scheduler tests remain in the test-enabled plugin DLL
because its inline singleton and extension-slot state must be owned by the same
module as the implementation. The standalone test executable runs those
registrations through the shared GoogleTest registry.
