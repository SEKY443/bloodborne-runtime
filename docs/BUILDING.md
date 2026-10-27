# Building, running and testing

[← Documentation](README.md)

## Requirements

- macOS on Apple Silicon (macOS 15 or newer), or Linux with Nix (`shell.nix`; Linux is built in
  CI but not played on).
- macOS: Xcode Command Line Tools and [Homebrew](https://brew.sh):

  ```bash
  brew install bash pkgconf glslang nasm cmake ninja spirv-tools clang-format
  ```

  FSR 4.1.1 on Metal also needs Xcode with its Metal Toolchain
  (`xcodebuild -downloadComponent MetalToolchain`), even when `xcode-select` points at the
  Command Line Tools. Without it, or without `spirv-tools` or `clang-format` (DXC needs it),
  the build skips FSR 4.1.1 with a warning, and the game reports it unavailable.

- Your own dump of the game with update 1.09, prepared as in
  [Game files](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/guide/game-files.md).

## Build and run

```bash
git clone --recursive https://github.com/bloodborne-native/bloodborne-runtime.git
cd bloodborne-runtime
bash build.sh                                  # the first run also builds deps/ (about an hour)
BB_GAME_DIR=/path/to/your/game bash run.sh
```

- `build.sh` builds `out/bb-probe`, the GPU library and bbcpu (from the `recomp/` submodule).
  `bash build.sh --test` also runs the native tests.
- `deps/` (MoltenVK, SDL3, FFmpeg, ...; for FSR 4.1.1 on Metal also DXC, the spirv-cross command
  and the fsr4-native-macos tree, without AMD's initializers) is built by
  `scripts/macos/build-deps.sh`. Its libraries
  record absolute paths: a moved or copied `deps/` must be rebuilt (remove `deps/macos-arm64`,
  then `BB_ARCH=arm64 bash scripts/macos/build-deps.sh`).
- On macOS `build.sh` applies `gpu/patches/fsr-vulkan-macos/0001-use-vulkan-headers-without-loader-on-macos.patch`
  to the FSR-Vulkan submodule once. It links that library to Vulkan headers, while the native
  Metal path loads no Vulkan library. The patch is checked before application, so a repeated
  build leaves the submodule pointer unchanged. Linux uses the existing Vulkan build path and
  does not apply this macOS patch.
- From a checkout, saves and caches go to `user/` and settings to `bbport.ini`.
- The macOS app (launcher, DMG, releases) is
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac), which builds this
  repository.

## Environment

`run.sh` reads its options from the environment; the launcher sets them from its settings.

| Variable | Effect |
|---|---|
| `BB_GAME_DIR` | The game folder |
| `BB_OUTPUT_RES=WxH`, `BB_RENDER_RES=WxH` | Output size, and the game's render size (with `BB_RENDER_RES`, also set `BB_DMEM_MB`) |
| `BB_UPSCALER=fsr3\|fsr411\|metalfx\|off`, `BB_UPSCALE_PRESET` | Upscaler and preset |
| `BB_FSR411_DIR` | AMD's FSR 4.1.1 files: the Vulkan path's set, or for FSR 4.1.1 on Metal `initializers.bin` and `initializers-ultraperformance.bin` (`tools/fsr4_metal_initializers.sh <the player's amd_fidelityfx_upscaler_dx12.dll>`) |
| `BB_FSR4_METAL_SYNC=1`, `BB_FSR4_METAL_DIR` | FSR 4.1.1 on Metal with CPU waits instead of shared events; its built files (default `out/fsr4_metal`) |
| `BB_FPS=uncap\|90\|60\|30`, `BB_FPS_LIMIT` | Frame-rate mode and limit |
| `BB_METAL_FRAME=1`, `=0` | Native Metal (default on Apple Silicon, no Vulkan loader mapped) or the MoltenVK fallback |
| `BB_GFX_BACKEND=metal`, `=vulkan` | The render adapter backend, selected by `run.sh` with the renderer |
| `BB_VULKAN_LOADER` | Path to the optional Vulkan loader on the MoltenVK path; `run.sh` sets it from `deps/` or the app bundle |
| `BB_RECOMP_LIB=PATH` | Load the recompiled library |
| `BB_LANGUAGE`, `BB_UI_LANGUAGE` | The game's language (PS4 code), the menu's (`en`, `pt-BR`, ...) |
| `BB_MODS_DIR`, `BB_MODS_ENABLED=0` | Mods folder, or no mods |
| `BB_PATCHES_DIR` | Third-party patches folder |
| `BB_FRAME_STATS=1` | Frame statistics in the log |
| `BB_SKIP_GAME_CHECK=1` | Skip the game folder check |

Switches for single features are listed where the feature is described in
[Architecture](ARCHITECTURE.md).

## Tests

- `bash build.sh --test`: the native tests (settings, the AV player's clock and queue, stable
  slot storage, texture upload hashing, ...). They need no game or GPU and also run in Linux CI,
  which is started by hand for now; it compiles the Linux build but does not check its rendering.
- `cd tests && python3 -m unittest discover -p 'test_*.py'`: the script tests. On macOS the
  Linux-only cases fail (`test_packaged_vulkan`, the Intel tonemap check), and
  `test_probe.LoaderTests` fails (its images use 4 KiB segments; Macs have 16 KiB pages): known,
  not regressions.

## Running the game for tests

Tests use English (`BB_LANGUAGE=1 BB_UI_LANGUAGE=en`; the test scripts set it). Run one game at
a time (`pgrep -x bb-probe` first). Test windows open on the screen.

- `tools/test_run.sh NAME [VAR=VALUE...]`: a background run with a **copy** of the player's save
  (the real one is never written). It boots, loads the save, waits for the level (two frame-stat
  windows above `LEVEL_DRAWS=200` draws a frame), holds there (`HOLD=45` seconds) and prints the frame summary and any fault. `SAVE_FROM=DIR` starts from a launcher
  backup instead; `KEEP_SAVE=1` keeps the previous test's progress. The test copy of the
  settings is `out/test-user/bbport.ini` (keep `show_fps=1` and `fps_size=big` there).
  `TEST_TITLE="..."` (default: NAME) and `TEST_DETAIL="..."` put a banner at the top of the
  game's window saying what the run tests (`BB_TEST_TITLE`, `BB_TEST_DETAIL`: the overlay's
  test banner), for whoever sees it; it is in `grab.sh --screen` pictures, not in game frames.
  Each line is centered; under them a clock counts the run's time (light), then the time left
  once the hold starts (amber, red in the last 10 seconds; `BB_TEST_CLOCK`: a file where the runner writes when it will close the game, in seconds since
  1970). Each frame-stats window also logs `Process:`: the game's CPU time (100% = one core), the
  hottest die sensor and the system's thermal state.
- `tools/test_opening_video.sh NAME`: a new save in a fresh folder, through the opening video to
  character creation. Reports whether playback completed. `VIDEO_GRAB=1` (in the environment,
  not as an argument) saves the character creator's frame. Part of release testing.
- `tools/grab.sh [--screen] NAME`: saves the game's own frame as a PNG (`--screen` includes the
  in-game menu and the counter). Pictures come from the game, never from the screen.
- Test games are background-only apps (they never become the active app, so focus and Cmd+Q
  behave unlike a launcher game). For a test someone plays in, pass `BB_BENCH_FOREGROUND=1`.
- Scripted menu keys: `BB_MENU_KEYS_FILE=$PWD/out/menu.keys` with tokens like
  `r1 r1 down enter`; `touch out/menu.trigger` opens or closes the menu.
- `tools/mac_bench.sh`: what `test_run.sh` runs underneath (`SAMPLE=1` takes a CPU sample,
  `TRACE=1` a Metal System Trace).

## Measuring

The game is GPU-bound on Apple Silicon, so CPU-side changes are measured in CPU time. Compare
alternating runs (A, B, A, B) at the same place with the same save and settings: FPS, frame-time
p90, game CPU (100% is one core) and the GPU thread's time per draw (`GPU thread us/draw` in the
log). A healthy run in the Hunter's Dream on an M3 Pro is about 58–62 FPS.

Runs back to back are not equal: the second of a pair reads about 5% more GPU-thread time per
draw as the Mac warms up (three base-then-new pairs showed a 5% difference that a new-then-base
pair did not, 4.42 against 4.36 us). Alternate which build goes first before calling a cost.

- Metal System Traces: `TRACE=1`, read with `tools/mst_summary.py`, `tools/mst_passes.py TRACE
  --complete-frames` and `tools/mst_shaders.py`. `xctrace` needs
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`; recordings take over a minute to
  finalize after their time limit.
- Metal API validation (object lifetimes): `MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=nslog`,
  then search the log for "being destroyed while still required".

What has been measured so far: [Performance](PERFORMANCE.md).
