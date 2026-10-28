# Native renderer (milestone R5)

[← Documentation](README.md)

**Goal:** the game's graphics drawn with each platform's own GPU API, with nothing translated
while the game runs: no MoltenVK on macOS, no shaders compiled during play. The renderer stays
multiplatform: one shared core, and thin backends for Vulkan (Linux, Windows) and Metal (macOS).
It is the graphics half of the native port; the CPU half is the
[recompiler](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md).

This page is the renderer's status and plan. The GPU interface it is built on is specified in
[Render adapter](RENDER_ADAPTER.md); what a frame costs is in [Performance](PERFORMANCE.md).

## Principles

1. **Multiplatform.** Nothing works on Metal only. Planning a frame, tracking resources and
   converting shaders happen in the shared core; a backend only speaks its API.
2. **Always playable.** Native Metal is the default on Apple Silicon; the MoltenVK renderer stays
   selectable (`BB_METAL_FRAME=0`) until N5 is accepted.
3. **Measured.** Each step is compared against the path it replaces in alternating runs: FPS,
   frame-time p90, CPU time, the GPU thread's time per draw, and GPU time from Metal System
   Traces.
4. **Ahead of time where possible.** Shaders and texture layouts converted once, on the player's
   machine, from their own copy of the game. No game code, shaders or assets in any repository.

## Progress

Each bar has 20 blocks, one per 5% of the step's accepted scope. R5 is the mean of its seven
steps.

| Step | Progress | State, and the next action |
|---|---|---|
| **R5 · native renderer** | 64% `████████████░░░░░░░░` | N0, N1 and N4 done; N5 the default, waiting for acceptance; N6 under way |
| N0 · scope | 100% `████████████████████` | Done: where the commands come from and where a frame's time goes |
| N1 · frame planning | 100% `████████████████████` | Done: clears deferred into the next pass's load action |
| N2 · shaders ahead of time | 0% `░░░░░░░░░░░░░░░░░░░░` | After N6. First action: an inventory of the shader variants the game uses |
| N3 · textures as objects | 0% `░░░░░░░░░░░░░░░░░░░░` | After N6. First action: trace where the engine creates and releases textures |
| N4 · render adapter | 100% `████████████████████` | Done: A0–A6; the renderer records through the [adapter](RENDER_ADAPTER.md) |
| N5 · Metal backend | 95% `███████████████████░` | M1–M4 implemented; native Metal the default since 0.9.0 RC 1. Next: acceptance (below) |
| N6 · engine render boundary | 57% `███████████░░░░░░░░░` | 4 of 7 steps: the plan; the draws, their output state and their resources observed ([below](#n6-the-engines-render-boundary)). Next: N6.5, mesh draws recorded from the draw state |

## Now and next

In this order (the app's roadmap):

1. **N5 acceptance (now).** The maintainer's play sessions passed. Acceptance waits for:
   - players' reports on the 0.9.0 release candidates (RC 1: native Metal by default; RC 2:
     online and the save editor);
   - the start-up capability comparison on M1 and M2 Macs (`BB_METAL_CAPS=compare` logs every
     difference between the Metal backend's answers and MoltenVK's; `BB_METAL_CAPS=moltenvk`
     uses MoltenVK's). On the M3 Pro it finds 0 differences.

   Until then the app bundles MoltenVK and the Vulkan loader for the fallback only. After
   acceptance: decide whether the fallback leaves the macOS app.
2. **N6, the engine's render boundary (under way).** The native renderer takes the engine's
   draws at CoreGraphics2's draw methods, with their draw state, instead of decoding the PS4
   command buffers the game writes. The plan, its steps and what the observer found:
   [below](#n6-the-engines-render-boundary).
3. **The recompiler's CPU work** (bloodborne-recomp).
4. **N2 and N3.** Shaders are still recompiled from GCN and converted to MSL while the game runs
   (the Metal backend caches built pipelines between sessions); textures are still inferred from
   GPU memory by shadPS4's texture cache.

## What was measured

- **The command stream (N0).** Bloodborne links libGnm statically: the game writes the PS4's
  command packets (PM4) itself, through 99 known libGnm methods, and the packets record those
  calls without loss. Until N6, the packet reader is the front end.
- **A frame on MoltenVK (N0;** Hunter's Dream, about 48 FPS): 14.9 ms of GPU
  work and 5.9 ms of GPU idle (28% of the frame) in about 420 Metal encoders. Every clear,
  tessellated draw (MoltenVK runs the vertex and hull stages as a compute pass) and copy between
  draws ended a render pass. CPU: the game's code about 3.8 cores, the renderer about 1.2,
  MoltenVK and Metal about 0.6.
- **Frame planning (N1).** Eligible clears become the next pass's load action (on by default;
  `BB_DEFER_CLEARS=0` turns it off): +1–2 FPS. Drawing fixed-factor tessellation as triangles
  (`BB_FIXED_QUAD_TRIANGLES=1`) cost more CPU and p90 than it saved and stays opt-in; Metal's own
  tessellator does that work in the Metal backend. Grouping copies gained nothing.
- **The Metal backend (N5)**, M3 Pro, 1080p output:

  | Where, setting | Metal | MoltenVK |
  |---|---|---|
  | Cathedral Ward (the test save), native 1080p | 38.2–39.6 FPS | 30.6 FPS |
  | Hunter's Dream, FSR 3.1 Balanced (the Metal backend's first version, when it still offered FSR) | 65.5 and 61.7 FPS, p90 16.3 and 17.6 ms | 58.0 and 58.1 FPS, p90 20.4 and 18.9 ms |
  | A new game, 3 h 26 min of play by the maintainer, native 1080p | median 42.2 FPS, 37.2 or more 90% of the time, no faults | not measured |

  Every pipeline the game used in a long session builds in Metal (550: 510 graphics, 40 compute;
  0 failed), and no draw met a missing pipeline or an unsupported command. The GPU thread
  records a draw in about 4.0–4.4 µs on the Metal backend.
- **Upscalers on Metal.** At native 1080p no upscaler beats native: in Cathedral Ward native
  39.0 FPS, MetalFX Balanced 33.5, MetalFX Quality 31.7. At 1080p the frame is not bound by
  pixel work, so the upscaler is off by default.

## The engine's render boundary (for N6)

Dantelion2's CoreGraphics2 draws through seven methods of its graphics context (`draw`,
`drawInstanced`, `drawIndexed`, `drawIndexedInstanced`, `drawIndirect`, `drawIndexedIndirect`,
`dispatch`). Each applies the state blocks of the draw state it is given, binds the shader
resources and calls libGnm. Counted with `BB_DRAW_CENSUS=1` (M3 Pro, translator, the player's
save in a level, 33 FPS: about 62,000 libGnm draws and dispatches a second, about 1,880 a frame):

| Through | Share of libGnm's draws and dispatches |
|---|---:|
| The seven methods (`drawIndexed` 33,000 a second, `draw` 17,800, `drawIndexedInstanced` 1,800, `drawIndirect` 100) | 85% |
| CoreGraphics2's compute passes: buffer copies and fills, texture clears and copies, on the same context and resource binder | 7% |
| Render-target passes (fast-clear elimination, depth-stencil clears, depth decompression) | 2% |
| Scaleform's renderer (menus and HUD) and one small wrapper outside CoreGraphics2 | 6% |

The compute passes make about 4,400 dispatches a second, which is nearly every dispatch: the
only others are the few the render-target passes make. `dispatch`, the seventh method, did not
run in the count.

So N6 can take nearly all of the world's draws at the seven methods; the compute passes, the
render-target passes and Scaleform are three smaller boundaries of their own.

**Who calls the seven methods** (the 16 busiest call sites): 65% of the calls come from
CoreGraphics2's mesh draw (`CG::MeshRenderer::drawMesh`: one draw per face set of a mesh, with
the mesh's vertex buffers and input layout); then three functions with 18%, 7.5% and 5.9%. Every
mesh draw counted used one material class, which picks a shader program by technique and pass
type and binds its five stages' shaders, its constants (parameter objects writing its constant
buffer) and its textures (pixel stage, and domain stage for tessellation). The 18% and the 5.9%
are deferred lighting (batches of up to 12 lights drawn as full-screen quads limited by the
batch's depth bounds); the 7.5% draws geometry written into an upload ring.

**The draw state** is one object modelled on Direct3D 11: input assembly, 32 vertex buffers, the
rasterizer, blend and depth-stencil states, eight colour targets and a depth target, and five
graphics stages, each with 16 samplers, 128 textures and 14 constant buffers. Its known layout is
in bloodborne-recomp's `annotations/structs.h`.

## N6: the engine's render boundary

R4 (bloodborne-recomp) named the boundary and described its objects:
[the annotations](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md#annotations),
with the draw state's layout in `annotations/structs.h`.

### Steps

| Step | Progress | State |
|---|---|---|
| N6.1 · the plan | 100% `████████████████████` | This section |
| N6.2 · draws observed | 100% `████████████████████` | Every draw through the methods matched to its packets; topology, counts, index buffer and shaders agree |
| N6.3 · output state observed | 100% `████████████████████` | Render and depth targets, blend, depth-stencil, rasterizer, viewport, scissor, stencil reference, blend colour and depth bounds agree |
| N6.4 · resources observed | 100% `████████████████████` | The textures, samplers, constant buffers and vertex buffers the shaders read are the draw state's |
| N6.5 · mesh draws from the draw state | 0% `░░░░░░░░░░░░░░░░░░░░` | Next: `CG::MeshRenderer::drawMesh`'s draws (65% of the methods' calls) recorded from their records, with the buffer manager's addresses; measured against the packet path |
| N6.6 · the other draw methods | 0% `░░░░░░░░░░░░░░░░░░░░` | The deferred lighting's quads and the upload ring's geometry |
| N6.7 · the smaller boundaries | 0% `░░░░░░░░░░░░░░░░░░░░` | Compute passes, render-target passes (including the methods' own fast-clear resolves) and Scaleform: each its own boundary, or left to the packet path |

### How a draw reaches the renderer

- **Tags in the stream.** An entry hook on each draw method reads the draw state it is given into
  a record (`gpu/shim/bbport_render_boundary.cpp`). A hook on libGnm's draw functions files the
  record under a number and writes a three-dword PM4 NOP with that number just before the draw's
  packet, for the method's own draw only (the libGnm call returning into the method): a method
  that changes a render target first resolves the old one's fast clear with a draw of its own,
  which keeps the packet path. The GPU ignores the NOP; the engine copies its recorded commands into the buffers it
  submits, and the tag travels with them. The packet reader, meeting the tag and then the draw,
  knows which record the draw is.
- **One stream, one order.** A draw taken over is still recorded where its packet stands in the
  stream: the reader hands the record to the native renderer at the tag instead of decoding the
  registers. Waits, labels, flips, compute passes and every draw not taken over keep the packet
  path in the same stream, so nothing needs reordering or extra synchronization.
- **Translator and recompiled library.** The hooks rewrite the methods' first instructions, so
  those 11 functions fail the recompiled library's byte check and run translated; everything they
  call stays recompiled. The cost is measured with each step.

### How the draw state maps onto the adapter

- **Pipeline.** The five stages' shaders (each `CgShader`'s code address) are still GCN code,
  recompiled and converted as today (N2 changes that); the input layout and the vertex strides
  give the vertex input; topology, the rasterizer, blend and depth-stencil blocks and the render
  and depth targets' formats give the rest of the pipeline description.
- **Draw state.** Viewports, scissors, the blend colour, depth bounds and the stencil reference
  map onto the adapter's `DrawState`.
- **Bindings.** Each stage's samplers (S#), textures (T#) and constant buffers (V#s made from
  address and size) are the resources the engine's binder copies into its descriptor tables.
  Which binding slot each lands in is given by the shader's resource table, as the packet path
  finds it today.
- **Vertex and index buffers.** 32 vertex buffers with strides and offsets; the index buffer with
  its DXGI format and byte offset. The shader reads one V# per vertex attribute: the buffer's
  address plus the attribute's offset in a vertex, with the buffer's stride.
- **Buffer addresses.** Constant buffers, and the vertex buffers marked `managed`, are placed by the
  context's buffer manager, which looks their GPU addresses up in a table of its own (bloodborne-
  recomp's `annotations/structs.h`, `CgConstantBuffer`). Recording a draw from the draw state
  needs those addresses: the hook will ask the engine's own lookup on the game's thread.

### What the observer found (N6.2 to N6.4)

`BB_RENDER_BOUNDARY=observe` (not with `BB_DRAW_CENSUS=1`) compares each tagged draw with the
registers the packet reader decoded, every 5 seconds in the log. In a two-minute run (M3 Pro,
the test save, translator, no faults):

- 3,006,657 draws went through the methods and were tagged; 3,004,963 (99.94%) reached the reader
  with their tag. The 311,385 draw packets without one are the 295,520 libGnm draws made outside
  the methods and the 15,906 fast-clear resolves the methods make themselves.
- Every field compared agrees in every tagged draw: topology, counts, index type and start,
  instances; the five stages' shaders; the colour and depth targets, the target mask, blend,
  depth-stencil and stencil operations, clip and primitive setup, viewport 0, the stencil
  reference, blend colour and depth bounds. The draw state's scissor applies only when its
  rasterizer block enables the viewport scissor (`scan_mode[1]`, 738,110 draws); then it agrees
  too. So for these draws the draw state alone says what the packets say.
- The resources (2,839,214 draws whose resources the draw-preparation workers had read in time,
  91%): every texture the shaders read is in the draw state's tables with the same T#
  (16,422,346); samplers 99.98% (9,392,182 of 9,394,165; the rest are most likely empty slots the
  binder fills with the engine's default sampler, not checked yet); every constant buffer matches
  one of the stage's by size (23,447,710); every vertex buffer by stride (12,134,315), and by
  address where the draw state places the buffers itself (11,721,677). The one buffer more per
  draw is shadPS4's null buffer, not the game's.
- Observing costs frame rate: 34.2–34.9 FPS with every comparison, 37.4–37.6 with the first
  ones, 36.2–37.0 without it that day. It is a diagnostic only.

## The Metal backend today (N5)

On Apple Silicon `run.sh` selects it (`BB_METAL_FRAME=1`, `BB_GFX_BACKEND=metal`) unless
`BB_METAL_FRAME=0` asks for MoltenVK; Rosetta and other Vulkan drivers (`BB_VK_DRIVER`) use
MoltenVK. Linux keeps the Vulkan backend.

- **Start-up without Vulkan.** The native path links and maps neither the Vulkan loader nor
  MoltenVK. The shared renderer still uses Vulkan's types and calls some Vulkan entry points
  (instance, device, queue, capabilities, object creation); the Metal backend answers them
  (`mtl_vk_layer`, decision 6 of the [adapter](RENDER_ADAPTER.md#decisions)). Capabilities come
  from a recorded profile plus the MTLDevice (`mtl_vk_caps`). A C entry-point bridge
  (`mtl_vk_c_bridge`) lets optional Vulkan upscaler libraries resolve without the loader.
- **Resources.** Buffers, images, views and samplers are Metal objects; the game's memory is used
  in place (`newBufferWithBytesNoCopy`); everything the GPU may touch is in a Metal residency set.
  No VMA allocator.
- **Commands.** The adapter's Metal recorder (`mtl_recorder`) encodes draws, dispatches, draw
  state, bindings, barriers, clears, blits and fills; what the shared code still records as
  Vulkan commands (the scheduler's passes, transfers) is encoded by the Metal frame
  (`mtl_frame`). Submissions are committed on the backend's own queue; timeline semaphores are
  Metal shared events, fences Metal completion.
- **Shaders and pipelines.** SPIR-V to MSL with SPIRV-Cross (direct buffer, texture and sampler
  slots); rect lists rewritten as triangles; tessellation as compute kernels plus Metal's
  tessellator; geometry shaders as kernels. Built pipeline states are kept in a Metal binary
  archive between sessions (`PipelineFactory`).
- **Presentation.** The frame is drawn letterboxed into the window's `CAMetalLayer` drawable,
  with the settings overlay and FPS counter drawn by Dear ImGui's Metal backend.
- **Upscaler.** MetalFX only, encoded on the frame's command buffer with the backend's own
  textures; a setting asking for FSR or DLSS runs MetalFX. FSR 3.1 and FSR 4.1.1 are offered only
  with the MoltenVK fallback. Off by default.
- **Diagnostics.** `BB_METAL_TRACE=1` (command counts), `BB_METAL_CAPS=compare|moltenvk`.

The shader compiler, shader rewrites, pipeline cache and format and state tables originated in
[Huax-1839/bloodborne-macos](https://github.com/Huax-1839/bloodborne-macos/commit/0f0cf54) and
reached this project through
[Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port)
(`0b50589`, `78b59f5`), which further developed the Metal command layer and frame encoder. The
first Metal frame intercepted the renderer's Vulkan calls; the adapter replaced that route.
Crashes and their fixes are in [Stability](STABILITY.md).
