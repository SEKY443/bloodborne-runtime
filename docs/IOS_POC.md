# iOS proof of concept

Branch `ios-poc` (this repository and bloodborne-recomp). Bloodborne running on an iPhone 17 Pro
Max: the runtime, bbcpu and the recompiled library, the Metal renderer, signed with a free Apple
ID. A proof of concept, not a release: nothing here is packaged or distributed, and the branch is
not merged. How the work is meant to be split between this repository and a future app
repository: bloodborne-mac's `docs/ARCHITECTURE.md` ("Where code lives").

## Where it stands

- The game boots, loads the copied Mac save and plays, with the FPS overlay and touch through
  SDL's mouse emulation.
- Gameplay in the Hunter's Dream at 960×540, all effects on (about 1,420 draws a frame):

| Build | FPS |
|---|---:|
| First gameplay | 7.1 |
| All effects off (the same build) | 7.6 |
| The game's mutex as host code (`src/runtime_game_lock.c`) | 16.4 |
| That mutex called without the translator (recomp `825c8c8`) | 16.9 |
| Current recompiler, host `tanf`/`atan2f`/`wmemset`, the 730 functions of the jump-table change left to the interpreter | 23.1 |
| Those 730 functions recompiled too, after the generator fix (recomp `f0f4eb0`) | 30.0–30.7 |

- 30 FPS is the game's own cap. Over 40 s the last 10-second sample fell to 22 in each run.

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

1. A new profile at 30 FPS: what still holds the frame, and why it drops after a while (heat).
2. MetalFX at a lower render resolution (less GPU time and heat at the same frame rate).
3. The PS4's own libraries through the recompiler (they are interpreted).
4. Other spinning waits, found the way the mutex was.
5. QoS per thread (only the main and render threads), if the profile shows them on slow cores.

## Build and run

Needs the macOS build first (`bash build.sh`; its dependency sources are reused), Xcode, an
iPhone in developer mode and an Apple ID added to Xcode (a free one works; apps are re-signed
every 7 days).

```sh
bash scripts/ios/build-deps.sh                       # deps/ios-arm64 from deps/build/src
bash scripts/ios/build-gpu.sh                        # out/ios/gpu/libbbgpu.a
bash scripts/ios/recomp.sh                           # out/ios/librecomp.dylib (BB_RECOMP_GEN: the C)
BB_IOS_TEAM=<team> BB_IOS_DEVICE=<udid> bash scripts/ios/build-app.sh   # builds, signs, installs
```

The app's `Documents` (Finder's Files tab, or `xcrun devicectl device copy to ... --domain-type
appDataContainer`) holds `game/` (the dump), `boot-linked.bin` and `content.bin` from the Mac's
`out/`, `patches.bin` written by `scripts/patches.py` for the phone's resolution (with Skip
Intro), and the saves (`mac-save/` for a copy of the Mac's, `user/` for the phone's own).

- `scripts/ios/trial.sh`: one gameplay measurement (launch, press into gameplay, FPS, stop).
- `scripts/ios/bisect.sh`: the recompiled function behind a crash.
- `scripts/ios/press.sh`: a button press from the Mac (`Documents/pad`, `BB_PAD_FILE`).
- `scripts/ios/profile.py`: a Time Profiler trace summarized by symbol.
- The app's switches (`ios/app_main.c`): `BB_IOS_SAVE`, `BB_IOS_CPU_ONLY`, and the probes
  `BB_IOS_PROBE_ONLY`, `BB_IOS_METAL_TEST`, `BB_IOS_FILE_TEST`, `BB_IOS_GUARD_TEST`. devicectl
  keeps environment variables between launches, so a switch is turned off with `0`, not by
  leaving it out.

Generated C, the recompiled library, logs and traces stay in the ignored `out/`.
