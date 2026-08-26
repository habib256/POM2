# POM2 core SDK policy

POM2 currently exposes one optional C++17 SDK artifact: the static archive
`pom2_core`, imported by consumers as `POM2::core`. Its only supported public
header is `<pom2/core.hpp>`.

The repository contains an installed-package contract test and a standalone
consumer example. It does not contain a registry of external consumers, an
external compatibility matrix or a separately versioned SDK release. That is
not proof that no external user exists, but it is not evidence for maintaining
five public binary interfaces either.

## Boundary

The SDK is the `pom2::Core` facade, not the internal source layout:

- `pom2_media`, `pom2_machine`, `pom2_devices` and `pom2_runtime` are internal
  CMake `OBJECT` targets and are not installed;
- headers under `src/` are private and have no compatibility guarantee;
- the desktop frontend consumes the same `pom2_core` archive as an embedding
  application;
- `pom2::Core` is host-driven and single-threaded: it opens no window, OS audio
  device, socket or process, and performs no wall-clock pacing.

The archive can contain runtime implementation members without forcing them
into a consumer executable: static-link archive members that are not referenced
are not linked. The stable facade and its behaviour are therefore the useful
boundary; multiplying archive files would not make that boundary cleaner.

Before 1.0, intentional public API changes are documented in `CHANGELOG.md` and
may occur in a minor release. Installed package compatibility follows CMake's
`SameMajorVersion` rule. Private headers and internal target names may change at
any time.

## Verification

Configure a renderer-free SDK build and run its external-consumer contract:

```bash
cmake -S . -B build-core-sdk \
  -DPOM2_BUILD_FRONTEND=OFF \
  -DPOM2_ENABLE_TESTS=ON \
  -DPOM2_INSTALL_CORE_SDK=ON
cmake --build build-core-sdk --target pom2_core test_pom2_core_api
ctest --test-dir build-core-sdk -R 'pom2_core_(api|sdk_consumer)' \
  --output-on-failure
```

`pom2_core_sdk_consumer` installs the SDK into a private staging prefix,
configures `examples/pom2_core_consumer` only through `find_package`, builds it
and runs it. This catches leaked private includes, incomplete install exports
and missing transitive platform dependencies.

## When to split

Split the artifact only for a demonstrated external requirement, such as a
deployment that must omit host runtime code, or a runtime that needs an
independent version and release cadence. Introduce shared libraries only when
POM2 must distribute and support an actual ABI. Until then, source-layer
separation plus one installed archive is the lower-cost and safer contract.
