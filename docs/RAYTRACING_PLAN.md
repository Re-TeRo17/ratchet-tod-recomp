# Ray tracing "remaster pack": feasibility study and plan

Status: study only. Nothing described here is implemented. No runtime code was
changed to produce this document; the measurements come from two short runs of
the existing diagnostics (`RSX_DRAW_STATS`, `RSX_PRESENT_LOG`, `RSX_GPU_TIME`,
`RSX_DRAW_TRACE_FRAME`, `RSX_CONST_LOG`) plus the HLSL dumps the renderer
already writes (`RSX_HLSL_DUMP_DIR`).

Conventions: **FACT** = observed in code or in a log of this study.
**HYPOTHESIS** = inferred, needs a confirming step (given in the text).
The scene used is the Metropolis level at the start of `-level metropolis`
(`PS3_NO_FMV=1`, default window, no MSAA). Other levels and in-game moments may
differ; gameplay with the hero and enemies on screen was not captured (see 2.6).

Target: RT is to be done **after** the 60 fps goal. The game is currently
CPU-bound (the draw walker), so anything added on the CPU per draw directly
competes with that goal. Section 6 is written around this constraint.

---------------------------------------------------------------------------

## 1. Hardware and API support

FACT
- Development GPU: NVIDIA GeForce RTX 3070, 8 GB (`nvidia-smi`: driver string
  617.14, 8192 MiB; the engine logs `adapter: NVIDIA GeForce RTX 3070 (8018 MB)`).
- Windows SDK 10.0.26100 is installed; its `d3d12.h` contains the DXR 1.1
  types (`D3D12_RAYTRACING_TIER_1_1`, `ID3D12GraphicsCommandList6`), and
  `dxcompiler.dll` ships in the SDK `bin\x64`.
- The D3D12 engine creates the device with `D3D12CreateDevice(adapter,
  D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device)` (`rsx_d3d12_engine.c`, in the
  adapter loop and in the fallback; also `rsx_d3d12_backend.c`). 11_0 is only
  the *minimum* level, so it does not prevent DXR: the same object can be
  queried for `ID3D12Device5`. The engine already queries `ID3D12Device1` and
  `ID3D12GraphicsCommandList1`, so the pattern exists.
- All shaders are compiled with FXC (`D3DCompile`, `vs_5_0`/`ps_5_0`), with
  results cached on disk (`RSX_PSO_CACHE`). DXR needs DXC (shader model 6.3+),
  which is a **new dependency** for the RT module only; the existing path stays
  on FXC.
- The code base is C using COM macros (`CALL(obj, Method, ...)`); DXR types are
  all available through the C interface of `d3d12.h`.

HYPOTHESIS (verify with the capability probe of phase 0)
- RTX 30-series with a current driver reports `RaytracingTier = 1.1`
  (inline ray queries, `DispatchRays`, `ExecuteIndirect`-style builds). The
  probe is `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5)`; it was not run
  here because it needs code in the process.

Consequences
- The RT pack must be **optional and gated**: a GPU without tier >= 1.0 gets no
  RT and the game runs exactly as today. Tier 1.1 (inline `RayQuery` from pixel
  or compute shaders) is the better target, because it avoids shader tables and
  can reuse the engine's root-signature style.
- 8 GB of VRAM is the budget to respect (the engine already holds per-surface
  MSAA twins, a depth snapshot pool and 16384-entry object tables).

---------------------------------------------------------------------------

## 2. Geometry: how draws reach the host

### 2.1 Path (FACT)

Guest RSX FIFO -> `rsx_dispatch.c` (register file) -> `rsx_draw_engine.c`
(decode a draw, fetch/convert vertices, build the pipeline) -> a backend
(`rsx_d3d12_engine.c`, interface `rsx_draw_backend` in `rsx_draw_engine.h`).
The backend does not touch D3D12 per call: `bind_*`/`draw` calls append
`EngRecord`s and `eng_encode_records()` (`rsx_d3d12_engine.c`) turns them into a
command list at submit time (`eng_submit_list`). That encode step is the natural
place to observe the whole frame.

- Vertex data: `dc_fetch()` reads the guest arrays (RSX location 0 = local
  memory/VRAM, 1 = main memory, given by `location`/`offset` in each attribute)
  and writes an **interleaved, float-expanded** buffer: stride = 16 bytes per
  *enabled* attribute (the legacy rule "16 x float4", reduced to the attributes
  the vertex program reads). Types are decoded to float: S1, F32, half (SF), UB,
  S32K (signed 16-bit as raw value), CMP (11:11:10 packed normals/tangents) and UB256.
- Index data: rebuilt on the CPU into **u32** indices per draw (unique-ified
  vertices, strips/fans/quads turned into triangle lists when needed, restart
  cuts handled). Observed draw `prim` values in one frame: 8096 x TRIANGLES (5),
  45 x TRIANGLE_STRIP (6), 310 x QUADS (8, particles/sprites). No other
  primitive appeared.
- Vertex cache (`RSX_VCACHE`, default on): `EngVCEntry` in `rsx_draw_engine.c`
  stores, per distinct draw source, `verts` (the float-expanded buffer), `indices`
  (u32), `n_verts`, `stride`, the guest spans it was built from (`span[]`) and
  a GPU buffer handle (`buf`, `ib_off`) once the entry is promoted
  (`RSX_VCACHE_GPU_MIN`). Validity is either a guest-memory write-watch stamp
  or a content hash. Measured: about 5400-6000 entries, 70-79 MB; per
  120 frames `hit ~930k` of which `~840k by the write-watch`, `miss ~75-120k`.
  So in this scene roughly 90% of draws re-use geometry that did not change.
  This is directly reusable as the **identity** for a BLAS (key = `EngVCEntry.key`
  plus the `content` hash) and as the **source vertices/indices** (CPU copy and,
  for promoted entries, a GPU buffer).
- Per draw the engine also has: render state (`rsx_be_render_state`: blend,
  alpha test, depth test/write, cull, colour mask), the target set (up to 4
  colour targets, surface table `g.surfaces`, depth slots `g.zdepths`), the
  textures/samplers bound, and the 512 x vec4 vertex-program constant block
  (`g.vp_cb`, uploaded to the backend by `bind_vs_constants` only when it changed).

### 2.2 Volume (FACT, one frame of Metropolis start)

- 8451 draws traced in one frame; 8419 of them into the main scene surface
  (s1, 960x704), 16 into 96x96 per-object shadow maps (s7), the remaining ~16
  into small post-process targets and the 1280x720 HUD/present surface (s6).
- Sum over the main pass: about 1.61 M converted vertices and about 1.54 M
  triangles of index references per frame (`RSX_DRAW_STATS`: ~190 M vertices
  and ~550 M index references per 120 frames).
- 190 distinct pipelines in that frame; the engine's own `[hitch]` line reports
  2.9k draws at 8.4 ms on the walker in a lighter moment, and 8.4k draws here.
  (The CPU cost per draw is what the 60 fps work is fighting.)
- Not reliable here: absolute GPU time. With the 160k-line draw trace enabled the
  frame stalls on stderr (`[hitch] 32 s`), and `RSX_GPU_TIME=2` then measures
  a perturbed frame (about 19 ms for the 960x704 pass). Take GPU numbers from a
  clean `RSX_GPU_TIME=2` run (the user's figure: 25-46% GPU use).

### 2.3 Attribute formats (FACT from the draw trace)

Typical static-level draw (7220 of 8451 draws have this attribute 0):
`a0 = type 5 (S32K, signed 16-bit raw) x4, stride 20`, `a1 = half x2` (UV),
`a2/a3 = CMP` (11:11:10, normal/tangent), `a4 = UB256` or half (vertex
colour / second UV / lightmap UV). Other attribute-0 layouts: S32K x4 stride 24 or
28 (370, 204), half x4 stride 8 or 20 (285, 172), F32 x3 stride 12 (126, mostly
decorative/UI), half x3 (36). Positions are therefore **quantised in model
space**, and `a0.w` carries other bits in some programs (a dumped vertex
program computes `frac(abs(a0.w) * c3.zw)`: packed texture coordinates).
After `dc_fetch` everything is float.

### 2.4 Where the transform lives (FACT from three dumped vertex programs
`log\hlsl_cob\*_vs.hlsl` and from `RSX_CONST_LOG` of frame 1400)

The vertex programs are translated to HLSL (`rsx_vp_decompiler.c`) and run on
the GPU; constants are one `float4 vp_c[512]` cbuffer. Observed convention:
- `c[8..11]` = the **world-to-clip matrix**, one row per constant: `clip.x =
  dot4(r, c[8])`, `.y = c[9]`, `.z = c[10]`, `.w = c[11]` (in the scene pass
  c[11] is about the view depth, e.g. 72.8 next to an eye at z about 78).
- `c[7].xyz` = **eye position in world space** (-28.7, -1.0, 78.1 in the log;
  `c[7].w = 1`). The programs compute `c[7] - r` for the view vector.
- `c[18].xyz` = **per-object translation** (changes with every object: -29.97,
  -1.56, 74.58 for objects next to the eye; -7458, 20280, -30610 for the large
  background chunks), and in the skinned program `c[18].w = 6.1035e-05`
  (= 1/16384) is the position dequantisation scale.
- Static program shape (3 programs checked): `r = v0.xyz + c[18].xyz;
  clip = (r,1) * c[8..11]`. So for these, **world position = raw S32K position
  + c[18].xyz** with no per-object rotation. `c[16]`, `c[17]` and `c[19]` change
  per object too (about 1550, 1550 and 1380 loads in the first part of the
  logged frame); `c[19]` looks like fog/colour parameters.
- A second matrix pair `c[12..15]` was loaded with a projection-like content
  (|2.414| = 1/tan(22.5 deg)) in the first uploads of the frame. The same
  slots 8..11 carry a different matrix in the shadow/off-screen passes. So a
  pass must be identified (by target size, see 3.1) before slots 8..11 are
  taken as the camera.

HYPOTHESIS (to confirm before building on it): the layout above holds for
the majority of the level geometry; a fraction of programs (skinned meshes, sky,
decals, water, UI) use other conventions. Confirmation step: in the
translation stage, classify each distinct vertex program by pattern (static =
`v0 + c[18]` then `dot4` with `c[8..11]`; skinned = indirect constant
addressing) and print a histogram weighted by draw count and by triangle count.

### 2.5 Static vs skinned vs particles/UI

FACT
- **Skinned** meshes run GPU skinning in the vertex program: the HLSL has
  indirect constant addressing `vp_c[(32u + a0.z) & 511u]` etc. (per-vertex
  bone indices decoded from an attribute, a bone palette of 3 vec4 rows per
  bone starting near c[32], blended by 4 weights). 29 of 153 vertex programs in
  the `hlsl_cob` dump (Cobalia) use it. The matrices are filled by the game
  (the SPU job with 4x4 matrices noted earlier); to the host they are just
  constants uploaded before the draw.
- The existing counter `s_skin_draws` (attribute 7 of type UB256) showed `0/0`
  in the traced frame, and `RSX_TRACE_SKINNED_FROM` only looks at targets at
  least 1280 wide while the scene is 960 wide. Neither identifies skinned
  draws in this game; classification must use the vertex-program pattern above.
- **Particles/sprites**: QUADS (prim 8, 310 per frame), blended
  (497 draws with src/dst 0x302/0x303, 215 with 1/0, 67 with 0x302/1), usually
  depth write off (697 draws with depth write 0 in the frame).
- **UI/post**: few draws into s6 (1280x720) and small targets.
- **Static level**: depth test and write on, no blend: 7570 non-blended draws.

HYPOTHESIS: draws with blend off + depth write on + a "static" vertex program +
a vcache hit by the write-watch are the stable level geometry; an instance's
c[18] stays constant frame to frame for level chunks and moves for dynamic
props. The classification should be learnt per `(vcache key, c[18])` across
frames, not assumed.

### 2.6 Not captured
The traced frame has no skinned character visible (opening of the level), so
bone palette size per draw, triangle counts of Ratchet/enemies and the cost of
refitting are **unmeasured**. A gameplay capture is a prerequisite of phase 2.

### 2.7 A trap: the game culls on the CPU (FACT of the architecture, impact HYPOTHESIS)
Only what the game decided to draw reaches the engine (frustum/portal culling is
done by the guest). A TLAS built from "this frame's draws" would miss shadow
casters behind or beside the camera, and everything a reflection or GI ray
should hit off-screen. The RT module therefore needs a **persistent scene
cache** (instances kept alive for some seconds / distance after they were last
drawn), not a per-frame snapshot. This also determines VRAM use.

---------------------------------------------------------------------------

## 3. Rendering structure

### 3.1 Forward renderer, LDR scene target (FACT)
- One main pass: 8419 of the ~8450 draws go to a single 960x704 colour surface
  (format "8-bit RGBA" in the engine's table, `fmt3`) plus one depth surface; no
  MRT G-buffer appears (surfaces used per frame: s0 = presented 1280x720, s1 = scene,
  s6 = HUD, s7 = per-object shadow maps 96x96 and a 320x176 chain, s8..s11 = a
  bloom/blur chain 320x176 -> 160x96 -> 80x48). The draw trace prints only the
  first colour target of each draw, so a second simultaneous target (MRT) was not
  excluded directly; the evidence is that the surface list has no extra
  full-size colour surface (HYPOTHESIS: forward, single target; confirm by
  printing `n_targets` per draw).
- The scene is 960x704 in a 1280x720-class presentation; MSAA twins and
  "depth as colour" copies exist (RIPARTENZA: scene 960x704 MSAA 2x, soft
  particles read the depth through a 1920x704 view of the 1280x720 zeta).
  `RSX_SCALE` (0.5..8) and `RSX_AA` (fxaa, msaa2/4/8) change the host resolution.
- Consequently there is **no normal buffer and no albedo buffer**. Depth exists
  (the engine snapshots/resolves it: "depth resolve" passes, `eng_ms_depth_sync`).
  Normals would have to be reconstructed from depth or from the geometry.

### 3.2 Shadows of the game (FACT)
- The dynamic shadows are **per-object shadow maps**: 64x64, 96x96, 96x128 depth
  (16-bit) targets drawn at moving offsets (`0x0169xxxx`), 16 draws into a 96x96
  target in the traced frame, sampled with hardware compare: the log prints
  `shadow-compare units 0x20` (texture unit 5) and `unit 1/15 reads zeta ...`.
  History: "square shadows" and flicker were fixed in the engine (PIANO_SVILUPPO,
  RIPARTENZA 22/10).
- The static level has no cascaded sun shadow map in the traced frame; its
  lighting looks to be baked (vertex colours / lightmap-style attributes).
  **HYPOTHESIS**: a sun shadow from RT is therefore an *addition* for the level
  and a *replacement* only for characters/props.
- Sun direction: **not found**. It is not in `c[7..19]`. Candidates: vertex
  program constants used by lit programs, fragment program constants
  (`RSX_FPCONST_AT_FP=<hash>` prints them), or a Lua level script. Practical
  plan: ship a per-level `sun_dir` value in a sidecar file for the pack, and
  automate later by finding the unit vector that appears constant per frame.

### 3.3 Where the shadow map is sampled (HYPOTHESIS, to confirm)
The receiver fragment programs sample unit 5 with a compare sampler. Which
programs: list the `fp-hlsl` hashes of draws with `tex & 0x20` (`RSX_PIPE_LOG=1`
prints them; `RSX_SKIP_FP=<hash>` removes a program to see its effect).

---------------------------------------------------------------------------

## 4. Architecture options

### (a) Cheap hybrid first: screen-space effects

Inputs available: depth (resolved single-sample copy, the engine already has the
machinery), the scene colour (LDR 8-bit, so effects must be conservative), the
camera matrices from `c[8..11]` and eye `c[7]` of the main pass, and
reconstructed normals (from depth derivatives; per-vertex normals are not in a buffer).

Injection point: in `eng_encode_records()` at the first record that follows the
last **opaque** scene draw (the sequence is opaque draws first, then
blended/depth-write-off draws: 697 of 8451 have depth write off), still inside
the scene surface's pass sequence, before the bloom chain draws to s8..s11. At
that point the colour holds lit opaque geometry and depth is final for opaque
surfaces.

- **SSAO / GTAO**: feasible. Output multiplies the colour. Risk: it darkens baked
  light and emissive pixels (forward shading, no separate ambient term); mitigate
  with a strength cap and by excluding pixels whose shader marks them
  emissive (not identifiable generically).
- **Contact shadows** (screen-space ray march toward the sun): feasible; needs the sun
  direction (3.2). Good for characters on the ground.
- **SSR**: feasible but weak (LDR, single layer, no off-screen). Useful as the
  fallback for RT reflections.
- Cost: 0.3-1 ms at 1080p-4K-class on the 3070, GPU only, no extra CPU draws
  (one fullscreen dispatch at one record boundary).
- Effort 1-2 weeks, low risk; the dominant risk is subjective visual quality, not
  correctness. It also builds the injection point and the depth/camera plumbing the
  RT path needs.

### (b) DXR

Per frame (all GPU work after cheap CPU bookkeeping):
1. **Capture** during encode, one small record per scene draw: `(vcache key,
   GPU vertex/index buffer, n_verts, n_indices, stride, vertex-program class,
   the 4-8 transform constants, blend/alpha-test flags, texture ids)`. No vertex
   data is copied.
2. **BLAS**:
   - static rigid draws: build once per vcache entry, in **model space**, from
     the cached float vertices (position = `a0.xyz`, stride from the entry,
     u32 indices). The instance transform is a translation by `c[18].xyz` (HYPOTHESIS 2.4),
     so no vertex-shader evaluation is needed; compact after build;
   - skinned draws: a compute pass applies the bone palette (a port of the
     HLSL skinning to compute, or the translated vertex program wrapped in a
     compute kernel) into a world-space vertex buffer, then **refit** (update) the
     BLAS each frame;
   - anything else (unknown program): evaluate the translated vertex program
     (stream-out or compute wrapper), take clip-space output, undo the camera
     matrix `world = inverse(c[8..11]) * clip` (valid only when the program ends with
     that matrix, 2.4), and treat as dynamic/rebuild. Or skip it: not casting
     rays-visible is the safe default.
   - particles/blended/UI: not in the RT scene (they are not occluders).
   - alpha-tested geometry (foliage, fences): needs texture + UV in the any-hit
     path; first version treats it as opaque or excludes it.
3. **TLAS**: rebuild every frame from the persistent instance table (2.7),
   about 8-10k instances observed (a TLAS build of that size is cheap; the
   persistent scene may be several times larger).
4. **Effects as compute/inline RayQuery from the depth buffer** (primary visibility
   is rasterised, so no primary rays):
   - **Sun shadows**: reconstruct world position from depth + camera, trace one ray
     towards the sun per pixel into a visibility mask (R8), denoise, then apply it
     at the injection point of (a). Because lighting is baked, apply it as a
     multiplicative darkening limited to sun-facing pixels (needs normals: from the
     triangle ID if the primary draws also write one, or from depth).
     **Open design problem**: the game's own per-object shadow maps would
     double-darken; either keep them for characters and use RT for the level, or
     skip the game map receiver by binding a white map on unit 5 (a
     texture-binding substitution done in `sink_bind_textures`).
   - **AO**: short rays (0.5-2 m) in a cosine hemisphere, 1-2 per pixel.
   - **Reflections**: a ray from the surface along the reflected view vector
     (for water, glass, glossy floors only, selected by pixel-shader/material
     class). The hit has to be **shaded**: the game's lighting lives in
     translated fragment programs that cannot be run per ray. Practical fallback:
     use albedo from the hit's texture (needs bindless textures, SM6.6 descriptor
     heap indexing) times a cheap sun term, or reproject the previous frame's
     colour at the hit (screen-space) and fall back to a sky colour.
   - **GI**: hardest. Options: one-bounce diffuse from rays with reprojected
     previous-frame colour as radiance (screen-space radiance cache), or a
     probe grid (DDGI-style) fed by the same rays. Realistic only after
     everything above works.
5. **Denoise**: 1 ray/pixel needs a denoiser. Options (licences to check before
   shipping, the repo is public): NVIDIA NRD (MIT, SDK integration layer), AMD
   FidelityFX Denoiser (MIT), a bespoke bilateral + temporal filter (no
   dependency, enough for a binary shadow mask and AO). ReSTIR-style
   resampling (RTXDI: licence to verify) matters only for many-light direct
   lighting; the sun is a single light, so plain denoised 1 spp is enough. DLSS/FSR
   upscaling is orthogonal. All third-party items need a licence check and an
   entry in `THIRD_PARTY_NOTICES.md`; DXC (Microsoft) also needs one.

Budget on the 3070 (HYPOTHESIS, to measure): at the engine's internal
resolutions (960x704 up to `RSX_SCALE` 4 = 3840x2816) a sun-shadow dispatch costs
roughly 0.5-2 ms at 1 spp, AO a similar amount, per-frame TLAS under 1 ms.
Reflections and GI are the items that can exceed the 16.7 ms budget at 4K and
must be tied to `RSX_SCALE`/quality presets. VRAM: a compacted BLAS is on the
order of tens of MB for the 1.5 M-triangle frame; the persistent scene and
denoiser history are the larger parts.

### Recommendation
Do (a) first because it shares plumbing and gives visible results early; then
(b) in the order shadows -> AO -> reflections -> GI, each behind its own switch.

---------------------------------------------------------------------------

## 5. Plan, effort and risk

Effort in focused working days (agent-assisted); "risk" is the chance the item
slips or has to be redesigned.

| Phase | Content | Effort | Risk |
|---|---|---|---|
| 0 | Pre-setup (section 6): DXR probe, `tod.ini` switches, hook points, geometry capture file | 2-3 d | low |
| 1 | Offline tooling: reader/viewer for the capture, vertex-program classification histogram, verify 2.4 on all levels, find the sun | 3-5 d | low |
| 2a | Screen-space pack: depth resolve plumbing at the injection point, SSAO + contact shadows | 5-8 d | low-medium (visual quality, MSAA) |
| 2b | DXR core: device5/DXC, persistent scene cache, static BLAS from vcache + instance transforms, TLAS per frame | 8-12 d | medium-high (2.4 may not hold for all programs; culling) |
| 3 | Sun shadows via RayQuery + denoise + composition against the game's maps | 6-10 d | medium (double shadowing, baked light) |
| 4 | Skinned characters: compute skinning + refit; gameplay capture for cost | 6-10 d | high (SPU-fed palettes, per-game programs) |
| 5 | AO via RT | 3-5 d | low (reuses 2b/3) |
| 6 | Reflections (selected materials) | 10-15 d | high (hit shading, textures bindless) |
| 7 | GI | 15-25 d | very high (research) |

Order is flexible after phase 3; each effect ships behind its own switch and
defaults to off.

Key risks, in order: (1) hit shading for reflections and GI (no deferred
material data); (2) per-vertex-program variety (transform conventions); (3) the
CPU budget of per-draw bookkeeping while the project is CPU-bound; (4) baked
lighting means RT shadow/AO must be *added* in a way that does not double-darken;
(5) MSAA twins and `RSX_SCALE` multiply the cost of every full-screen RT pass;
(6) licences of third-party denoisers.

---------------------------------------------------------------------------

## 6. Pre-setup to do now (no change to performance when off)

Rule: with every `RT_*` key off, the only cost may be a few predictable
branches outside the per-draw hot path. Nothing here may add per-draw work to
the walker (the CPU bottleneck).

1. **Capability probe at device creation** (one-time, after `s_dev` is created in
   `rsx_d3d12_engine.c`, near the existing `CheckFeatureSupport` call): query
   `D3D12_FEATURE_D3D12_OPTIONS5`, log
   `[rt] DXR tier 1.1 supported` or `not supported`, and keep an `s_rt_supported`
   flag; `QueryInterface(ID3D12Device5)` and cache it. Also log shader model
   (`D3D12_FEATURE_SHADER_MODEL`) and whether `dxcompiler.dll` can be loaded.
2. **Switches** in `tod.ini` (the file just maps keys to environment variables, see
   `src/settings.cpp`), all default off:
   `RT_SHADOWS`, `RT_AO`, `RT_REFLECTIONS`, `RT_GI` (0/1), plus
   `RT_QUALITY` (1..3), `RT_SUN_DIR` (x,y,z override for levels without detection),
   and a debug `RT_CAPTURE_DIR`. Document them in `tod.ini` and `README.md`. The
   runtime reads them once at startup into a struct; `RT_*` is ignored (with one log
   line) if the probe failed.
3. **One new module** `libs/video/rsx_rt.{c,h}` (new files only) holding the state:
   `rsx_rt_init(device)`, `rsx_rt_frame_begin()`, `rsx_rt_note_draw(const
   EngRtDraw*)`, `rsx_rt_inject(point)`, `rsx_rt_frame_end()`. With RT off,
   `rsx_rt_note_draw` is a single inline `if (!g_rt_on) return;`.
4. **Hook points** (all already exist as natural seams):
   - device init: item 1;
   - per-draw metadata: in `rsx_draw_engine.c` right after the backend `draw` /
     `draw_buffer` call (about line 4294-4308 in the current file) the engine
     knows the vcache key, `vce->buf`, strides, render state and the VS constant
     generation; pass them to `rsx_rt_note_draw`. Alternative with zero walker
     cost: read them in `eng_encode_records()` (backend thread of the encode
     step), where the records already carry state;
   - injection: `eng_encode_records()` at the first record after the last opaque
     draw of the scene surface (3.1), and the later composite before the bloom
     chain;
   - frame end: `eng_submit()` / `eng_submit_list()`.
5. **Geometry capture tool** (the most valuable piece to build first). Two layers:
   - reuse the existing **`RSX_CAPTURE=<file.rsxcap>`** (`rsx_capture.c`): it
     records the register file, every method and every guest memory page read,
     and `rsx_replay` (CMake target `rsx_replay`, headless D3D12) re-runs
     those frames without the game. This is already the offline corpus: RT
     development can happen in the replay process, in seconds, on identical frames.
   - add **`RT_CAPTURE_DIR=<dir>`** (RT module, debug): at one chosen frame write
     `frame.rtcap` with, per scene draw: vcache key, prim, vertex-program hash,
     pass id (target size/format), the first 40 vertex constants
     (c[0..39]) and the bone palette range when the program uses indirect
     addressing, vertex and index buffers (deduplicated by vcache key, stored
     once), blend/depth state and texture ids; plus the camera
     (c[7..11] of the main pass), the depth surface as raw floats and the
     presented image. A small Python (or C++) viewer/validator checks that
     `inverse(c[8..11]) * clip` reproduces the depth, which tests hypothesis 2.4
     end to end without any renderer.
6. **Do-not-do list now**: no per-draw allocation, no DXC load unless an `RT_*`
   key is on, no change of device creation flags, no change of the FXC path, no
   edits of the existing pass sequence.

Acceptance for the pre-setup: with defaults the build is bit-identical in behaviour
and the benchmark (`RSX_GPU_TIME=2` and the 4K MSAA x8 worst case) is unchanged
within noise; with `RT_CAPTURE_DIR` set, one frame file is produced and the validator
passes on at least three levels.

---------------------------------------------------------------------------

## 7. Open questions (to close in phase 1)

1. What fraction of draws (by count and by triangles) use the static transform
   shape in 2.4, per level?
2. Sun direction and where it can be read.
3. Which fragment programs sample the game's shadow maps; can the receiver be
   neutralised per program?
4. Skinned draw cost in gameplay (bone count, triangles) and bone palette location
   and stability across frames.
5. DXR tier on the actual user GPU (probe), and the cost of one full-screen
   RayQuery dispatch at `RSX_SCALE` 1, 2 and 4.
6. Water, glass and mirrors: which vertex/fragment programs identify them (for
   reflections), and whether any level has real planar reflections already.

## 8. Method and reproducibility

Commands used (nothing built, no code changed, save data kept in `saves\fresh_rt`):
`PS3_ARGV=-no_legal;-no_frontend;-no_movies;-level;metropolis`, `PS3_NO_FMV=1`,
`PS3_SAVEDATA_ROOT=...\saves\fresh_rt`, with
`RSX_DRAW_STATS=1 RSX_PRESENT_LOG=1500 RSX_GPU_TIME=2 RSX_HITCH_LOG=30
RSX_OBJ_STATS=1 RSX_DRAW_TRACE_FRAME=1600` for the volume and format numbers and
`RSX_CONST_LOG=7,19 RSX_CONST_LOG_FRAME=1400 RSX_CONST_LOG_MAX=6000` for the
constants. Vertex program shape comes from `RSX_HLSL_DUMP_DIR` dumps made earlier
(Cobalia). Counts are from one frame and one level; treat them as order of
magnitude.

---------------------------------------------------------------------------

## Status (branch rt-ao, RTAO implemented; section written after the first working version)

FACT (measured, Metropolis, spot.pad replay, 1280x720 window, RTX 3070, RT_AO=1):
- DXR probe on the RTX 3070: raytracing tier 1.1, SM 6.5, ID3D12Device5 present. Logged at start (`[rt] DXR probe`).
- Switches (tod.ini / env): RT_AO (default 0), RT_AO_RADIUS (1.0), RT_AO_RAYS (3), RT_AO_HALFRES (1), RT_AO_STRENGTH (0.8),
  RT_AO_DEBUG (1 raw AO, 2 primary-ray geometry test). Diagnostics: RT_AO_TRACE=<n>, RT_AO_DUMP=<dir>, RT_AO_CVAR=1, RT_AO_AB=<n>.
- Shaders are compiled OFFLINE with the SDK dxc (rt/compile_rt_shaders.ps1 -> rsx_rt_ao_dxil.h): no dxcompiler.dll/dxil.dll is shipped.
  The vertex-program evaluation kernels are FXC cs_5_0 (D3DCompile, already a dependency). DXC licence: to be verified before
  anybody ships the DLLs; with offline bytecode nothing of DXC is redistributed.
- Geometry: a vertex program is "static" if it has no indirect constant addressing and no vertex textures. Its translated HLSL is
  run as a compute kernel over the cached vertices; world = inverse(c[8..11]) * clip. RT_AO_DEBUG=2 shows that the rebuilt
  world matches the depth buffer (floor, walls, trees, fences white). Most level draws use object->clip through c[32..35]
  (not c[18]): the first static-class rule (c[8..11] literal + c[18]) covered only ~340 of ~5500 opaque draws.
- Instance identity = vertex cache key+content + the constants the program reads in c[16..42] except c[32..35] (camera dependent).
  An instance is copied after being seen in 2 frames, built at <=48 per frame, kept 30 s unseen; scene reset after 3 frames with 0 known instances.
- Scene reached: ~16k instances, 1.44 M triangles, BLAS 124 MB (not compacted). AO GPU cost (960x704 scene, half-res traces, 3 rays):
  0.4-0.6 ms (`[rt-gpu]` lines). Not measured: 4K MSAA8, fps A/B with bench.ps1, Cobalia (see below).
- Injection: before the first draw into another target that samples the scene colour (bloom chain). MSAA path (twin RTV, depth sync) written, NOT run.

NOT DONE / UNVERIFIED (the user asked to stop running tod before these):
- fps/GPU numbers at 4K MSAA8 (>=3 runs each), Cobalia screenshots, MSAA x8 correctness, flicker over time (noise is static + spatial filter, no temporal accumulation).
- Dynamic objects (Ratchet, props, skinned/indirect programs) are not in the scene: they receive level AO but cast none.
  Foliage with alpha test is treated as opaque geometry.
- Known limits: AO also darkens particles drawn before the bloom chain; far geometry (>60 m) fades out; AO radius is world units.
- Walker cost with RT_AO=1: promoted vertex-cache entries keep their CPU copy (more RAM); one hash per static draw. With RT_AO=0: a branch on g_rsx_rt_on.

A/B images: log/bench_rt/ab_png/pm2_frame*_AB.png (RT_AO=0 left, 1 right, consecutive frames, RADIUS 2, STRENGTH 1, RAYS 4) and pm1_* (defaults).
