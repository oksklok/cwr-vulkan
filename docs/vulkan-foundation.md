# Vulkan backend development

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
Normal menu/intro rendering is demonstrated; mission gameplay is not yet tested.
