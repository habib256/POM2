# POM2 core SDK policy

POM2 exposes one optional C++17 facade: `include/pom2/core.hpp`, implemented by
`src/Pom2Core.cpp`. It is a PIMPL wrapper over `Memory` + `M6502` + the small
set of devices an embedding host needs (display, speaker, cassette, Disk II,
Mockingboard), so a consumer never sees an internal definition.

Its contract is pinned by the `pom2_core_api` test, which drives the facade
through a boot, a display frame, audio pulls, a disk mount and a full cassette
record/save/clear cycle, and asserts the header stays self-contained.

## What is deliberately not here

There is **no installed `pom2_core` package yet** — no `find_package(pom2_core)`,
no `POM2::core` imported target, no standalone consumer example. Those exist on
`refactor/core-boundaries-and-coordinators`, where they rest on that branch's
layered CMake (media / machine / devices / runtime built as one-way object
libraries). The v0.8.5 tree builds its sources as flat lists into the
executables and has no installable library target for them to attach to, so the
install half of the SDK lands with that layering, not before it.

The repository likewise contains no registry of external consumers and no
compatibility matrix. That is not proof no external user exists, but it is not
evidence for maintaining several public binary interfaces either.
