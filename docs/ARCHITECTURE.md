# POM2 source architecture

POM2 exposes and installs one `pom2_core` static archive. Internally, its
sources compile in layer-specific CMake `OBJECT` targets, composed in this
direction:

```text
foundation headers <- media <- machine <- devices <- runtime <- frontend
```

A layer may include itself or anything to its left. It may never include a
layer to its right. `cmake/Pom2Architecture.cmake` checks this at configure
time. It also rejects quoted first-party headers that have not been assigned
to a layer, so adding a header cannot silently bypass the policy. Machine and
device sources are additionally forbidden from including worker-thread and
host-network system headers directly; host transports must be injected through
a classified contract.

## Responsibilities

- `media`: host-file representations and codecs—disk images, ProDOS volumes,
  snapshot containers and resource-path resolution. It does not know a CPU or
  expansion card.
- `machine`: deterministic motherboard state and built-in hardware—CPUs,
  `Memory`, timing, `SlotBus`, IWM/Sony routing, speaker and cassette signal
  generation. It has no window or OS audio device.
- `devices`: optional expansion-card implementations. They may use machine
  contracts and media backings, but not worker threads or frontend code.
- `runtime`: composition and host services—`pom2::Core`,
  `EmulationController`, pacing threads, OS audio, sockets/process transports,
  rewind orchestration and host-side printer rendering.
- `frontend`: ImGui, GLFW/OpenGL, panels, input discovery and presentation.

Header-only seams shared by several layers live in the explicit
`POM2_FOUNDATION_HEADERS` manifest. This is deliberately small; it is not a
general-purpose dumping ground.

## Frontend coordination

`MainWindow` remains the frontend composition root, but no longer owns the
mutable policy for every subsystem. Ten coordinators carry explicit state and
lifecycle contracts:

- `AudioCoordinator` owns the authoritative audio-source registration and
  teardown inventory, discovers every Mockingboard/Phasor/Echo card from the
  live `SlotBus`, exposes locked immutable inspector/mixer snapshots, applies
  commands by `(type, slot)` and persists independent per-slot levels;
- `StorageCoordinator` owns the live Disk II/HDV/CFFA/SmartPort topology,
  preferred-device policy, flush policy, session-only provisioning markers and
  the storage panels' locked snapshot/command boundaries. It also captures the
  value-only Disk II/HDV/CFFA media snapshot used by profile, slot rebuild and
  shutdown paths, models both drives of every Disk II, synchronizes settings
  and restores cards by slot. Its topology views are ephemeral and are never
  retained across a rebuild;
- `DevicePanelCoordinator` resolves Chat Mauve, Uthernet I/II, ClockCard and
  every Super Serial card from the live `SlotBus`, captures immutable frame
  values and applies returned commands in one short state-lock section;
- `MouseCoordinator` owns the renderer-free host-pointer boundary: it resolves
  both mouse implementations from the live `SlotBus`, copies inspector and
  screen-hole state under `lockState()` and routes input without retaining a
  concrete card pointer;
- `NetworkCoordinator` owns backend selection, FujiNet helper lifecycle,
  host-side discovery/status and the FujiNet panel snapshot/command boundary;
- `PrinterCoordinator` owns printer-cable arbitration, streaming cursors and
  Grappler BUSY/DIP handover across Printer, Grappler, FujiNet and SSC sources;
- `SlotConfigurationCoordinator` owns the effective slot plan after settings,
  profile fixtures and uniqueness rules are applied, plus the explicitly
  staged editor draft. Neither is machine topology: its immutable live view is
  copied from `SlotBus` under the machine lock;
- `SlotProvisioningCoordinator` owns additive, session-only topology changes
  caused by explicit boot intent. It selects the profile-valid HDV/SmartPort
  target, chooses a free slot, constructs through `SlotCardFactory`, wires the
  device and marks it non-persistent without altering the effective plan;
- `SlotRebuildCoordinator` owns the topology transaction shared by profile
  switches and Slot Configuration Apply. After media durability succeeds, its
  phase machine invalidates rewind/session topology, gates AI endpoints,
  detaches audio/UI consumers, clears `SlotBus` under `StateAccess`, retires
  network/display state, and republishes endpoints only after reconstruction;
- `DebugCoordinator` owns debug tools and their lock/write handover protocol.

They are frontend policy objects, not renamed fragments of `MainWindow`: each
owns an invariant or resource lifecycle and can be tested without rendering a
window.

Panels use a data-in/command-out boundary wherever card state crosses the CPU
worker seam. A coordinator resolves the card under `lockState()`, copies a
snapshot by value, releases the lock before ImGui renders, then re-resolves the
card to apply the command. No card or unit pointer from such a panel survives a
slot rebuild. Persistent-setting updates are performed after the machine lock
is released. This pattern currently covers Chat Mauve, Uthernet I/II, Super
Serial, ClockCard, both Mouse Card implementations, SmartPort, storage
inventory, FujiNet and the complete printer path, in addition to the older
self-contained disk, HDV,
cassette and joystick panel contracts.

Slot configuration uses three non-interchangeable values. `effectivePlan()` is
the resolved persistent intent, `draft()` contains unapplied UI edits, and
`captureLive()` describes only the cards currently owned by `SlotBus`. Missing
ROMs, fallback implementations, CLI FujiNet and auto-provisioned media cards
may make the live snapshot differ from the plan, but never mutate that plan.

Configured ROM-sensitive card construction is also outside `MainWindow`.
`SlotCardFactory` owns resource discovery, firmware validation and explicit
implementation fallbacks for Disk II, HDV, CFFA, Grappler, SmartPort and both
Mouse Card variants. Its resource locator is injected, so tests exercise missing,
valid and fallback firmware deterministically. Card construction is media-free:
after the complete replacement topology exists, `StorageCoordinator` restores
Disk II, HDV, CFFA and SmartPort paths/write-back settings in one explicit phase. This
same ordering is used at startup, profile switches and Slot Configuration
Apply. During a profile switch the captured live overlay wins even for empty
Disk II/HDV/CFFA media, preventing stale settings from remounting an ejected
image. `MainWindow` remains the composition root only for live wiring—CPU,
audio, transports and final `SlotBus` ownership.

Immediate storage actions follow the same boundary. Disk II and generic media
commands are addressed by slot plus drive/bay and executed by
`StorageCoordinator`; it re-resolves the target under `lockState()`, performs
mount/eject/write-back/type changes, copies the resulting persistence updates,
then releases the machine lock before updating and saving `Settings`. Menus,
the disk library, kiosk mode, file dialogs, Floppy Emu and the immediate media
panel therefore share one command policy and never mutate Disk II, HDV or CFFA
objects directly. Eject-all visits both drives of every Disk II card and all
block/SmartPort media, while preserving any image whose write-back fails.

The same coordinator owns the effective 3.5-inch target. A plugged SmartPort
card is authoritative; otherwise the `EmulationController` on-board pair is
used. `captureDisk35()` and all mount/eject/write-back/WOZ-conversion commands
share that decision, eliminating the former case where a mount targeted
SmartPort while the panel displayed the on-board drive. SmartPort hardware is
also constructed empty: unit types, paths and write-back settings join the
explicit post-topology restoration phase instead of being interpreted inside
`plugSlotsFromSettings`.

The 38 panel visibility flags and panel-local working data (textures, HGR
buffers, print cursors, transient status, kiosk navigation and host mouse
capture/routing) live in an opaque `MainWindowUiState`. `MainWindow.h`
therefore exposes one ownership
edge instead of every widget field; panel hosts can later take ownership of
smaller sub-states without changing the public class layout.

The rendering/composition code is physically split into bounded translation
units:

- `MainWindow.cpp`: construction, teardown, slot composition and frame
  orchestration;
- `MainWindow_Command.cpp`: command palette, docking, menus and status bar;
- `MainWindow_Screen.cpp`: screen, kiosk and pointer routing;
- `MainWindow_DevicePanels.cpp`: audio, network, printer, video and expansion
  device inspectors;
- `MainWindow_Media.cpp`: Disk II, 3.5-inch, HDV, SmartPort, FujiNet and media
  dialogs;
- `MainWindow_Input.cpp` and `MainWindow_AuxPanels.cpp`: host input/texture
  upload and the smaller keyboard/about/editor/cassette/rewind surfaces;
- `MainWindow_Slots.cpp` and `MainWindow_MemoryMaps.cpp`: slot/profile policy
  UI and memory visualisations.

Every `MainWindow*.cpp` is capped at 3,000 physical lines by the configure-time
architecture check and `architecture_mainwindow_tu_size` CTest. A growing
responsibility must therefore gain a new coherent TU instead of recreating the
god-object under another filename.

## Frontend concurrency gate

The `frontend-tsan` CI job instruments the complete Linux frontend with
ThreadSanitizer and runs the renderer-free concurrency contracts. The primary
test combines the real CPU worker, an AI-like parallel state reader, immutable
device snapshots/commands and repeated Chat Mauve/Uthernet/Super Serial/
SmartPort/FujiNet/Printer/Grappler/Disk II/HDV/CFFA, Mouse/Clock and audio-card
replug.
The campaign also drives a real headless ImGui Memory Viewer frame, rewind
transport, audio-source teardown and disk-path publication. GLFW/OpenGL code is
compiled and instrumented but no display server is required at runtime, which
keeps the race gate deterministic on PR workers.

## Machine input boundaries

`Memory` owns only address decoding for the `$C0xx` input switches. `Keyboard`
owns the latch, strobe and thread-safe paste queue; `PaddleInputs` owns button
and modifier state plus the cycle-based RC timers. Their behaviour is pinned by
the mirrored/strobe `$C0xx` contract tests before changes to bus routing.

## Build targets

- `pom2_foundation` is an internal `INTERFACE` target carrying the neutral
  C++ and include-path contract;
- `pom2_media`, `pom2_machine`, `pom2_devices` and `pom2_runtime` are internal
  `OBJECT` targets with exclusive ownership of their source manifests;
- `pom2_core` aggregates those objects into the only public/installable static
  archive, exported as `POM2::core`;
- `pom2_imgui` is the frontend executable and consumes `POM2::core`.

`POM2_BUILD_FRONTEND=OFF` configures a renderer-free core/headless build: it
does not require Dear ImGui, GLFW or OpenGL. The public SDK policy, compatibility
contract and evidence required before splitting the archive live in
[`SDK.md`](SDK.md).

Native tests use assertion-enabled twins of the four object targets. This
preserves their `-UNDEBUG` contract without compiling the core once per test or
changing production objects. Platform libraries remain dependencies of the
aggregate archives, so internal implementation targets do not leak into the
installed SDK.

## Extracted host boundaries

`AudioSource` and `RateAware` already follow that pattern: their neutral
contract lives in `AudioSource.h`, while `AudioDevice` and miniaudio device
ownership stay in runtime.

`SuperSerialCard` now follows it as well. The deterministic 6551 registers,
FIFO queues, IRQ state and slot ROM compile in `devices`; `SuperSerialTransport`
is the injected device-side contract; `SuperSerialTcpTransport` in `runtime`
owns BSD/Winsock handles, the listener thread and wall-clock baud pacing. The
frontend and headless composition roots install that adapter explicitly. A
card constructed without a transport remains usable for deterministic tests
and snapshots, but cannot open a host listener.

`FujiNetCard` is likewise a `devices` source now. `FujiNetLink` is its narrow
device-side SmartPort request contract; it contains no transport selection,
thread, socket, serial-port or process API. `SpOverSlipLink` remains in
`runtime` and implements that contract, while `FujiNetHost` owns helper-process
discovery and supervision. The frontend is the composition root: it configures
the concrete runtime link, injects it into the card, and talks to the host
service for helper lifecycle. A card without an adapter installs a disconnected
null link and deterministically reports “no device”.

`UthernetIICard` and `W5100Device` now compile in `devices`. Their
`W5100Socket` contract contains only protocol-neutral addresses, byte buffers
and outcomes; `W5100HostSockets` in `runtime` owns BSD/Winsock handles,
non-blocking polling and asynchronous DNS. The frontend injects that factory.
Without it, TCP/UDP OPEN remains CLOSED while deterministic registers and the
MACRAW/IPRAW `NetworkBackend` path continue to work.

Reusable deterministic implementations of these seams live under
`tests/fakes/`. Device tests inject them directly, so protocol marshalling,
reset/resynchronisation and connection state can be exercised without opening
host sockets, serial listeners or helper processes.

## Adding or moving code

1. Add each `.cpp` to exactly one `POM2_*_SOURCES` manifest.
2. A same-stem `.h` is assigned automatically. Add header-only contracts to
   the narrowest applicable `POM2_*_HEADERS` manifest.
3. Configure CMake. An unclassified header or upward include is a hard error.
4. Move a source upward only when it genuinely owns higher-level policy. To
   move it downward, first replace higher dependencies with lower-layer
   interfaces.

The layers intentionally remain object targets rather than separately shipped
libraries. A future split into static/shared artifacts should happen only if a
real embedding or deployment need justifies four additional binary interfaces.
