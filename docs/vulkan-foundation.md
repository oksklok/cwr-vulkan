# Vulkan backend development

Current status: Vulkan remains opt-in, with gameplay lighting/fog, projected
shadows and ordered screen batching. The latest measured Infantry map result is
6.55 -> 47.49 FPS with 91.25% fewer screen draws. The dated milestones below are
historical; their earlier unsupported-feature lists are superseded by later work.

Repository migration: `oksklok/cwr-vulkan` is an independent clone with its own
`.git` directory and official `BohemiaInteractive/CWR` upstream. Its main starts
at official 3.05 (`ffc6183`) and replays only the six Vulkan commits from the
historical branch through `f88f733`; no Chinese localization history is included.
The historical Git branches are preserved in a verified local bundle/ZIP. The
linked CWR-RR worktree has been removed through Git after moving its ignored
game/build evidence into this independent checkout. Private tools/dependencies
are under `build/legacy-vulkan/tools/`; preserved old build/client outputs under
`build/legacy-vulkan/` and `dist/legacy-vulkan/`; stock game data and the isolated
renderer profile under `game-local/`. Active builds use `build/vulkan-local/`
and `build/gl33-local/`. No old CWR/CWR-RR folder is required.
Relocation checks: both configurations rebuilt from relocated tools/dependencies;
83 driver-free guards and 44 focused cases / 223 assertions passed. The stock
GL33 initialization check exited 0. The real RTX 4060 Ti indexed-triangle run
presented 3,548 frames, exited 0, and reported zero validation errors/warnings
through teardown (implicit layers disabled only for that process). This is a
diagnostic smoke test, not evidence of Vulkan game-world rendering support.
Historical runtime results below retain their original context; migration checks
are reported separately, not substituted for those artifacts.

`PoseidonVK` is an opt-in SDL3 Vulkan backend registered as `vk`. GL33 remains
the default and reference game renderer. Clear/present, indexed triangle and
native static textured Shape drawing work, including stock P3D models and a
WRP-derived scenery portion. Normal `--render vk` startup now visibly reaches
the menu over the stock intro world. Rendering remains experimental and unlit;
this is not a claim of playable missions. Latest verification is recorded below.

## Build and selection

Configure the normal Windows Clang preset with `-DCWR_HAS_VULKAN=ON` and an
installed/system Vulkan SDK. CMake uses `find_package(Vulkan REQUIRED)` and
`Vulkan::Vulkan`; no Vulkan dependency framework is vendored. The option defaults
to OFF, so normal GL33 builds do not require or link Vulkan. The indexed triangle
also requires the SDK's `glslangValidator`: CMake compiles the two GLSL sources to
embedded SPIR-V 1.0 at build time (no runtime shader compiler). SDL3's `vulkan`
vcpkg feature must be enabled; its Windows default otherwise rejects Vulkan windows.

```powershell
cmake --preset win-x64-clang-rwdi -DCWR_HAS_VULKAN=ON
cmake --build build/win-x64-clang-rwdi --target PoseidonVK PoseidonGL33 PoseidonTests PoseidonVKPolicyTests PoseidonGame
ctest --test-dir build/win-x64-clang-rwdi -R '^PoseidonVKPolicyTests$' --output-on-failure
```

The game clients (Game, GameDemo and Tetris) explicitly register the backend
when enabled. The existing `--render` option accepts `vk`; its default remains
`gl33`. Registration does not initialize SDL or enumerate physical devices.
Factory availability means the backend was compiled/registered, not that a
compatible driver has been verified. Creation reports errors and returns null.
Vulkan has lower factory priority than Dummy, so automatic selection preserves
the existing GL33-to-Dummy fallback instead of selecting an unfinished renderer.

Use the explicit renderer-only smoke mode for live testing:

```powershell
PoseidonGame.exe -C <GOG-Remastered-3.05-directory> --render vk --vulkan-smoke triangle --vk-validation --width 800 --height 600 --timeout 30
```

`--vulkan-smoke clear` tests the original clear/present path; `--vulkan-smoke shape`
constructs a real engine Shape and uses the production geometry virtuals. All modes use the
existing backend factory and SDL window/event loop, stop before game-content and
audio initialization, never fall back to GL33, and exercise normal destruction
on timeout or window close. A disabled Vulkan build rejects the option. Normal
game startup with `--render vk` still encounters explicit unsupported calls.
Use the existing `POSEIDON_USER_DIR`, `POSEIDON_CACHE_DIR` and `POSEIDON_TEMP_DIR`
redirects to keep test writes outside the retail content directory.

## Ownership and frame lifecycle

- `EngineVK` inherits the existing `Engine` directly and implements its window,
  frame and capability seams. It owns an SDL Vulkan window and balances its
  video-subsystem reference. It uses the existing window placement resolver and
  the shared SDL input/focus/fullscreen event pump.
- `VulkanContext` is private to this backend. It owns the instance, SDL-created
  surface, selected device, queues, command pool and all swapchain/frame objects.
  It needs Vulkan headers and the loader, but no engine, SDL or asset types.
- `SwapchainPolicy.hpp` contains only driver-free surface/queue decisions.
- `EngineVK_Unsupported.cpp` centralizes explicit failures for unfinished
  resource and rendering entry points. It does not inherit Dummy's no-op draws.

Instance creation uses SDL's required surface extensions, optional debug-utils
object names and optional portability enumeration. `--vk-validation` requires the
Khronos validation layer; its absence fails initialization with an actionable
error, rather than allowing an unvalidated smoke-test success. Synchronization
validation is enabled when its layer extension is available. Warning/error callbacks
include their message IDs. Loader diagnostics are distinguished from validation
messages. A Vulkan 1.0 device is sufficient; portability subset is enabled when
advertised, with its properties2 instance dependency retained.

Device selection requires graphics and presentation queues, the swapchain
extension, usable surface formats/present modes, and color-attachment image
usage. Discrete GPUs are preferred over integrated GPUs. A combined graphics
and present family is preferred; distinct families use concurrent image sharing.

Swapchain creation chooses a supported format, clamped/fixed surface extent,
bounded image count and supported composite-alpha mode. FIFO is the default;
requested off/adaptive intervals use supported alternatives with FIFO fallback.
Each image has a view, clear-pass framebuffer and presentation semaphore.

Two frames in flight each have a command buffer, acquire semaphore and initially
signaled submit fence. `InitDraw` checks SDL's drawable size, skips minimized or
zero-sized windows, recreates when needed, waits for the frame fence and acquires
an image with a finite timeout. Out-of-date acquire defers recreation without
resetting the fence. Suboptimal acquire is consumed and presented before rebuild.

The render pass transitions an undefined image into color-attachment use and
finally into presentation layout. Every frame begins with opaque black;
`Clear` records a full color clear using the engine's packed RGB convention.
There is no retained framebuffer-content contract. Each swapchain image now has
a D32 (D16 fallback) depth attachment cleared to 1 every frame; `clearZ` also
supports an explicit depth clear.

`FinishDraw` closes and submits the command buffer, then presents. The submit
waits at color-attachment output. Fences reset only immediately before submit.
Present-wait semaphores are indexed by acquired image rather than frame index,
as described in the [Khronos semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
Out-of-date/suboptimal present requests recreation. Other Vulkan errors stop
the backend and release resources; failed recording/submission is not retried
with potentially stranded synchronization objects.

Resize and fullscreen/pixel-size events request deferred recreation. Recreation
waits for device idle, destroys the old image resources, and rebuilds from fresh
surface capabilities. Minimized windows retain old resources until drawable.
Shutdown waits idle, destroys the triangle pipeline before the render pass,
then framebuffers/depth attachments/views/semaphores and swapchain, immutable geometry buffers,
pipeline layout, frame synchronization, command pool, device, surface, instance,
SDL window and the backend's SDL video reference. Partial initialization cleanup
and repeated shutdown are supported. This uses the conventional core-Vulkan
idle-based swapchain teardown; presentation fences/maintenance extensions and
non-blocking swapchain retirement are outside this stage.

The shared `SDLEventWindow.hpp` moved from PoseidonGL33 to
`Poseidon/Graphics/Shared`, retaining its existing behavior and adding drawable
pixel-size events for DPI changes. The existing source-audit test follows the move.
Vulkan uses SDL's fullscreen path and does not initialize the GL ImGui renderer.

## Deliberately unsupported

Immutable opaque untextured Shape/TL submission and depth testing are implemented.
Dynamic/animated geometry, 2D/UI/font rendering, texture bank/loading/upload,
material binding, depth bias, gamma correction and shadow rendering remain unsupported.
Required draw/resource methods throw an actionable `std::logic_error`; neutral
gamma/bias requests are allowed, and unsupported capabilities report false.
Optional engine features otherwise retain the base interface's unsupported
defaults. Monitor enumeration and SDL display-mode requests are implemented;
monitor switching and readback are not.

No GL33 renderer replacement, assets, localization, modern shading, upscaling or
renderer redesign is part of this change.

## Indexed triangle and donor reuse

`EngineVK::DrawTestPattern("triangle")` is allowed only with
`--vulkan-smoke triangle`. Three immutable position/color vertices and uint16
indices **2,0,1** are uploaded into coherent host-visible buffers, bound to a
minimal graphics pipeline and drawn with `vkCmdDrawIndexed`. Dynamic viewport,
scissor and aspect compensation follow the swapchain extent. Both frame slots
share read-only geometry; resize rebuilds the pipeline against the new render
pass, retaining buffers/layout. Device-idle teardown releases all resources.

Selectively adapted from **Deus-Ex (koosoli)**, `koosoli/PoseidonVK` commit
`7523bd5d3afcc13f13e51050d7c717ca103a7c19`: the host-visible allocation/map/unmap
pattern in `BufferVK.hpp/.cpp` and `bootstrap_triangle.vert.glsl` /
`bootstrap_triangle.frag.glsl`. These remain GPL-3.0-or-later with the additional
Bohemia terms in `LICENSE`. Buffer ownership/bounds/failure cleanup were tightened;
the shader UBO/descriptors were removed in favor of 32-byte push constants. The
pipeline, smoke loop and integration adapt to our existing context, frame slots
and per-image present synchronization; donor texture/mesh/framework code was not
incorporated. Official 3.05 renderer seams and working lifecycle code are retained.

The backend-private `BufferVK` helper accepts buffer size/usage independently of
the triangle. It is the minimal preparation for `Engine::CreateVertexBuffer` and
`DrawSectionTL`: GL33's existing `Shape` path fan-triangulates polygons and tracks
per-section index ranges. The Shape milestones below implement those production
engine entry points using the same packing and polygon-fan conventions.

## Live verification (2026-10-09)

- GPU: NVIDIA GeForce RTX 4060 Ti, driver 617.14 (Windows 32.0.16.1714), Vulkan
  1.4.351; installed SDK/Khronos validation layer 1.4.350.0. API requests remain 1.0.
- Used `PoseidonGame` against the authorized local GOG Remastered 3.05 content
  directory with user/cache/temp redirected under ignored `build/vulkan-live`.
  The renderer-only diagnostic does not claim game-world Vulkan compatibility.
- First live clear attempt exposed SDL3's missing Vulkan build feature. After
  enabling it, clear/present and resizing worked. Normal close then exposed an
  inactive progress query unnecessarily calling the unsupported texture bank;
  a guarded short-circuit fixes this shared startup/close seam, with regression test.
- Clear retest: two resize recreations, 230 submitted frames, normal shutdown,
  zero validation errors/warnings. Triangle: visibly correct at 800x600,
  1104x801 and 704x501, also after minimize/restore; 11,038 submitted frames,
  normal close exit 0, zero core/synchronization validation errors or warnings
  including resource destruction. Pipeline recreation occurred for every new
  swapchain; geometry was uploaded once.
- One external loader error remains: a stale Epic Online Services overlay JSON
  path. Implicit overlays were disabled only for the test processes; their
  disabled-layer loader warnings are not validation warnings. No machine overlay
  installation or registry was changed.
- Vulkan-enabled and GL33-only Game/GameDemo/Tetris, GL33/Dummy and the Poseidon
  test target build successfully. Driver-free policy CTest passes all **83** checks (original
  42 plus buffer allocation/map/bind failure cleanup, bounds/lifetime and indexed
  layout/draw guards). Factory: 17 cases in each configuration (ON: 94 assertions,
  OFF: 91); window/close regression: 3 cases / 10 assertions in each. No driver is
  called by these tests.
- Final rebuilt clear/triangle timeout checks both exit 0 with zero validation
  errors/warnings, including teardown. Missing explicit `--render vk` and a
  Vulkan-disabled build both reject the diagnostic with exit 1. Default GL33
  `--check --nosound` exits 0 against the same content; the existing missing
  `biscamel\icamel2.paa` warning is not changed. Import inspection confirms no
  `vulkan-1.dll` dependency in the GL33-only executable.
- Game-local content, localization and resources retain identical before/after
  file-count/size/timestamp snapshots. No assets or distribution settings changed.
  Formatting and `git diff --check` pass; no dependency blocker remains.

Local evidence: `build/vulkan-live/clear/retest.{stdout,stderr}.log` and
`build/vulkan-live/triangle/{stdout,stderr}.log`, with `initial.png`, `resized.png`
and `restored.png`. Other GPUs/platforms, normal Vulkan gameplay and non-blocking
swapchain retirement are not validated by this milestone.

Follow-up error-path fix: mapped pointers become owned only after `vkMapMemory`
succeeds. Terminal backend shutdown returns the final validation count, so the
smoke verdict includes destruction-time errors. Driver-free regressions cover a
failed map with non-null output and an injected teardown-only validation error;
both build configurations and their focused tests pass. The GPU results above
precede this follow-up; no new live run was performed for these error-path fixes.

Explicit-validation follow-up: a missing requested Khronos layer fails before
instance creation. Driver-free tests cover the rejection/error message, enabling
an available requested layer, and unchanged layer-independent operation without
the flag. CLI help now describes the strict requirement; no new live run was performed.

Muted-logging follow-up: smoke success also requires the backend's stored failure
flag to be clear, sampled after `StopAll`/terminal shutdown and before destruction.
Two driver-free verdict tests use real `critical`/`off` logging filters and retain
normal close/timeout, frame, validation and logged-error guards (9 assertions).
No logging subsystem, rendering path or normal window-close behavior was changed;
no new live run was performed.

Shape milestone 1 (2026-10-10): `CreateVertexBuffer` accepts immutable `VBStatic`
and `VBBigDiscardable` Shapes. Position/negated-normal/UV packing, polygon fans,
`VertexIndex` indices and section ranges follow GL33. Dynamic/dirty updates reject
explicitly. GPU allocations are shared with referencing frame slots and released
before device destruction even if their Shape survives shutdown. Vulkan and
GL33-only builds pass; extraction/range rejection tests pass (2 cases, 13
assertions), as do the existing 83 driver-free guards. Shape drawing is still
unsupported at this intermediate milestone; GPU upload is built but not yet
live-exercised.

Shape milestone 2 (2026-10-10): `PrepareMeshTL` reads the scene camera and
model-to-world transform using GL33's camera-relative convention; `BeginMeshTL`,
`DrawSectionTL`, and `EndMeshTL` now record actual indexed mesh draws. The opaque
untextured shader uses the engine projection (0..1 depth, Vulkan Y inversion)
and white or `IsColored` scene constant color. Each swapchain image has a depth
attachment; resize recreates depth/framebuffers/pipelines while retaining mesh
buffers. Textures/materials, animated meshes, blending and unsupported render
flags still reject explicitly. Focused extraction/transform tests pass (3 cases,
19 assertions); 85 driver-free guards pass. The existing live triangle path also
presents correctly with the new render pass (380 frames, zero validation errors
or warnings through shutdown). Live Shape integration follows in milestone 3;
this intermediate commit does not yet claim a visible Shape.

## Engine Shape live integration (2026-10-10)

`--render vk --vulkan-smoke shape --vk-validation` now renders a real `Shape`
through `ConvertToVBuffer` / `CreateVertexBuffer`, `PrepareMeshTL`, `BeginMeshTL`,
`DrawSectionTL` and `EndMeshTL`. The fixture has 8 vertices, 6 quad sections and
36 triangulated indices; two instances use separate rotating model matrices and
a translated/oriented engine camera. A combined section range and nonzero index
offsets are exercised. The farther instance is submitted after the nearer one,
so overlapping visibility depends on depth testing. Color is an opaque scene
constant; normals/UVs retain GL33 packing but are not shaded or sampled. This is
an engine mesh integration check, not normal gameplay or a hardcoded GPU triangle.

Real RTX 4060 Ti verification with Khronos core/synchronization validation:

- Captured and inspected distinct rotating poses at 800x600, 1104x801, 704x501,
  and after minimize/restore. Geometry, section colors, perspective and depth
  occlusion were visible. Resize rebuilt depth/framebuffers/pipelines correctly.
- Released the Shape's vertex-buffer owner while a frame was still recording,
  then re-uploaded next frame. In-flight references kept allocations alive;
  output and validation remained clean.
- The final 22-second timeout run submitted 3,394 / presented 3,391 frames,
  exited 0, and reported zero validation errors/warnings through destruction.
  The final normal window-close run submitted 1,216 / presented 1,213 frames,
  exited 0 and also reported zero validation errors/warnings. Counts differ by
  three during resize/restore; no backend failure was reported. External stale Epic overlay loader diagnostics
  remain; implicit layers were disabled only for the test processes.
- Vulkan and GL33-only builds pass. Focused Shape/factory/smoke/window tests:
  15 cases in each configuration (ON: 93 assertions; OFF: 90). The new Shape
  cases contribute 4 cases / 28 assertions. Vulkan's registered CTest passes
  1/1, with 85 driver-free guards. Root CTest enumeration is blocked by an
  ungenerated Studio test-list file in this targeted build; broad suites were
  not rerun.
- GL33 remains the default. Its initialization check exits 0; default-renderer
  startup initializes the world and normal closure exits 0. The live menu-window
  captures were black, including with the preserved pre-task GL33 executable,
  so visible GL33 menu/gameplay is not claimed by this check. Its renderer code
  was unchanged. Stock game data/resources metadata snapshots were preserved.

Evidence (ignored): `build/shape-live/lifetime-timeout/{result.json,stdout.log,
stderr.log,initial.png,rotated.png,wide.png,small.png,restored.png}`;
normal-close evidence under `build/shape-live/lifetime-close/`; GL33 initialization,
current launch and pre-task comparison under `build/shape-live/gl33-*`.

Remaining limits: immutable static buffers only; opaque full-viewport untextured
draws with default depth state and optional `IsColored`/`DisableSun` flags;
no textures/materials, lights, blending, animation, shadows or UI. Unsupported
flags/lighting and dirty/dynamic updates fail explicitly. `Shape::Draw` still
reaches unsupported per-section material/texture preparation. The smallest next
step is the neutral untextured section-preparation path needed for `Shape::Draw`
itself to consume these buffers; textured game geometry follows separately.

## Historical driver-free verification (initial foundation)

`PoseidonVKPolicyTests` exercises queue selection, extent/image-count bounds,
format/present/composite-alpha fallback, recreation classification and empty
context/invalid-initialization guards. Its 37 checks make no Vulkan API call,
create no instance or SDL window, and enumerate no physical devices. It links
the loader only to compile/link the real lifecycle implementation.

The existing Catch2 graphics/factory tests also test actual backend registration,
priority, idempotence, enum/code lookup and unavailable-backend handling. Those
tests use sentinel creation callbacks and never construct a Vulkan engine.

Verification on Windows used the installed Vulkan SDK 1.4.350.0, MSVC 14.44,
and worktree-local LLVM 21.1.8, CMake 4.4.4, Ninja 1.13.2 and vcpkg. The latter
tools and the unchanged manifest dependencies were installed under ignored
`build/tools` after dependency installation was authorized; nothing is vendored
into the backend. There is no remaining dependency blocker for this build.

- With Vulkan ON: PoseidonVK, PoseidonGL33 (and Dummy in Poseidon), all three
  client executables and all seven unit-test executables built and linked.
- With Vulkan OFF: GL33/Dummy, all three clients and all seven unit-test
  executables built and linked. Import inspection confirms that the GL33-only
  game executable does not import `vulkan-1.dll`; the Vulkan-enabled one does.
- Driver-free policy CTest: 1/1 passed, all 37 checks. The context and tests also
  compiled with MSVC `/W4 /WX`, and for x86 as objects only.
- Graphics/factory/input selection: 348 test cases passed with Vulkan ON and
  OFF. Hidden fixture-generation tests are excluded from this selection. The
  registration test uses real Dummy/Vulkan descriptors, but models GL33 with a
  sentinel because this harness stubs its registration. It also checks that the
  CLI default is still `gl33` and the existing Auto enum value is unchanged.
- Broader suites (excluding external game-data tags): Core 812/812 and
  Foundation 475/475 passed in both configurations. Poseidon 2450/2451 passed
  in both; the unchanged `ModArchive::Unpack rejects a valid archive wrapping a
  corrupt PBO` test throws `No mapping for the Unicode character exists in the
  target multi-byte code page.` No unrelated archive/encoding fix is included.
  Foundation's intentional crash-handler child test emits a diagnostic dump;
  the parent suite passes.
- Vulkan-enabled application suites: Server 7/7, Evaluator 95/95, Tetris 9/9,
  headless software-rendered Studio 50/50 passed.
- New C++ files pass clang-format checks; `git diff --check` passes.

At the initial foundation stage, legacy tests using absolute `/tmp` paths ran through a temporary drive alias
rooted in this worktree's ignored build directory; user/cache/temp directories
were also redirected there. No game executable was launched and no Vulkan
instance, physical device or live clear/present frame was tested. Before/after
file-count/size/timestamp snapshots of `game-local/Remastered`, localization and
resources were identical. Physical driver behavior was unverified at that stage;
the live results above supersede that limitation. These broader historical suites
were not all rerun for the indexed-triangle change.

## Native Shape sections (2026-10-10)

`--render vk --vulkan-smoke shape --vk-validation` now invokes ordinary
`Shape::Draw`, including `ShapeSection::PrepareTL`, neutral mip preparation and
basic unlit diffuse material colors. Textured requests remain explicit errors.
Individual mesh destruction no longer waits for device idle: referencing frames
retain buffers until their fences signal; terminal device teardown still drains.
RTX 4060 Ti live captures (`build/shape-live/native-close`) visibly confirmed
rotation, section colors and depth overlap across resize/minimize/restore.
Normal window close exited 0, 1134 submissions, zero validation errors/warnings
through teardown. Focused tests: 12 cases/83 assertions; policy guards: 85.

## Stock texture uploads (2026-10-10)

`--vulkan-smoke texture` mounts stock `dta/data.pbo` with the existing bank API,
loads `data\domek1_front_okna.pac` and `data\domek1_side.pac` through
`QIFStreamB` and `DecodePAABuffer`, and samples cached RGBA images on native
Shapes. No asset extraction or alternate decoder. Uploads use a staging buffer,
one-time command and fence, then shader-read layout; images/descriptors are held
by referencing frames. Only the original top mip is supported for now.
Two visibly distinct textures, UV orientation and section switching were inspected
in `build/shape-live/texture-close`, including resize/minimize/restore. Exit 0,
1081 submissions, zero validation errors/warnings through shutdown. Decoder and
Shape tests: 26 cases/4219 assertions; policy guards: 85. Alpha textures currently
fail explicitly; stock `domek2_side.paa` decoded successfully as cutout but is not
yet a supported draw. This is the next section-pipeline milestone.

## Textured sections (2026-10-10)

The texture fixture now draws stock PAC opaque and PAA cutout sections through
`Shape::Draw`. RGBA alpha is classified by the engine decoder; cutout discards
below 0.5 while retaining depth writes. A source-alpha blend variant disables
depth writes, with ordering left to the existing scene/section-class passes.
Clamp U/V, repeat and point/linear section samplers are supported. Other effects
and depth overrides remain errors. No lighting or texture-streaming framework.
`build/shape-live/cutout-close`: two textured native Shapes visually inspected
with depth overlap, correct UV orientation and window holes; resize/restore and
normal close passed, 1170 submissions, zero validation errors/warnings. Focused
decoder/Shape tests: 26 cases/4221 assertions; policy: 85 guards. Blend rendering
has not yet been visually verified against a stock translucent asset.

## Real stock P3D models (2026-10-10)

`--render vk --vulkan-smoke models --vk-validation` reads normal game configuration,
mounts stock Data/Data3D PBOs, uses `ModelCache` ODOL loading and `ShapeAdapter`,
and draws `data3d\dum_mesto.p3d` (561 display vertices, 9 sections) and
`data3d\jeep.p3d` (3438 vertices, 50 sections) through native `Shape::Draw` with
the existing `Object` material provider. No renderer-specific model loader.
Diffuse/emissive material modulation is unlit; stock specular fields do not add
a specular lobe. Opaque/cutout sections precede back-to-front blend objects using
the engine section-class filter. Crew/proxy geometry, animation and lighting
are not demonstrated. Arrows move the camera, PageUp/Down elevate, A/D turn.

RTX captures in `build/shape-live/models-close` and `models-scan-close` were
inspected: recognizable building and Jeep, original UVs/section textures,
glass/cutout visibility, depth, resize and restore. Camera movement was visibly
verified using bounded scan-code input (virtual-key-only injection did not
reach SDL). Normal close exited 0: 1440 submissions, zero validation errors or
warnings. Timed 22-second run exited 0: 3321 submissions, zero validation issues
through teardown. Focused model/adapter/Shape tests: 28 cases/126 assertions.
These are real models in an integration fixture, not a loaded game world.

## Stock-world scenery portion (2026-10-10)

`--vulkan-smoke world` reads stock `worlds\eden.wrp` with the same `WrpReader`
used by Landscape's OPRW loader. Four nearby static buildings are selected from
its 56,740 source objects, retaining original WRP matrices and source IDs
16952, 16967, 17061 and 17059. Models: `dum_mesto`, `dumruina`, `dum_rasovna`.
No manually arranged replacements, renderer-specific asset loader, terrain,
mission simulation or gameplay. This is a scenery-only real-content integration
route, not success of the normal Landscape draw flow.
RTX screenshots in `build/shape-live/eden-first` visibly show the stock building
cluster with original textures/placement and depth. Resize/restore/normal close
passed; exit 0, 1144 submissions, zero validation errors/warnings through shutdown.

Normal `--render vk --vk-validation` was attempted without smoke. The first
blocker was configured gamma (now implemented in the fragment shader); the next
is `EngineVK::Draw2D`, called by `ProgressSystem::Draw` during world initialization.
The run did not reach a menu/world. The next practical step is minimal textured
2D primitives for that existing progress/menu flow, not a new UI framework.

## Normal menu and intro world (2026-10-10)

The earlier startup blockers above are superseded. `Draw2D`, pixel/absolute
polygons and GL33-style textured 2D lines now share the existing Vulkan upload,
descriptor, frame-fence and shader path. Reciprocal-W, vertex ARGB, UVs, clipping,
alpha blending and depth are retained. Existing FreeType atlas creation/update
uses RGBA textures; the engine's GL-only atlas validity cast was corrected.
No font system was replaced. Texture interpolation uses decoded engine pixels,
keeps one cached result per source pair, and retires changed images at frame
fences. Missing stock references return null with a warning, matching GL33.
Eight sampling states are shared device-wide: stock preloading exposed and fixed
the original per-image sampler allocation-limit violation.

Software `FaceArray` submission now consumes the engine's already transformed,
lit and clipped TL vertices. Dynamic vertex-buffer requests decline hardware
storage and use this existing engine route; immutable GPU Shapes remain native.
Unused clipped vertices are not converted. Sky and animated texture frames use
the original scene/texture animation systems. Terrain detail uses GL33's 32x UV
and alpha modulation; stock water's specular texture uses its decoded bump sample
and sun direction. Depth bias reuses `ZBiasMath`/projection conversion. Native
materials are intentionally unlit diffuse/emissive, not full light-list rendering.
Vulkan explicitly advertises no projected-shadow support; Scene does not request
that optional pass. Unsupported shadow commands still fail, and GL33's capability
defaults to true. Other unsupported effects remain errors, not successful no-ops.

Normal startup without **any** smoke flag was captured and inspected on the RTX
4060 Ti in `build/shape-live/normal-restore`: recognizable stock intro vehicles,
terrain, original logo, menu labels and UI lines. Wide/small resize and
minimize/restore passed after fixing no-acquired-frame mesh handling. Normal
window close exited 0, 84 submissions/82 presentations, zero Khronos core or
synchronization validation errors/warnings through shutdown. Focused decoder,
Shape/screen, model/adapter and WRP tests: 55 passed, one archived external-data
case skipped, 4355 assertions passed; driver-free policy/lifetime guards: 85.

Limitations: original top mip only, no streaming/eviction, native lighting/fog
approximation, no projected/shadow-map effects, decals or 3D line/point effects.
Normal menu/intro rendering is demonstrated; see the stock mission check below.

## Final integration checks (2026-10-10)

Normal `--render vk --vk-validation --test-mission <stock Missions/01TakeTheCar.ABEL>`
uses the existing isolated mission-test staging and normal simulation/Scene path,
without any Vulkan smoke flag. RTX captures in `build/shape-live/stock-mission-input`
were inspected: textured terrain, town buildings, cutout trees, sky, first-person
M16 and HUD. Bounded Right/Up input visibly turned/moved the player. Wide/small
resize, minimize/restore and normal close passed: 98 submissions/97 presentations,
exit 0, zero Khronos core/synchronization validation errors or warnings. This is
real mission rendering and basic movement, not full campaign/combat qualification.
Radio text contains replacement glyphs; its encoding/font path remains unresolved.

Normal startup's previously ignored `--timeout` now closes the ordinary main loop
cleanly (default zero remains unlimited). A 22-second menu/intro run in
`build/shape-live/normal-timed-fixed` exited 0 after resize/restore, 194/193 frames,
zero validation issues through shutdown. Final triangle, textured native Shape
and real-model smoke regressions also passed visually, including lifecycle checks:
1189, 1183 and 1427 submissions respectively, zero validation issues. The loader
still reports a stale installed EOS overlay manifest; disabled implicit overlay
messages are separate from Khronos validation and no overlay registry was changed.

Vulkan-enabled and GL33-only builds passed. Final focused asset/geometry/factory
tests: 63 passed, one archived external-data WRP case skipped, 4410 assertions;
font/shutdown tests: 18 cases/327 assertions; policy guards: 85. GL33-only factory
and Shape tests: 13 cases/94 assertions. Default GL33 checks passed in both builds;
the GL33-only normal menu/intro was visibly inspected and closed cleanly in
`build/shape-live/final-gl33-menu`. Stock game/resources metadata hashes remain
unchanged; all profiles, staged missions and screenshots are under ignored build
directories. Next useful rendering improvement: original mip-chain upload/sampling
to reduce visible distant-texture aliasing, then broader mission/effect coverage.

## Gameplay improvement: screen states (2026-10-10)

Screen draws now use four independent depth/blend pipeline keys, removing the
no-depth opaque/blend collision regardless of creation order. Six focused Shape
cases/46 assertions passed. RTX menu and Take the Car captures under
`build/gameplay-live/state-*` were inspected for translucent UI/HUD, cutout
foliage and software geometry; resize/restore and close passed, 81/71 submissions,
zero Khronos core/synchronization validation issues through shutdown.

Original stored PAA/PAC mip chains are now exposed by `DecodePAAMipChainBuffer`,
using the same palette/PacLevelMem traversal and RGBA conversion as the unchanged
top-level API. Sequential and OFFS-indexed sources retain valid shorter chains.
Synthetic size/order/alpha/short-chain tests and actual bank-backed `domek1_front_okna`,
`domek2_side`, `detail_dx`, Abel `rwn` and `s3` checks passed: 24 cases/4318 assertions.
Top-only Vulkan texture regression was visually checked with resize/restore/close,
zero validation issues. GPU mip upload follows separately.

Vulkan now uploads every decoded stored level in one staging allocation/copy
submission, transitions the complete chain, and exposes all levels in its image
view. Shared samplers use trilinear filtering, or nearest texel/mip for explicit
point sampling; clamp/repeat remain unchanged. Image-view bounds handle shorter
chains. Dynamic font/UI and interpolated textures retain their single-level path;
alpha classification still uses the original top level, avoiding reclassification
of cutout assets from lower-mip edge alpha. Texture metadata/CPU pixels expose
the original levels too. No generated mips, streaming or cache redesign.
RTX `mips-menu`, `mips-mission`, `mips-models` captures were inspected: distant
terrain speckle is clearly reduced against `state-menu`, original building/road
textures and cutout foliage remain visible, HUD/UI still blend. Movement,
resize/minimize/restore and close passed with zero validation issues through
shutdown (menu 83 and mission 95 submissions). Focused decoder/stock/Shape tests:
30 cases/4364 assertions; policy CTest passes. Both build configurations passed.

Transient profiling (`CWR_VK_PROFILE=1`) measures wall frame intervals/p95, CPU
recording/upload/retirement and fence/acquire/present waits separately. With
Khronos validation enabled, matched intro intervals at about 353 submissions/frame
improved from 117.3 ms / 8.5 FPS to 6.45 ms / 155 FPS; buffer allocations/frame
fell from 706 to zero after warm-up, geometry uploads from 33.7 to 0.20 ms and
retirement from 72.6 to 0.001 ms. GPU execution time is not timestamp-profiled.
Take the Car before: 92-179 ms/frame, 502-878 allocations/frame, 24-43 ms upload;
after: representative settled intervals 10.7 ms / 93 FPS, zero allocations and
0.032 ms uploads, despite about 570 transient submissions/frame. Different mission
views are not a controlled FPS comparison. Settled static textures do not reupload;
occasional font/interpolated image updates remain real uploads/fence waits.
Two frame slots now retain mapped 4 MiB vertex/1 MiB index pages. A completed slot
fence resets cursors; overflow adds a page without replacing recorded buffers.
Immutable P3D allocations are unchanged. Offset-copy guards add two focused checks
(policy total 87). Sustained intro before/after captures, mission movement, resize,
restore, close and timed exit were inspected with zero validation issues. The
first requested 60-second stationary mission exited cleanly after about 30 s
(reason not established);
it supplied profiling but did not pass that duration gate. A 40-second sustained
intro and a 25-second timed mission passed separately. Evidence: `profile-*`.

## Gameplay effects and integration (2026-10-10)

Real firing exposed unsupported transformed 3D lines and projected decals.
Both now reuse the existing textured-quad path, retaining engine colors, UVs,
reciprocal-W and depth. Lines match GL33's three-pixel ribbon; billboards use
GPU clipping. Screen polygons select opaque/cutout/blended state from the
texture and section flags rather than always blending. Weather can restrict
a stored sky chain to its finest level; old images remain fence-owned.
Bank-backed mip-limit regression checks cover five real textures.
Blended billboards reject only near-zero alpha, preserving alpha-fog fades.

The radio replacement glyphs were valid localized UTF-8, not a Vulkan encoding
error. On Windows, missing glyphs may use the installed Microsoft YaHei face
through the existing FreeType atlas. Latin metrics remain unchanged; no fonts,
localization or game assets were edited.

RTX 4060 Ti normal Vulkan startup and stock Take the Car, Infantry and Heavy
Metal were exercised with exact-window input: movement/rotation, optics,
firing, weapon mode/grenade selection, reload input, map and pause/resume.
Take the Car ammunition decreased; Heavy Metal grenade count fell 6 to 5 and
its explosion dust/smoke billboards were visually inspected. Infantry and
Heavy Metal each passed a 60-second sustained interval, resize/minimize/restore
and normal close, with 10211/6598 submitted frames and zero Khronos core or
synchronization validation errors/warnings through shutdown. Settled intervals
were about 7.5 ms / 133 FPS and 12.5 ms / 80 FPS respectively, validation on;
these are sampled scenes, not whole-mission benchmarks. Evidence is under
`build/gameplay-live/{takecar-effects-decals,infantry-effects,heavy-metal-sky-fixed}`.

Final checks: Infantry pause-menu Abort was selected with keyboard input and
exited cleanly (3148 submissions); a final Heavy Metal grenade/resize/restore
run reached its 70-second timed exit (5860 submissions), both zero validation
issues. Normal Vulkan menu/intro timed exit passed too. Both build configurations
pass 49 focused cases (ON 4745 / OFF 4700 assertions) and Vulkan policy CTest.
Default GL33 visibly launched the stock menu in both executables and closed
normally. Stock-data/resource file counts, bytes and metadata hashes are unchanged.
Final evidence: `infantry-abort-final`, `heavy-metal-alpha-final`, `final-*`.

Native Shapes still use approximate unlit diffuse/emissive color and no native
distance fog; shadows, points and full material/effect parity are not implemented.
No campaign or complete mission was finished. The highest-value next rendering
milestone is native Shape lighting/fog using the existing engine inputs.

## Lighting/fog follow-up (2026-10-10)

Software TL now uses the same near-zero alpha rejection as 2D for blended
effects, retaining the opaque cutout threshold only for cutouts. NoZWrite is
independent of blending: opaque depth-read-only screen pipelines have their
own key. RTX Heavy Metal movement, firing/grenade, map/pause, resize/restore
and close were inspected (`build/lighting-live/m1-baseline`), zero validation
issues through shutdown. Shape state tests and policy CTest pass.

Native Shapes now use GL33's sun Direction/Diffuse/Ambient and material
ambient/diffuse/forcedDiffuse/emissive calculation, including DisableSun.
Packed normals retain GL33's negation; inverse-transpose world normals handle
rotation and nonuniform scale. Software TL colors are not lit again. Small
mapped uniform pages are frame-fenced, retained and reused; no synchronous or
per-section buffer allocation. Heavy Metal late-afternoon and Take the Car noon
captures were inspected, with close/resize/restore and zero validation issues
(2178/1236 submissions). A matched GL33 Heavy Metal capture has closely matching
terrain, foliage and weapon brightness/colors. Seven focused cases/58 assertions
and policy CTest pass. Evidence: `build/lighting-live/{m2-*,reference-heavy}`.

Native distance fog now uses the scene's weather/view-distance start/end range
and current fog color, with camera-relative distance and FogDisabled/NoDropdown
honored. Software TL carries its existing Fog8 visibility instead of recomputing
distance; IsAlphaFog effects retain their existing opacity fade, without a second
RGB fog mix. HUD/ordinary 2D remain unfogged. Shared fragment fog is applied before
gamma. Separate native/screen constant caches reuse frame-fenced uniform slices.
Ninjas dawn/fog gameplay and Convoy's distant hills, trees, road and vehicle cab
were inspected on RTX, with resize/minimize/restore and clean close (1594 and
692 submissions, zero validation issues). Ninjas GL33 reference brightness and
colors remain close. Eight focused cases/66 assertions and policy CTest pass.
Evidence: `build/lighting-live/{m3-fog,m3-convoy,reference-fog}`.

Night-eye color response now follows GL33's engine-provided coefficients,
before fog/gamma; the world resets it for HUD/menu drawing. Native Shapes support
up to eight engine-selected point/reflector lights, with GL33's night/material
modulation, attenuation and cone conventions. Stars use the engine's already-lit
screen points as subpixel-weighted quads; moon/flares consume software TL lighting.
Shadow Killer now launches rather than failing on DrawPoints/SpecLighting, and
its stars, moon, terrain and weapon darkness match the inspected GL33 capture.
Helitrain clear morning, Ninjas dawn fog, Heavy Metal dusk and an isolated
03:00 Take the Car copy were inspected against GL33 (stock missions unchanged).
The night town receives real local lights (246 light contributions/frame in the
sampled view) and has zero settled GPU allocations/texture uploads.

A live mission-command test changes visibility 900 -> 300 -> 900 and then fog
to 0.8. Recorded ranges change to 84..280, 264..880 and 64.8..216 metres. It caught
TextureVK's CPU GetPixel wrapping (1,1) to blue sky instead of clamping to the
horizon as PacLevelMem does. Correcting that produces the same grey distant
terrain as GL33. This does not change GPU repeat/clamp sampling. The corrected
run passed resize/restore/close with zero validation issues. Focused ON/OFF
checks pass (53/52 cases, 4776/4726 assertions), plus 89 driver-free policy checks.
Evidence: `build/lighting-live/{m4-*,reference-night*,reference-day,reference-visibility}`.

Final integration: Heavy Metal's 100-second timed run included a 60-second
sustained segment and grenade dust/smoke; Infantry and Take the Car each had
30-second sustained segments with movement, aiming/firing, weapon/reload,
map/pause and resize/minimize/restore. Infantry exited via pause-menu Abort;
Take the Car closed normally. Shutdown reported zero Khronos core/sync errors
or warnings (6887/6409/3810 submissions). Normal Vulkan menu/intro timed exit
also passed (3422 submissions). Real captures, not exit codes alone, were checked.
The old smoke fixtures needed an owned engine sun after lighting was introduced;
they now initialize it, and the cube has actual face normals. Shape, texture,
P3D model, WRP scenery and triangle smoke modes all passed visual/lifecycle checks.
Both builds and focused tests pass; default GL33 visibly opens the stock menu in
both executables. Stock data/resources counts, sizes and metadata hashes are unchanged.

Comparable Heavy Metal samples (800x600, validation on) changed from roughly
12..13 ms / 77..82 FPS to 14..15 ms / 67..72 FPS with lighting/fog. Infantry
settled around 8..9 ms / 111..125 FPS. These are similar-view samples, not a
controlled benchmark; loading, map changes and resize intervals are excluded.
Settled GPU allocations and texture uploads remain zero; transient uploads are
about 0.05 ms/frame in Heavy Metal, with no new synchronous per-draw uploads.
Evidence: `build/lighting-live/m5-*`.

Remaining differences: no object/vehicle shadows or full material specular
response; water parity is still approximate, with a thin bright shoreline strip
in Infantry absent from the GL33 reference. Two small water-state experiments
did not fix it and were reverted. No campaign/full mission was completed, and
night-vision goggles were not exercised. Basic object/vehicle shadow parity is
the highest-value next visual milestone.

## Projected-shadow work (2026-10-10)

The mip decoder retains its decoded prefix at valid one-pixel tails unsupported
by PacLevelMem, while rejecting truncated payloads and invalid dimensions.
Three existing PAA/PAC fixtures reproduce the old failure and now pass; five
sampled retail textures still pass (20 focused cases / 264 assertions).

Native alpha selection now honors IsAlpha/IsTransparent and material opacity.
Blending no longer implicitly disables depth writes: NoZWrite/NoZBuf select
independent native and screen pipelines, following GL33. Blended fades retain
the near-zero threshold; explicit cutout flags use GL33's 192/255 threshold.
Ten focused cases / 88 assertions and 89 policy guards pass. RTX Take the Car
foliage, buildings, weapon/HUD and model-smoke Jeep/building were inspected;
a matching GL33 mission view was captured. Timed and normal-close Vulkan runs
report zero validation errors/warnings. Evidence: build/shadow-live/m1-*.

Projected shadows now consume Scene/Object's existing projected Shapes. A
D32S8 (D24S8 fallback) attachment and per-pass stencil clear retain receiver
depth; EQUAL-zero/INCREMENT exclusion and ZERO/ONE_MINUS_SRC_ALPHA blending
darken each pixel once across all casters. A dedicated late-test fragment shader
keeps texture holes out of the stencil mask. Native and software draws use
engine opacity and shadow-distance inputs, without ordinary lighting/RGB fog.
The normal Scene capability is enabled. Invalid pass nesting/draw state fails.

Stock B02HMMWV at 13:00 visibly casts its projected vehicle shadow on the road
and roadside while driving in third person; nearby scenery/vehicle depth remains
intact. Take the Car's ordinary scene also runs. RTX core/sync validation through
normal close: 5244/4811 submissions, zero errors/warnings. Software shadow draws
were measured; cached native shadow coverage is checked in the next milestone.
Build, ten focused Shape cases and 92 policy guards pass. Evidence: m2-*.

Cached terrain-fitted shadow Shapes now accept the engine's immutable small
discardable buffers; other dynamic geometry still uses software TL. Native
shadow fading follows Scene::FogExponential rather than a linear approximation.
Stock HMMWV runs exercised both moving software shadows and stopped cached
native shadows (5 draws / 174 triangles per frame), with no steady-state GPU
allocations or texture uploads. Matching GL33 vehicle shadow shape/opacity,
resize and minimize/restore captures were inspected. Normal-close validation
was clean; 30 focused decoder/Shape cases (354 assertions) and 92 guards pass.
Evidence: m3-hmmwv, m3-final. A separate low-sun probe exposed an existing
IsLight flare-decal rejection; that is addressed in the appearance milestone.

Low-sun checks exposed IsLight decals being rejected (and software lights using
ordinary alpha blending). Screen/software light draws now use GL33's additive
SRC_ALPHA/ONE blend and disabled RGB fog, with distinct depth/blend pipelines.
Native world lighting is unchanged. Ten Shape cases / 93 assertions pass.

At matching B02HMMWV viewpoints, Vulkan and default GL33 captures at 13:00,
18:00 and 23:00 show matching projected vehicle/scenery shadow direction and
low-sun length. Existing runtime settings enabled object shadows (the isolated
profile originally had objectShadows=0). Buildings, trees and the roadside
monument cast in the ordinary Scene pass. At 19:30/23:00 projected submissions
fall to zero. Nearby nighttime brightness also occurs in GL33. A second Jeep
was placed through the existing script API for an overlapping-caster check;
the combined vehicle/building shadows did not accumulate extra dark bands.
These controlled time/placement probes are not claimed as untouched mission
playthroughs. One malformed probe aborted via the test harness; the corrected
run closed normally with 2809 frames and zero validation errors/warnings.
Evidence: m4-conditions, m4-reference, m4-overlap. Road filtering and foliage
edge differences versus GL33 remain outside this shadow change.

Final RTX 4060 Ti validation: normal --render vk startup, animated menu world,
menu-selected Heavy Metal and walking near its M1A1/building/soldier shadows
ran for six minutes (25888 submissions). Infantry at stock dawn ran 200 seconds
(11545): movement, camera rotation, sights, firing (ammo decreased), reload,
third-person animated shadow, weapon selection, grenade explosion/fading smoke,
map, pause/resume, resize and minimize/restore were visually checked. Stock
Shadow Killer at 03:00 exercised movement/sights/firing with zero projected
shadow submissions; keyboard-selected Mission Abort exited the test-mission
run normally (4771). All these runs reported zero core/synchronization validation
errors/warnings through teardown. HMMWV driving was checked earlier. These are
bounded gameplay checks, not completed missions or full gameplay certification.

Matched 800x600 HMMWV town profiles, same build/stock mission/viewpoint with
isolated object+vehicle shadow settings off/on: the last five settled two-second
windows averaged 15.53/17.31 ms (64.4/57.8 FPS), CPU recording 11.25/12.33 ms,
and window p95 12.13/13.12 ms. Shadows-on submitted 114 cached draws/frame;
both had zero transient allocations and texture uploads in those windows.
Fence time was 0.135/0.161 ms. These are short live samples, not GPU timings or
a deterministic benchmark. No allocator/scheduler changes were warranted.

Both build configurations pass; Vulkan has 30 focused decoder/Shape cases
(357 assertions) plus 92 policy guards, GL33-only has 19 decoder cases
(214 assertions). The GL33-only default menu was captured and closed normally.
Stock data/localization/resources were not edited. Evidence: build/shadow-live/m5-*.
Remaining limitations: legacy terrain-fitted shadows, not general object-to-object
shadow mapping; no exhaustive proxy/LOD coverage. The map remains expensive
(about 5 FPS / 11000 screen submissions in Infantry), and distant white water/
shoreline patches were observed but not investigated in this shadow task.
The next high-value performance step is reducing the map's repeated screen draw
overhead, independently of projected shadows.

## Ordered map batching: alpha prerequisite (2026-10-10)

Software-transformed explicit IsTransparent sections now use GL33's 192/255
cutoff even when decoded texture alpha suggests a different classification.
IsAlpha/IsAlphaFog/additive fades retain near-zero rejection; projected shadows
retain their existing independent alpha policy. Both build configurations and
11 focused Shape cases (131 assertions) pass, plus 92 driver-free policy guards.
RTX Infantry gameplay, map open/close, resize and minimize/restore were inspected
in `build/shadow-live/map-m1-baseline`. This is still the unbatched renderer.

Ordered screen batching now retains one consecutive run of compatible polygons
in CPU vectors, fan-triangulates with 32-bit indices, and uploads once through
the existing frame-fenced transient pages. Image version/sampler, cutoff,
blend/additive mode, independent depth state and scissor must match. Vertex
reciprocal-W, UV, color and fog remain untouched. Gamma, fog color and night-eye
changes flush before mutation; direct draws, diagnostic draws, shadow boundaries,
clears, explicit FlushQueues and frame end also flush. Runs are bounded at 65,536
vertices/196,608 indices; an oversized individual polygon flushes immediately.
Dynamic textures allocate new immutable image versions, with queued and recorded
draws retaining their original version. Native P3D and software shadow geometry
are not batched or reordered.

Both builds, 13 focused cases/156 assertions and 92 policy guards pass. RTX
Infantry map/gameplay, map close, resize/restore and normal close were visually
checked (`build/shadow-live/map-m2-batched`): 2,667 submissions, zero core/sync
validation errors/warnings through teardown. Initial map observations reduce
roughly 11,800 polygons to about 1,030 screen draws; controlled A/B measurements
follow below. `CWR_VK_SCREEN_BATCH=0` disables batching for process-local A/B
profiling; the default is enabled. CWR_VK_PROFILE now distinguishes queued 2D
polygons, emitted batches, all actual screen draws (including software geometry),
and polygons per batch. Existing transient counts still count geometry uploads.

### Real Infantry map comparison

RTX 4060 Ti, 800x600, Khronos core/synchronization validation on, same executable
and isolated profile, stock Infantry default map area/scale. Batching off/on
used `CWR_VK_SCREEN_BATCH=0`/default. Simulation was stopped with the existing
harness after loading for stationary map sampling; these are controlled map
measurements, not mission playthroughs. Five consecutive settled two-second
windows per run were retained before interaction/capture; frame-weighted results:

| Metric | Batching off | Batching on |
| --- | ---: | ---: |
| Frame time / FPS | 152.72 ms / 6.55 | 21.06 ms / 47.49 |
| Existing CPU recording interval | 133.51 ms | 14.13 ms |
| Submitted 2D polygons/frame | 11,790 | 11,836 |
| Actual Vulkan screen draws/frame | 11,809 | 1,033 |
| Emitted 2D batches/frame | 11,790 | 1,014 |
| Polygons/batch | 1.00 | 11.67 |
| Transient allocations/frame | 0 | 0 |
| Geometry upload time/frame | 0.345 ms | 0.105 ms |

This is 7.25x measured FPS and 91.25% fewer screen draws. Small differences in
radio/tutorial overlays account for the 0.39% polygon-count difference; map area,
scale and geometry are matched. The existing record_ms interval includes
fence/acquire/present waits: respectively 2.512/0.951/5.685 ms before and
0.233/0.017/0.476 ms after. It is not a GPU timestamp or pure CPU-cycle metric.
No texture uploads occurred in these sampled windows. Evidence:
`build/shadow-live/map-m3-{off,on,gl33}` and `build/map-perf/m3-matched-samples.csv`.

Default and zoomed Vulkan/GL33 captures were compared for terrain, roads, grid,
tree/building symbols, unit markers, labels, notebook, compass and translucent
overlays. Zooming, right-button panning, different areas and map open/close were
exercised. No missing symbols, stale textures, incorrect order or clipping was
observed. Both Vulkan runs closed with zero validation errors/warnings (680 and
8,904 submitted frames). GL33-only default renderer also displayed the real map
and closed normally. The map is now responsive; remaining cost is dominated by
recording about 1,000 state-separated runs, not allocations or upload bandwidth.
No additional localized hotspot was established that warranted expanding this
change. Redundant command/state binding within those ordered runs is a useful
next profiling target; sorting transparent polygons remains out of scope.

### Gameplay and final regression checks

The existing isolated GOG data/profile setup was used with object and vehicle
shadows enabled in copied profiles. Stock missions were loaded through the normal
mission-test path; no smoke geometry replaced gameplay. Evidence is under
`build/shadow-live/map-m4-*`:

- Infantry: movement/rotation, sights, firing (M16 ammo 30 -> 29), reload,
  weapon switching, grenade throw (6 -> 5), visible explosion/dust/smoke,
  map open/close, zoom-out and pan, pause/resume, third-person animated geometry,
  resize/minimize/restore, and keyboard-selected Mission Abort. Normal shutdown:
  11,746 submissions, zero core/synchronization validation errors/warnings.
- Take the Car: movement/rotation, sights/fire/reload input, foliage and HUD,
  map zoom/pan through town/road/forest areas, pause/resume, resize/restore and
  normal close. 3,814 submissions, zero validation issues through destruction.
- B02HMMWV: third-person driving (position change verified), exhaust/dust,
  moving vehicle and scenery projected shadows, map zoom/pan, pause/resume,
  resize/restore and normal close. 3,195 submissions, zero validation issues.

Comparable stationary 800x600 gameplay samples with the same executable/profile
and batching off/on showed no systematic 3D regression. Representative frame
interval ranges were 11.65..18.08 -> 10.40..12.67 ms in Infantry and
19.54..24.58 -> 13.61..13.88 ms in Take the Car. Corresponding recording intervals
were 10.64..13.55 -> 7.47..9.39 ms and 12.46..15.75 -> 12.96..13.18 ms.
AI, weather, radio overlays and window scheduling vary between live runs, so
these ranges are regression observations, not deterministic whole-mission gains.
Settled samples retained zero recurring transient allocations.

Normal Vulkan menu/animated intro reached a 20-second timed shutdown (exit 0,
1,989 submissions, zero validation issues). Default GL33-only menu/intro was
visually inspected and timed out with exit 0. Texture/Shape, real-model and
triangle diagnostics were visually checked and timed out with exit 0, respectively
1,833 / 1,771 / 1,841 submissions and zero validation errors/warnings. A further
batching-off menu comparison retained the same small intro projection artifact;
it also exited 0 with clean validation. Wide-map rectangular offshore contour
lines were reproduced in GL33 and are not a batching regression.

Final Vulkan-enabled and GL33-only builds pass. Focused Shape/batch/decoder tests:
32 cases / 274 assertions ON, 31 / 269 OFF; all 92 driver-free policy guards pass.
The batching tests cover fan order/attributes, 32-bit indices, bounded runs,
state splits and image-version lifetime. Live mixed UI/software/native/shadow
frames, font updates and teardown showed no missing or unflushed geometry.
All 7,606 stock-data/resource files retained their original count, lengths and
timestamps; localization and GL33 rendering code were not edited. Audio mute and
Caps Lock were restored to their initial states. These are bounded gameplay
checks, not completed missions or exhaustive coverage of all map scales/proxies.

Remaining limitations include pre-existing water/shoreline artifacts and material
parity gaps. Map performance now depends mainly on the remaining ordered draw
runs and CPU command recording; GPU execution is not timestamp-profiled. The
single next performance milestone is measuring and eliminating redundant Vulkan
pipeline/descriptor/viewport/scissor bindings within those runs, while retaining
exact draw order and invalidating cached state at command-buffer boundaries.

## Command recording state cache (2026-10-10)

VulkanContext now remembers only the state recorded in the current command
buffer: pipeline, the two immutable texture/sampler descriptor handles, lighting
set plus dynamic offset, vertex/index buffers plus offsets/index type, viewport,
scissor and all 112 push-constant bytes. Identical state commands are omitted;
draws, screen-batch boundaries, shaders and frame-fence resource ownership are
unchanged. Resetting a command buffer, destroying/recreating the swapchain and
the different-layout diagnostic triangle invalidate the cache. Clear and shadow
boundaries retain Vulkan state legally; changed shadow pipeline/constants still
bind normally. The existing native/screen lighting-data caches remain separate
from the actual descriptor binding cache.

CWR_VK_PROFILE adds emitted state-command counts, submit time, resource-retention
search time and binding-path time (including lighting-data lookup/upload).
command_ms measures elapsed reset/begin through end-command-buffer, excluding
fence/acquire/retirement/submit/present; it includes engine work between draws
and is not a GPU timestamp or pure CPU-cycle measurement. Existing record_ms
retains its previous broader meaning. No new profiling switch was added.

The Vulkan build, 32 focused Shape/batch/decoder cases (274 assertions), and 114
driver-free policy checks pass. The new checks exercise real DrawMesh calls and
verify identical-state suppression, offsets, index types, sampler/image versions,
lighting set/offset changes, clipping, viewport, complete push bytes, native /
screen / shadow transitions, diagnostic-layout restoration and reset. Real
800x600 Infantry map runs have clean core/synchronization validation through
normal shutdown. Matched timing and wider gameplay verification follow below.

### Measured remaining cost and stop decision

RTX 4060 Ti, stock Infantry map, 800x600, isolated copied profile, ordered batching
enabled in both binaries. Baseline is 7d98998 plus the same profiling timers;
cache is e20d527's implementation. Simulation was frozen through the existing
harness after loading, then the real map was opened with M. Each result uses the
first five complete upload-free map windows, weighted by frame count. Evidence:
`build/shadow-live/cmd-matched-{baseline-on,cache-on,cache-off}` and the initial
`cmd-m1-no-validation` baseline; `build/map-perf/commands-matched.csv` holds the
summary. The validation-off pair has identical polygons, batches and draws.

| Per-frame measurement | Validation on: before -> after | Validation off: before -> after |
| --- | ---: | ---: |
| Observed FPS | 52.18 -> 98.26 | 158.28 -> 158.35 |
| Observed frame interval | 19.166 -> 10.177 ms | 6.318 -> 6.315 ms |
| Command recording interval | 12.654 -> 8.458 ms | 2.635 -> 2.450 ms |
| Binding path | 7.210 -> 3.302 ms | 0.315 -> 0.133 ms |
| Retention searches | 0.031 -> 0.031 ms | 0.029 -> 0.027 ms |
| Geometry upload | 0.100 -> 0.096 ms | 0.089 -> 0.087 ms |
| Queue submission | 0.593 -> 0.573 ms | 0.027 -> 0.028 ms |
| Fence wait | 0.268 -> 0.264 ms | 0.002 -> 0.003 ms |
| Acquire | 0.044 -> 0.041 ms | 0.002 -> 0.002 ms |
| Present | 0.481 -> 0.487 ms | 3.396 -> 3.582 ms |
| Actual screen draws | 1,034 -> 1,034 | 1,033 -> 1,033 |
| Screen batches | 1,015 -> 1,015 | 1,014 -> 1,014 |
| Settled GPU allocations | 0 -> 0 | 0 -> 0 |

Important measurement limitation: wall-clock FPS is noisy, not the isolated
cache gain. The validation-on baseline includes stalls outside the measured
recording interval. Two additional validation-off baseline runs using fixed
startup timing (`cmd-matched-baseline-off`, `cmd-repeat-baseline-off`) measured
94.77 and 104.94 FPS, yet their recording intervals remained 2.588/2.725 ms and
their broader record_ms remained 6.093/6.074 ms. Individual settled windows in
those runs also reached about 158 FPS. Do not interpret those stalls, or the
entire validation-on FPS difference, as work removed by the cache. Their source
outside VulkanContext was not established; no unrelated scheduling change was
made. The directly measured benefit is about 4.20 ms of recording with validation
and 0.19 ms without it. Normal validation-off presentation already dominates the
remaining binding work; no meaningful steady FPS increase is claimed there.

Validation-on production state commands/frame (diagnostic triangle commands are
not counted by these DrawMesh counters):

| Command | Before | After |
| --- | ---: | ---: |
| Pipeline | 1,034 | 7 |
| Texture descriptor pair | 1,034 | 1,030 |
| Lighting descriptor + dynamic offset | 1,034 | 1 |
| Vertex buffer + offset | 1,034 | 1,034 |
| Index buffer + offset/type | 1,034 | 1,034 |
| Viewport | 1,034 | 1 |
| Scissor | 1,034 | 15 |
| Push constants | 1,034 | 10 |
| Total | 8,272 | 3,132 |

This eliminates 5,140 commands/frame (62.1%) without eliminating a single draw
in the matched pair. Texture/sampler transitions and transient geometry offsets
really change and are deliberately still bound. Tutorial/radio overlays differ
slightly between runs (11,836 vs 11,790 polygons in the validation-on pair);
the off pair has 11,836 in both. The earlier 1,030..1,034 draw variation also
occurred in the baseline. No batcher or draw-order code changed.

Linear retention searches are only about 0.03 ms on the map and about 0.10 ms
in the checked Take the Car/HMMWV gameplay windows. They do not justify another
cache or ownership change. No additional hotspot optimization was retained or
needed. Further map micro-optimization is not justified by validation-off results.

### Live regression verification

Current evidence is under `build/shadow-live/cmd-*`, separate from the previous
batching milestone. All three gameplay missions used copied profiles with
object/vehicle shadows enabled and unchanged stock GOG assets:

- Infantry: movement and camera rotation, sights, firing (M16 30 -> 29), weapon
  switching, two grenade throws (6 -> 4), visible explosion smoke/dust,
  third-person animated geometry and projected shadow, map open/close/zoom/pan,
  HUD fades, pause/resume, resize/minimize/restore and Mission Abort. Exit 0;
  12,556 submissions, zero core/synchronization validation errors or warnings.
- Take the Car: movement/rotation, aim/fire/reload input, foliage cutouts,
  translucent tutorial/radio HUD, town/forest/road map, map transitions,
  pause/resume and resize/restore. Normal exit 0; 3,498 submissions, zero issues.
- HMMWV: third-person driving (position moved about 11 m), exhaust/dust, vehicle
  and object shadows, map zoom/pan/open/close, pause/resume and resize/restore.
  Normal exit 0; 3,632 submissions, zero issues.

Default GL33-only Infantry gameplay and map were visually compared with Vulkan:
terrain/roads/grid, unit markers, symbols, labels, translucent overlays and
notebook/compass layers remained faithful at the inspected scales and areas.
Zoom/pan/map close worked in GL33. No missing geometry, stale texture, incorrect
ordering or new clipping/flicker was observed. These are bounded checks, not
completed missions or exhaustive scene coverage. GPU execution was not timestamped.

Normal Vulkan menu/animated intro and diagnostic triangle rendered and reached
timed exit 0 with respectively 2,860 / 1,663 submissions and zero validation
errors/warnings through destruction. Default GL33-only menu/intro also rendered
and reached timed exit 0. The loader still reports the pre-existing stale EOS
overlay manifest independently of the clean Khronos validation counters.
Both build configurations pass; focused tests remain 32 cases/274 assertions ON,
31/269 OFF, plus 114 driver-free guards. No GL33 rendering code, stock assets or
localization changed: all 7,606 protected files retain count, length and timestamp.
Original endpoint mute and Caps Lock state were preserved; no game remains open.

The highest-value next visual-parity milestone is the pre-existing white
water/shoreline artifact visible in Infantry, not further map command reduction.

## Water/shoreline cause and targeted correction (2026-10-10)

The white Infantry strip was reproduced against GL33 with the same scripted
camera position/target, dawn time, weather, visibility, 800x600 and copied profile.
A temporary magenta tag on native SpecularTexture draws covered exactly the
white pixels: these are landscape water Shapes, not software geometry, missing
terrain, a surface overlay or a stale UI texture. The tag and trace logging were
removed after diagnosis. Evidence: `build/shadow-live/water-{baseline-vk,
baseline-gl33,trace-vk,trace-gl33,fixed-vk}`.

The actual native section flags are 0x02002000 (SpecularTexture | NoClamp), with
animated `data/more_anim.*.pac` primary textures and `data/specular_dx.paa` as the
secondary texture. LandscapeRender's WaterFlags deliberately do NOT include
IsWater. GL33's captured state is PSDetail (shader 1), SpecularTex (format 2):
TGDetail supplies 32x secondary UVs and PSDetail multiplies lit primary RGB by
secondary alpha * 2. Vulkan had inferred a water bump shader from the texture
source flag alone, decoded secondary RGB as a normal at unscaled UVs, and added
a large clamped light dot product. This erroneous additive term caused the white
patches. Neither a different depth state nor forcing the unused IsWater/TGWater
branch addresses that mismatch. The earlier unsuccessful state experiments
recorded above were not adopted.

Vulkan now derives secondary shader mode from the existing shared render-pass
descriptor, independently of the secondary texture source. This selects the
already-implemented detail-alpha shader operation for ordinary stock water.
Original textures/mips, animated frame selection, material lighting, fog, gamma,
repeat samplers, opaque depth-tested/depth-writing geometry and render order are
unchanged. No colors are substituted and no water geometry/effects are hidden.
The explicit IsWater family remains unsupported rather than being silently
enabled; no speculative new water shader or global sampler/fog change was added.

The original white strip is gone in the matched fixed capture, with a second
bay-facing capture showing ordinary near/distant water and shoreline continuity.
Both builds pass: 33 focused cases / 285 assertions ON, 32 / 280 OFF, plus 114
driver-free policy checks. The new regression locks the distinction between
SpecularTexture and IsWater and preserves sampler/alpha/depth policy. The first
fixed Infantry run closed normally with 10,834 submissions and zero Khronos
core/synchronization errors or warnings. Wider water-condition checks follow.

### Matched water-condition comparison

Six fixed-camera pairs were inspected in `build/shadow-live/water-m3-vk-ready`
and `water-m3-gl33`, using the GL33-only executable with no renderer override.
Both used the same copied shadow-enabled profile, 800x600, frozen simulation,
July 5 1985 and identical camera targets/positions. The original Infantry shore
and west-facing bay were checked at 05:35 (overcast 0.487594, fog 0, visibility
900 m); the bay, opposite coast and a low near-water view at noon (overcast 0.3);
the latter again with fog 0.65 and visibility 300 m. Harness receipts confirmed
matching camera positions and time. Texture-animation phase and NPC positions
are not synchronized, so these are controlled visual comparisons, not pixel diffs.

The erroneous white strip is absent. Ordinary water color/brightness, near-wave
texture, shoreline transitions and fogged horizon are recognizably faithful to
GL33 across these views. No new seam, missing water or depth overlap was seen.
Vulkan still has visibly coarser distant terrain/detail and smoother distant
water than GL33; that existing texture/LOD difference is not concealed by this
fix. Full native material-specular parity and the unused explicit IsWater family
are not claimed. No additional shader or global sampler change was justified by
these comparisons. Both comparison processes closed normally (Vulkan 3,127
submissions, zero core/synchronization validation errors or warnings).

### Water performance and final gameplay verification

Matched profiling used the stock Infantry bay at noon, the same camera and
shadow-enabled profile above, 800x600 on the RTX 4060 Ti. The saved 0b80145
executable was compared with the fixed executable. Each result aggregates the
last five complete two-second windows after settling (frame-weighted times,
FPS from total frames / total elapsed time). No build ran during sampling.
Evidence: `build/shadow-live/water-perf-{before,after}-{on,off}` and the reversed
order `*-off-repeat` pair.

| Validation / sample | Baseline FPS / frame ms | Fixed FPS / frame ms | Command recording ms, before -> after |
| --- | --- | --- | --- |
| On | 158.16 / 6.323 | 158.13 / 6.324 | 2.232 -> 2.223 |
| Off, first pair | 121.18 / 8.252 | 107.13 / 9.334 | 0.652 -> 0.603 |
| Off, reverse-order repeat | 158.42 / 6.312 | 158.28 / 6.318 | 0.605 -> 0.647 |

The first validation-off pair contains intermittent wall-clock stalls in BOTH
binaries outside the measured command interval. The repeat does not reproduce
the apparent FPS loss. Legacy `record_ms` (including presentation/waits) stayed
about 6.01 ms off and 6.03 ms on; settled geometry upload was 0.006-0.008 ms/frame,
with zero transient allocations and no texture uploads in the sampled windows.
Both binaries recorded identical sampled draw/state counts (189 lit draws,
109 screen draws, 14 pipeline / 218 texture-set binds per frame). This is a
visual correction, not a measured performance improvement; the stable repeat
shows no meaningful regression. GPU execution was not independently timestamped.

Bounded real-game checks, with core and synchronization validation enabled:

- Infantry: moved about 4 m, rotated toward/away from shore, aimed/fired/reloaded,
  threw a grenade (6 -> 5 with visible explosion smoke), opened/zoomed/closed the
  map, checked HUD/third-person soldier/projected shadows, paused/resumed, resized
  to 960x640 and minimized/restored. Running water was inspected in two captures
  ten seconds apart; animation/shore movement continued without new white patches.
  Mission Abort exited 0: 12,694 submissions, zero validation errors/warnings.
- Take the Car: movement/rotation, aim/fire (HUD 30 -> 29), foliage and building
  textures, translucent radio/tutorial HUD, map open/close, pause/resume and
  900x650 resize/minimize/restore. Normal close exited 0: 3,287 submissions, zero
  validation errors/warnings.
- HMMWV: cockpit and third-person driving (about 20 m), exhaust/dust, vehicle
  and object shadows, map open/close. A temporary runtime placement at
  [8110,5160,0] facing west added daylight coastal driving/water inspection;
  no mission file was edited and this was not mission completion. Water,
  animated flag and vehicle shadow remained intact across pause/resume and
  900x650 resize/minimize/restore. Normal close exited 0: 7,792 submissions,
  zero validation errors/warnings.

Evidence is under `build/shadow-live/water-game-{infantry,takecar,hmmwv}`.
Final Vulkan menu/animated intro reached timed exit 0 (2,482 submissions, zero
validation issues). Default GL33 in the Vulkan-enabled executable and the
GL33-only executable both rendered their menus/intro and reached timed exit 0.
Both final builds pass; focused tests pass 33 cases / 285 assertions ON and
32 / 280 OFF, plus 114 driver-free guards (their intentional mocked teardown
error is not a live validation error). The stale EOS overlay manifest loader
message remains an unrelated environment issue. All 7,606 protected stock and
resource files retain their initial count, length and modification timestamp;
no localization or GL33 rendering implementation changed.

Next highest-value visual-parity work: investigate distant/oblique texture
filtering and mip/LOD parity with GL33. Its linear samplers enable up to 16x
anisotropy whereas Vulkan currently uses plain trilinear sampling; the observed
distant-water smoothing is a concrete comparison target, not grounds for a
speculative global sampler change in this patch. Full material-specular and
explicit IsWater support remain separate limitations.
