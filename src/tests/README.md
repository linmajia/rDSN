
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
are migrated independently. Plugin-enabled test builds provide the test-only
`dsn.legacy.tests` host and a shared GoogleTest registry for those manifests. With
`BUILD_TESTING=OFF`, their embedded `*.test.cpp` sources and GoogleTest linkage are
also omitted, and legacy test-only executables are excluded from the default build.
