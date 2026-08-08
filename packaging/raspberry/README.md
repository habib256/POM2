# POM2 on a Raspberry Pi — build, tune, measure

Three separate things, in the order they are worth doing:

| Step | Script | What it buys |
|------|--------|--------------|
| Build for the actual core, with a profile | `build_native_pi.sh --pgo` | ~40 % off the emulation core vs the generic AppImage |
| Take the stutter out of the system | `pi_tuning.sh` | removes governor ramps, IRQ contention, swap hitches |
| Check you actually got it | `pom2_bench` | see [docs/PERFORMANCE.md](../../docs/PERFORMANCE.md) |

The measurements, the profiling recipe and the reasoning behind each
optimisation live in **[docs/PERFORMANCE.md](../../docs/PERFORMANCE.md)**.
This file is only the Pi-side how-to.

---

## 1. Build natively, with PGO

The released `POM2-v*-aarch64.AppImage` is built for **generic aarch64** — it
has to run from a Pi 3 to a Pi 5. Building on the Pi itself lets GCC use the
real core (`-mcpu=cortex-a72` on a Pi 4/400, `cortex-a76` on a Pi 5) and, far
more importantly, use a **profile**.

```sh
git clone <repo> POM2 && cd POM2
./setup_imgui.sh                                   # one-time: deps + pinned imgui
sudo apt install libglfw3-dev libgles2-mesa-dev    # GLES tier, see below
packaging/raspberry/build_native_pi.sh --pgo       # 2 passes + LTO, ~40-60 min on a Pi 4
sudo packaging/raspberry/build_native_pi.sh --pgo --install    # → /opt/POM2
```

What the script does, and the traps it closes:

* picks `-mcpu` from `/proc/device-tree/model` rather than trusting
  `-mcpu=native` (on some 64-bit kernels the MIDR GCC reads is incomplete and
  detection silently falls back to generic), and falls back to generic if the
  compiler rejects the value;
* caps `-j` by **RAM, not core count** — `-j4` on this codebase OOM-kills a
  4 GB Pi (`c++: fatal error: Killed signal terminated program cc1plus`);
* forces `-DPOM2_GLES=ON`. This is not optional on a Pi: Mesa's V3D caps
  *desktop* GL at 3.1, so POM2's GL 3.2 core context request fails outright
  and no window ever opens;
* keeps both PGO passes in **one build directory**, and copies the profiles
  from the `pom2_bench` objects onto the `pom2_imgui` ones. Both are load-
  bearing — see the two traps documented in
  [docs/PERFORMANCE.md § 5](../../docs/PERFORMANCE.md#5-the-build-recipe-pgo--lto);
  without them the build silently produces a binary with no gain at all;
* **fails** if any of `M6502`, `Memory`, `DiskIICard`, `DiskImage`,
  `Apple2Display` came out of training without a profile.

Training runs `pgo_train.sh`, which sweeps ][+ and //e banners, PAL and NTSC,
every video pipeline, and a 5.25" boot. If your checkout has no `.dsk` under
`disks_5.4/`, the script says so loudly: with the Disk II LSS untrained, a
PGO build can be *slower* than an untrained one on disk-heavy workloads.

Without `--pgo` you still get the `-mcpu` build (worth ~10-20 %):

```sh
packaging/raspberry/build_native_pi.sh          # plain
POM2_LTO=1 packaging/raspberry/build_native_pi.sh   # + LTO (long link)
```

Environment overrides: `POM2_PREFIX` (default `/opt/POM2`), `POM2_BUILD_DIR`
(default `build-pi`), `POM2_JOBS`.

---

## 2. Tune the system

```sh
sudo packaging/raspberry/pi_tuning.sh        # governor + IRQ pinning + swap off
sudo packaging/raspberry/pi_tuning.sh --uninstall
```

Governor `performance` (Pi OS ships `ondemand`, whose frequency ramps produce
exactly the periodic "it goes in jerks" stutter on a 60 Hz bursty load), hard
IRQs pinned to core 0 via `irqaffinity=0`, swap off (one swap-in mid-frame on
an SD card is a guaranteed audio dropout). The cmdline edit needs a reboot;
everything else is live. It is idempotent and fully reversible.

It deliberately does **not** install a kiosk session or touch the audio stack:
POM2 has kiosk mode built in (`F10`, or `--kiosk`), and a session launched
with `--kiosk` is settings-read-only for its whole life, so it cannot disturb
a desktop setup.

---

## 3. Measure

```sh
/opt/POM2/bin/pom2_bench --frames 3000 --quiet                    # CPU + bus
/opt/POM2/bin/pom2_bench --disk <image>.dsk --frames 900 --quiet  # + Disk II LSS
```

The `speed=… MHz (…x)` figure is emulated CPU throughput; `1.0x` is realtime
for an Apple II. The `ram=` / `fb=` hashes must not change between builds — if
they do, the faster binary is not the same emulator.

> ⚠ These scripts have **not yet been run on real Pi hardware**. The build
> recipe and its two PGO traps were validated end to end on x86-64 (same GCC
> semantics, same CMake object layout); the `-mcpu` selection, the RAM-based
> `-j` cap and everything in `pi_tuning.sh` are Pi-specific and untested.
