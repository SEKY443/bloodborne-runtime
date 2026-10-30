# iOS proof of concept

On `main` since 2026-10-29 (merged from the `ios-poc` branches of this repository and
bloodborne-recomp; the iOS code behind `TARGET_OS_IPHONE`). Bloodborne running on an iPhone 17 Pro
Max: the runtime, bbcpu and the recompiled library, the Metal renderer, signed with a free Apple
ID. A proof of concept, not a release: nothing here is packaged or distributed; the app is the
private bloodborne-ios repository. How the work is meant to be split between this repository and a future app
repository: bloodborne-mac's `docs/ARCHITECTURE.md` ("Where code lives").

## Where it stands

- The game boots, loads the copied Mac save and plays, with the FPS overlay and touch through
  SDL's mouse emulation.
- The phone prepares the game's executable itself (2026-10-30): the runtime reads
  `Documents/game`'s `eboot.bin` and links its system modules in memory, as on the Mac, so
  `boot-linked.bin` and `content.bin` no longer come from the Mac (`BB_IOS_PREPARE=0` still boots
  them). `patches.bin` still does, until the patches are prepared by the runtime too.
- Gameplay in the Hunter's Dream at 960×540, all effects on (about 1,420 draws a frame):

| Build | FPS |
|---|---:|
| First gameplay | 7.1 |
| All effects off (the same build) | 7.6 |
| The game's mutex as host code (`src/runtime_game_lock.c`) | 16.4 |
| That mutex called without the translator (recomp `825c8c8`) | 16.9 |
| Current recompiler, host `tanf`/`atan2f`/`wmemset`, the 730 functions of the jump-table change left to the interpreter | 23.1 |
| Those 730 functions recompiled too, after the generator fix (recomp `f0f4eb0`) | 30.0–30.7 |
| The interpreter looking up recompiled code at block starts only (recomp `e86a05d`) | 32.7–35.6 |
| Current `main` (2026-10-30: runtime `0dd9ff4`, the library rebuilt with the jump-table work), lossless texture compression on; the first 20 s after load | 37.8–42.6 |

- **Heat:** after about 40 s in the level the frame rate falls by about 30% (to 22.5 before the
  last change, 24.1 after it) at the same CPU use (about 375% of one core) and the same draws:
  the cores slow down. The hottest die sensor (the FPS counter and the `Process:` log line) climbs
  from about 52 °C to 73 °C, and the drop comes there, the die then cooling to about 67 °C. The
  system's thermal state stays "nominal" throughout: it is not the signal to watch.
- **Capped at 30 FPS, the phone throttled, it holds 30** with the GPU command processor and the
  recorders on the performance cores (user-interactive QoS: they had run on the efficiency cores
  about 78% of the time) and the waits' call sites no longer collected for every player (that
  stack scan cost the game's worker threads 3–10%): 29.8–30.0 FPS in both minutes of 2-minute
  runs, the lowest 5-second window 29.0, against 26.9–27.8 and 23.3 before (two alternating
  pairs, one minute apart, the die at 64–69 °C). A Time Profiler trace of the throttled game
  showed where the time went: the five GX workers about a third in semaphore waits and wakes,
  the GPU command processor 20% waiting for host copies, the recorder in system calls from its
  copies and draws.
- **Lossless texture compression** (textures made without the ShaderWrite and PixelFormatView
  usages until a view needs one, adapted from
  [MojahedAbushawish](https://github.com/MojahedAbushawish)'s work for Wooloomooloo2's
  [bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port), #33), four
  alternating 40-second runs, one minute apart: 37.8–39.2 FPS against 33.7–35.8 once gameplay
  settles (+10%), 27.2–28.9 against 25.8–27.3 once the phone throttles (+5%); 4 upgrades a run.
  The Mac gains +33%: the iPhone is bound by its CPU and its heat more than by GPU bandwidth.
  With one minute between runs the phone throttles after about 20 s in the later runs.
- **MetalFX** (640×360 upscaled to 960×540, `upscaler=metalfx`, `preset=1`, a `patches.bin` made
  for 640×360), one 90-second run against native 960×540 from a cool phone: 33.1–34.4 FPS before
  throttling against 29.1–34.8, the drop about 20 s later (about 65 s in the level against 45),
  then 24.6–25.8 against 24.0–24.7, at the same CPU use. The heat comes from the CPU; MetalFX
  delays the drop but does not prevent it.
- **Open:** one run of seven since the generator fix stopped on a host fault in the recompiled
  `f_c0ba10` (address `0x1000000010108`) while the level loaded; the same build ran again
  without it.

- The frame is bound by the CPU, not the GPU: effects off gained 7%, no Metal command buffer took
  25 ms, and the GPU submission thread was idle about 89% of the time at 7 FPS. The PS4 version
  targets 30 FPS.

## What the iPhone does not allow, and what the branch does instead

- **No JIT** (a free team cannot sign the entitlement): bbcpu is built without its JIT
  (`BB_JIT=0`, compiled out under `TARGET_OS_IPHONE`). The recompiled library runs the game's code
  and the interpreter runs the rest: the PS4's own libraries (libc and the others in
  `boot-linked.bin`), functions changed by patches, and what the generator leaves out.
- **No extended address space or larger memory limit** (both are entitlements a free team cannot
  have): the app has a 3.29 GiB memory limit, and the free address space is one block of 6 GiB at
  `0x7000000000`, measured before any allocation. Guest memory is laid out in it
  (`src/platform.h`): 512 MiB of low memory, then the game's range, with guest addresses equal to
  host ones and one file mapping as both the game's memory and Metal's view of it. File-backed
  shared memory does not count against the limit, GPU writes included. 4,608 MiB of direct memory
  (the game allocates about 4,589 MiB), a 384 MiB flexible span, the unused tail given back, the
  trap tables only over the game's range, one Metal buffer for the pool (`BB_POOL_SPLIT=0`).
- **No executable guest memory, no shared-memory objects, no `mach_vm` calls, no D24 depth
  formats, no `displaySyncEnabled`:** each replaced or guarded where it is used.
- **Wired memory is killed by jetsam:** the pool is not split into the pinned no-copy buffers the
  Mac uses.
- **The system keyboard runs out of address space:** SDL's text input is not started on iOS (the
  app closed when the character name entry opened it).

## Findings that apply beyond the iPhone

- **The game's spin-then-sleep mutex** (lock `0x20368c0`, unlock `0x2036a40`; the game's heap and
  other subsystems take it) spins until a TSC deadline before sleeping. With 6 cores (2 fast) and
  the game's code slower than on a PS4, eight threads spun on it and starved the thread holding
  it: 57% of the CPU in a Time Profiler trace. The host version keeps the lock word and the game's
  semaphore and spins 128 times with the CPU's `yield` before sleeping. On the Mac it costs
  instead (translator, copied save, 30 s: 414% of one core against 380% with the game's own,
  31.1 against 32.2 FPS): the JIT runs the game's lock as fast inline code, 12 cores leave little
  spinning to save, and the host version costs a trip out of the JIT at every lock and unlock. It
  stays for platforms without a JIT and with few fast cores.
- **Natives called from recompiled code** went through the translator; now they are called
  directly, as host imports are (recomp `825c8c8`).
- **Jump tables and calls that never return** (recomp `f0f4eb0`, on main): the recognizer now
  takes `cmp; jbe TABLE`, bounds up to 32 instructions back and an index copied before the
  `cmp`, which brings 135,000 more instructions into C. Decoding stops after calls to
  `__stack_chk_fail`, `abort`, `exit` and `_exit` (it used to run on into jump table data), and an
  instruction whose next one in address order is not where it continues jumps there explicitly.
- **Thread QoS** (`BB_THREAD_QOS=interactive|initiated`; by default the system's choice, which is
  user-initiated, `0x15`): every thread as user-interactive measured 27.1–29.0 FPS against
  30.0–30.7 and paced worse (a 10-second window with a median frame of 50 ms). `BB_THREAD_QOS_LOG`
  prints each thread's class.
- **Host libc functions** (`src/runtime_libc.c`): the PS4 libc's busiest functions as host code,
  for 16-bit `wchar_t`. They cut interpreted calls from 130,000 to 52,000 a second without
  changing the frame rate on their own.

## Next steps

1. Less CPU work for the same frames, since heat is what limits the frame rate now: the
   interpreter still had 13% of the samples (Havok's workers spend 45% of their time in it), so
   find what it runs (the PS4's own libraries, functions left out) and recompile it.
2. The generator's CPU work (the app's `docs/ROADMAP.md`, section 8), which helps the Mac too.
3. Other spinning waits, found the way the mutex was.
4. QoS per thread (only the main and render threads), if a profile shows them on slow cores.

## Build and run

The app (its shell, build, signing and test scripts) is
[bloodborne-ios](https://github.com/bloodborne-native/bloodborne-ios) (private), with this
repository as its `runtime/` submodule. What stays here are the iOS builds of the runtime's own
parts:

```sh
bash scripts/ios/build-deps.sh                       # deps/ios-arm64 from deps/build/src
bash scripts/ios/build-gpu.sh                        # out/ios/gpu/libbbgpu.a (BB_FSR off: no FSR on iOS)
bash scripts/ios/build-core.sh                       # out/ios/libbbios.a
bash scripts/ios/recomp.sh                           # out/ios/librecomp.dylib (BB_RECOMP_GEN: the C)
```

Generated C, the recompiled library, logs and traces stay in the ignored `out/`.
