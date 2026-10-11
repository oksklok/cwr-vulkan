# Vulkan backend development

## Optional anti-aliasing and world render scale (2026-10-11)

Off (default), FXAA, SMAA 1x, TAA and MSAA 2x/4x/8x are implemented.
Independent world render scales are 100% (default), 125%, 150% and 200%. TAA is
opt-in, with real instance/deformation motion history. Vulkan remains
opt-in; GL33 rendering, assets, localization and gameplay behavior are unchanged.
There is no graphics menu, upscaler, dynamic resolution or AA stacking.

Developer evaluator commands (process-local, not persisted):

```sqf
triAA "off"       // also "fxaa", "smaa", "taa", "msaa2", "msaa4", "msaa8"
triAA "motion"    // visualize velocity/rejection while taa is selected
triAA "motion-off"
triRenderScale 150
triAA "status"    // actual/requested mode, actual/requested scale, sample mask
```

Mode changes report `OK` and take effect at the next frame's existing
idle/recreation boundary. Unsupported sample counts return `UNSUPPORTED` and
retain the previous selection; they never silently become a different mode.
Invalid scales return false. TAA requires independent attachment blending;
unsupported devices retain their previous mode. GL33 reports unsupported for
these commands. Capabilities intersect
the exact color and sampled depth/stencil image formats with device framebuffer
and sampled-depth sample-count limits. The RTX 4060 Ti reports 1/2/4/8 support.

### Rendering and algorithms

The existing `BeginWorldEffects` / `FinishWorldEffects` boundary brackets a
separate world target only when AA or render scale requires one. Landscape,
sky, native objects, software TL terrain, transparency, water, fog, projected
shadows and rain complete there. SSAO reads that world's depth. AA/downsampling
then composite world color and depth into the original native-resolution scene
target before cockpit/weapon draws, HUD, map labels and menus. Gamma still runs
once on the completed frame, including overlays. Off at 100% uses the original
scene attachments without a world copy or AA images.

- FXAA uses NVIDIA FXAA 3.11's complete PC quality path, preset 29, 0.5 subpixel
  filtering, 0.125 contrast and 0.0312 dark thresholds. RGB luma is calculated at
  the reference fetch sites to include chromatic edges without a luma-copy pass.
- SMAA 1x uses the upstream HIGH preset with color-edge detection, blending
  weights (including diagonal/corner detection), and neighborhood blending.
  Original area/search table bytes are embedded, losslessly expanded through
  the existing texture uploader, and sampled bilinearly without anisotropy.
  Edge/weight targets are zero-cleared UNORM images. See
  [reference provenance and licenses](../engine/PoseidonVK/ThirdParty/README.md).
- MSAA uses real 2/4/8-sample color and depth/stencil attachments, Vulkan color
  resolves and per-sample projected-shadow stencil tests. Opaque cutout world
  materials use derivative-scaled alpha-to-coverage around the existing cutoff;
  blended smoke, glass and water keep their ordinary blend paths. Sampled depth
  uses a minimum-sample resolve in the shader for SSAO and subsequent depth
  composition. This avoids requiring a newer depth-resolve extension.
- Supersampling integrates the exact source-pixel area of each output pixel,
  including fractional 125/150% footprints. It uses actual rounded-up scaled
  dimensions, not a larger window. Depth conservatively takes the nearest
  covered surface; cockpit depth clears retain their original ordering.

World stencil stays attached throughout world drawing and CSM interruptions;
it is not filtered or used as AA scratch. After the world boundary no projected
shadow pass consumes it. Native-resolution overlay stencil retains its own clear
value. SSAO modulates the resolved world color after the final multisample pass,
so a later resolve cannot overwrite AO. CSM resumes the correct world target
and sample-count pipeline bank. Every boundary invalidates cached draw bindings.
FXAA and SMAA at 100% write directly into the native scene, avoiding an extra
full-frame copy; scaled post-AA uses one reusable intermediate before downsampling.
All GPU target resources follow swapchain lifetime; no GPU history images are
allocated per frame. CPU temporal geometry records are rebuilt each world frame.

### Temporal motion contract and resolve

The previous milestone's missing engine contract is now implemented, not replaced
by camera-only depth reprojection. `Object::_id` is unsuitable: it can be -1,
assigned again by landscape/load code, and changed by `SetID`. Object addresses
are recycled, and both Shapes and model proxy Objects are shared. A small
process-local 64-bit `RenderIdentity` member in Object and Shape gives every
lifetime/copy a fresh identity, independent of serialization. `Object::Draw`
pushes/pops the entire parent/proxy instance path through default-no-op Engine
hooks. GL33 ignores these hooks; no save format or stock asset changes are needed.

- Native immutable Vulkan meshes retain their actual previous MVP, keyed by
  full object path and Shape lifetime (including LOD). This path is not claimed
  to animate vertices: stock animated vehicles/soldiers use the software path.
- Software TL captures the actual post-animation, pre-clipping vertex positions
  in `FaceArray::Draw`. Previous homogeneous clip positions are retained by
  path, Shape and indexed topology; this includes soldier pose, turret, wheels,
  rotor and billboard transforms. Both CPU clip interpolation overloads carry
  the optional previous-position sidecar using the same intersection fraction.
  Pixel-projection conversion avoids applying camera FOV twice.
- Terrain and explicitly static terrain-fitted objects/roads reproject their
  known stationary world positions, including generated/clipped vertices.
  This static-world treatment is not applied to arbitrary moving objects.
- First appearance, changed LOD/topology, missing correspondence, ambiguous
  repeated instance draws, and unsupported effects reject history. Histories
  retain only consecutive world frames, not a persistent entity registry.

The world pass writes RGBA16F motion: XY is **previous UV minus current UV**,
including current/previous jitter, Z is expected previous-camera view depth in
meters, and W is 1 for valid opaque, .75 for valid cutout, 0 for unavailable, or
-1 for reactive coverage. Current sampled depth, jitter and validity are kept
separate from color. This is a usable future reconstruction input contract, not
an integration of DLSS/DLAA/FSR. Debug view maps XY displacement to red/green
around gray (80x UV scale); magenta means rejected, not zero velocity. HUD and
cockpit remain their normal colors. Debug switching also invalidates history.

TAA uses an eight-sample Halton(2,3) world-only projection jitter, two shared
RGBA16F history images on the existing graphics queue, nearest-depth velocity
dilation, previous-view-depth/disocclusion rejection, 3x3 YCoCg neighborhood and
variance clamping, velocity/luminance-sensitive history weighting, and bounded
8% detail restoration. History alpha stores view depth with a negative sign for
unreliable color, preventing reuse after smoke/water moves away. Sky has no
invented motion; only an immediate foreground edge can supply valid coverage.
SSAO resolves before TAA; downsampling and native overlays follow it; final
gamma remains last. Existing command buffers, frame fences, attachment resume
passes and swapchain recreation own all resources.

Alpha-tested foliage uses deformation motion and a lower history weight.
Partially transparent texels, smoke, water, additive/no-depth effects and
untracked draws reject history (including a one-pixel reactive border). Opaque
texels in stock blend-classified materials retain valid motion: rejecting the
entire texture would incorrectly exclude many stock buildings and plants.
Projected shadows preserve the receiver's velocity, with luminance/neighborhood
clamping for changing shadow color; they do not get fictitious caster velocity.

Camera subject/type changes, camera-effect changes, zero-time scripted camera
commits, scripted setPos/setPosASL/setDir on the camera subject (even small
teleports), large positional/directional/FOV discontinuities, NVG switching,
save loading, world cleanup, missing world frames, resize/scale recreation and
AA switching invalidate history. CPU history caches and GPU history validity
are independently gated, so stale records cannot make a reset frame valid.

### Limitations

Unsupported effects remain spatially sampled and can shimmer; smoke/transparency
are deliberately not accumulated. Sky/cloud texture animation and moving shadow
color have no independent velocity. Very thin fences, disoccluded edges and LOD
changes can still flicker, and TAA trades some fine texture contrast for stability.
It is not a promise of trail-free behavior for every mod/effect. No temporal
filter touches native cockpit instruments, weapons, HUD, menus or map text.
There is no SMAA+TAA stacking or vendor upscaler.

Spatial AA reduces visible stair steps but cannot provide temporal stability.
Fine foliage and thin geometry can still shimmer in motion; MSAA does not
supersample texture/shader evaluation. Supersampling is costlier and can soften
texture detail. Cockpit and weapon edges deliberately remain native and unfiltered.
Qualification is on the normal Windows UNORM swapchain; other GPUs and the sRGB
surface fallback are unqualified. Mode/scale changes reuse the existing
device-idle recreation and can hitch while pipelines/resources are rebuilt.

### TAA verification and performance

Vulkan-enabled and GL33-only RelWithDebInfo builds pass. Both pass the existing
`[vulkan-shape],[ShadowMath],[Conventions],[ShutdownOrder]` suites: 74 cases,
812 assertions, including new identity-reuse/copy, software projection and
homogeneous clipping checks. The Vulkan driver-free policy/lifetime executable
passes 311 checks; its intentional teardown-error fixture is not a live error.

Stock gameplay was exercised at 1920x1080 on RTX 4060 Ti / driver 617.14,
using isolated profiles and actual bounded keyboard/mouse input. Motion debug
was inspected before TAA color. Captures include consecutive 90-frame image
sequences (roughly 15-18 captured frames/second, not lossless 60-fps video).
Off/SMAA/TAA infantry comparisons reset position/direction but are not lockstep
AI replays; smoke, enemies and camera easing vary between sequences.

| Scene / local evidence run | Checks actually exercised |
| --- | --- |
| `taa-quality-infantry` | Independent soldier/limb/weapon vectors; running, firing enemies, blood/smoke, village foliage, fences and roofs; moving Off/SMAA/TAA image sequences |
| `taa-hmmwv` | Approximately 96 m driving over two bounded sequences; vehicle versus terrain velocity, dust rejection; TAA + SSAO/CSM, native cockpit at 150% world scale |
| `taa-tank` | Independent turret/hull velocity, turret aiming, about 11.6 m driving; optics and cannon fire (24 to 23 rounds), smoke; SSAO/CSM |
| `taa-cobra` | Rotor startup, takeoff from ground to 38.7 m, forward flight; cockpit/exterior switching, 125% scale, SSAO/CSM; translucent fast-rotor coverage rejected |
| `taa-night` | Night/NVG running; TAA + SSAO/CSM; 200% scale, resize to 960x640, minimize/restore |
| `taa-menu-save` | Main menu to stock Ambush intro/briefing/gameplay; ordinary menu Save/Load at TAA 150%; saved XY [8089.76,5153.36] restored after movement; map text; shoreline water/fog, scripted camera cut/teleport, SMAA/MSAA4/TAA switching |

Those six runs all closed normally (exit 0) with **zero core/synchronization
validation errors and warnings**. `taa-final-night` additionally exercises the
final small-teleport/NVG reset changes with GPU profiling disabled, covering
the ordinary acquire-semaphore wait path; firing/reload was checked (30 to 29
rounds), and its 9,338-frame shutdown also had zero validation warnings/errors.
`taa-gl33` launched the GL33-only binary with no renderer switch, exercised
stock infantry movement, rejected the Vulkan-only commands, and exited 0.
The host loader still reports its pre-existing stale Epic overlay manifest and
disabled third-party layers; these are distinct from Khronos validation output.
Local PNG sequences, action logs,
binary hashes, validation logs and timing JSON are in ignored
`build/shadow-live/taa-*`, not in the shipped assets.

Visual inspection found noticeably steadier foliage/roof edges than Off and
SMAA during the captured movement, with a modest loss of fine texture contrast,
not a blanket blur. No persistent soldier, vehicle or turret silhouette trails
were observed in the inspected sequences. Disocclusions reject instead of
dragging the old silhouette; smoke/water remain unfiltered. Native cockpit,
weapon, NVG overlay, HUD, briefing and map text stay sharp. This is bounded
stock-gameplay qualification on one GPU, not exhaustive coverage of every
animation, effect, frame rate or mod. The limitations above still apply.

`taa-perf` repeats Off/SMAA/TAA twice in the same frozen village setup used
below, native 1080p, projected shadows on, SSAO/CSM off, validation off. Each
mode settles for 14 seconds; the last four two-second profiler windows are
averaged. Existing frame timestamps include world draws, composition, overlays
and final gamma; new timestamps isolate the TAA resolve and are read only after
the existing frame fence, without a new wait.

| Mode | Whole-frame GPU ms, run 1 / run 2 | TAA resolve ms |
| --- | --- | --- |
| Off | 0.961 / 0.849 | — |
| SMAA | 1.369 / 1.426 | — |
| TAA | 1.543 / 1.546 | 0.351 / 0.351 |

Practical TAA overhead here is about 0.58-0.70 ms versus Off and 0.12-0.17 ms
versus SMAA, including motion output/world-target overhead, not just the resolve.
Automatic graphics clocks differed (Off 465/585 MHz, SMAA 705 MHz, TAA 825 MHz;
memory 810 or 5001 MHz). No clocks/power settings were forced. Consequently
these are practical elapsed GPU intervals, not fixed-clock throughput or a
guaranteed FPS gain/loss. Scaling increases both motion/history bandwidth and
resolve work. CPU per-instance/deformation bookkeeping also has a cost; the
renderer is not claimed to be GPU-limited in this scene.

### Earlier spatial-AA verification and performance

The results in this subsection predate the temporal implementation; references
to TAA rejection below describe that earlier executable, not the current mode.

Both Vulkan-enabled and GL33-only RelWithDebInfo builds pass. Each configuration
passes the existing `[vulkan-shape],[ShadowMath],[Conventions],[ShutdownOrder]`
suite (71 cases, 799 assertions). The driver-free Vulkan executable passes 309
checks, including new opt-in defaults, unsupported-mode retention, bounded
scale selection and odd-sized fractional extents. Its intentional teardown-error
fixture is not a live validation failure.

Live qualification used stock missions, isolated profiles, exact-window input,
1920x1080 and NVIDIA RTX 4060 Ti / driver 617.14. Developer commands positioned
the player and froze simulation for matched still comparisons; motion checks
then restored simulation. These are bounded movement sequences with screenshots
and position/ammunition evidence, not a continuous-video temporal-quality study.

| Stock scene | Live checks |
| --- | --- |
| Infantry / Ambush village | Off, FXAA, SMAA and all three MSAA counts; foliage, fences, rooftops, moving soldier; all four scales; SSAO + CSM; shoreline water/fog and camera transition |
| HMMWV | All modes driving (about 56 m total displacement); external and native cockpit; SMAA at 200% |
| Heavy Metal / M1 Abrams | All modes driving with SSAO + CSM; gunner optics and cannon fire, confirmed ammunition 24 to 23 |
| Ground Attack / Cobra | All modes in cockpit and actual flight (altitude above 30 m); external transition with SMAA at 150%; SSAO + CSM |
| Shadow Killer | All modes at night and under NVG, movement, native reticle/text/weapon; firing confirmed 30 to 29; MSAA 8x + 200% resize/minimize/restore |

Final spatial-code qualification runs `aa-q-infantry`, `aa-q-hmmwv`, `aa-q-tank`,
`aa-q-air` and `aa-q-night` all closed normally with exit 0 and **zero validation
errors or warnings**, including synchronization validation. The night and final
infantry runs also contain the final acquire-excluded GPU profiler. Earlier
`aa-final-infantry` includes map-label comparisons and SMAA + 150% resize/restore.
Local PNGs, action receipts, executable hashes and complete validation logs are
under ignored `build/shadow-live/aa-*`; they are not shipped as stock assets.

The final interface-name-only rebuild also passed `aa-menu-save` with profiling
disabled (the normal semaphore wait path): all modes in menus and moving stock
Ambush gameplay, briefing/map UI, and ordinary Save/Load with SMAA at 150%.
Loading restored `[8089.76,5153.36,0.00552177]` after movement; TAA and scale 99
requests left that active selection unchanged. Shutdown reported 19,671 frames,
zero validation errors/warnings and exit 0. Both final configurations launched
GL33 by default without `--render`; the new commands reported unsupported and
both exited normally (`aa-default-vkbuild`, `aa-default-glbuild`).

Visual review found reduced stair steps on rooftops, fences and silhouettes.
SMAA preserves more local texture contrast than FXAA; MSAA coverage smooths
cutout boundaries without applying a texture-wide post-filter. Fractional and
200% downsampling reduce fine-detail aliasing but cost more. HUD, map text,
cockpit instruments and weapon overlays remain sharp/native. No missing world,
stencil-shadow leak, broken water/NVG, or overlay filtering was observed in
these runs. This is not a claim of shimmer-free motion: spatial modes have no
history, and distant leaves/wires can still pop or sparkle. This earlier run
did not assess TAA ghosting.

The physical device supports every requested MSAA count. Unsupported-count
behavior is covered by the driver-free test (explicit rejection and previous
selection retention), **not** a second physical GPU or a forced driver failure.

GPU profiling is opt-in via the existing `CWR_VK_PROFILE` environment variable.
Two timestamps enclose the frame's GPU commands, including gamma. In profiling
mode only, the acquire-semaphore wait is moved to TOP_OF_PIPE so the starting
timestamp does not include waiting for the display's image. Results are read
after the existing frame fence, without an additional wait. Normal rendering
keeps its original late semaphore wait. This is an elapsed GPU command interval,
not an isolated shader-throughput measurement or a CPU/display frame time.

The matched village benchmark uses `[7088,6067,0]`, direction 90, external view,
16:00 clear weather, view distance 900, frozen simulation, projected shadows,
CSM off and SSAO off. Validation is disabled for timing. `aa-perf-infantry`
contains two 12-second sweeps with normal VSync. `aa-perf-unpaced` contains two
20-second sweeps requesting `triSetVsync 0`; despite the directory name and
mailbox selection, measured presentation remains around 162 FPS / 6.17 ms.
The configured CPU cap is 240, not the measured 162. No driver/global settings
or power plan were changed.

Timing statistics below use the second run, discarding the first two two-second
report windows after each mode/scale transition. Ranges are the retained window
means, not per-frame minima/maxima. Automatic GPU clocks varied from roughly
495 to 1950 MHz during sampling. **A fixed-clock incremental GPU cost is not
established by this experiment.** In particular, negative median differences
for lightweight modes must not be interpreted as AA speeding up rendering.

| Mode / scale | Median GPU ms | GPU window range ms | Median difference vs Off/100 ms (clock-confounded) |
| --- | ---: | ---: | ---: |
| Off / 100% | 1.763 | 1.416–1.803 | baseline |
| FXAA / 100% | 1.442 | 0.669–2.148 | -0.321; unresolved cost |
| SMAA 1x / 100% | 1.580 | 0.889–2.086 | -0.182; unresolved cost |
| MSAA 2x / 100% | 1.767 | 1.247–1.790 | +0.004; within variation |
| MSAA 4x / 100% | 1.868 | 1.219–1.869 | +0.106 |
| MSAA 8x / 100% | 1.998 | 1.453–2.019 | +0.236 |
| Off / 125% | 1.782 | 1.253–1.815 | +0.019; within variation |
| Off / 150% | 1.853 | 1.239–1.856 | +0.090 |
| Off / 200% | 1.980 | 1.363–1.981 | +0.217 |

Each row retains 16–18 report windows / 5,187–5,815 submitted frames. CPU/display
frame medians remain 6.173–6.178 ms across the table. These scene-specific results
are not a maximum-quality worst-case budget; combining MSAA, 200%, SSAO and CSM
was exercised for correctness, not exhaustively benchmarked. Reliable fixed-clock
incremental comparisons, other GPUs and a continuous-video motion study remain
unqualified.

## Optional Vulkan cascaded sunlight shadows (2026-10-11)

CSM remains **off by default**. The existing `triEnableShadowMaps` and
`triShadowSet*` developer commands now work with Vulkan; `triDisableShadowMaps`
restores the unchanged projected accumulator. No graphics menu or asset changes.

The backend consumes `Scene::RenderShadowMapDepthPass` / `ShadowCasterSet` and
the existing `BuildShadowCascadesTiered` results. Solid casters use front-face
culling; two-sided foliage/fences use the same 0.5 texture-alpha cutoff as GL33.
The existing draw/shadow LOD and caster-distance rules remain authoritative.
Each fence-retired frame owns a reusable D32 array (one to four layers), views,
framebuffers and descriptor. Solid/alpha streams upload once per frame through
the existing transient pages and are reused by every cascade.

An opt-in backend hook prepares the existing object list and depth maps before
landscape rendering. GL33 retains its original ordering. The depth pass suspends
the scene and resumes with color/depth/stencil LOAD operations, invalidating
the command cache. Native receivers use camera-relative world coordinates;
software-transformed receivers reconstruct them from clip W, projection and
camera rotation, including terrain. Sampling follows GL33's omni/frustum tier
selection, coverage fallthrough, quadratic cascade bias, 3x3 PCF, 15% transition
bands, distance/fog fade and sunlight strength. World-effect boundaries and
material flags exclude UI, sky and cockpit/weapon overlays. Maps reset each frame
and become inactive at night. Opt-in profiling reports cascade depth GPU time.

Verification: Vulkan and GL33-only builds, 56 focused cases / 738 assertions,
and driver-free Vulkan policy/command-cache checks pass. Both configurations
launch GL33 without a renderer override, with shadow maps disabled. GL33 renderer
files, stock assets and localization are unchanged. Software submissions without a shadow receiver keep
the original unlit path (including CSM OFF), rather than counting as native lit
draws or copying unnecessary receiver data.

Stock gameplay at 1920x1080 on the RTX 4060 Ti was exercised with exact-window
input and isolated profiles. Developer commands repositioned actors/set daylight;
these are rendering/gameplay checks, not completed mission playthroughs:

- Infantry village: walking, weapon fire, grenades (stock count 6 -> 4), moving
  soldier silhouettes, building receivers, foliage cutouts, map and pause.
- HMMWV: about 50 m of driving (42 km/h sampled), exterior/interior changes,
  recognizable vehicle shadows, fences and low-sun shadows (sun factor 0.82).
- Heavy Metal: tank movement, exterior/optics, cannon fire (24 -> 23 rounds;
  developer-assisted gunner seat change), smoke, CSM with SSAO, and live switching
  between four/2048 and two/1024 cascades.
- Ground Attack: stock Cobra cockpit/exterior, rotor startup and takeoff to
  about 39 m. HUD and cockpit overlays remain unshadowed; SSAO was also enabled.
- Shadow Killer: sunlight factor zero, no cascade passes, NVG, movement and
  HK fire (30 -> 29 rounds), CSM OFF/ON and SSAO.
- Normal menu -> Ambush -> save -> movement -> load restored the exact saved
  position with CSM still enabled. Water/shoreline with CSM+SSAO, resize,
  minimize/restore, and normal window close were checked separately.

All nine validated Vulkan sessions closed normally with zero Khronos core and
synchronization errors/warnings through teardown (57,649 submitted frames,
including the initial depth-pass development run). The final software fallback
adjustment was rechecked in the 2,500-frame tank firing/SSAO session. No new
water, cutout, smoke, gamma, specular, cockpit or command-cache regression was
observed; this is sampled coverage, not exhaustive certification.

Before/after captures, actions, launch hashes and validation logs are retained
locally under ignored `build/shadow-live/csm-*`. `csm-direct-{infantry,hmmwv,tank}`
contains repeated OFF/ON comparisons; `csm-{infantry,hmmwv,tank,night}-gl33`
contains GL33 shadow-map references. References are representative gameplay
views, not pixel-registered cross-renderer comparisons. Moving captures and
position/ammunition receipts accompany the static comparisons.

Performance: 1920x1080, RTX 4060 Ti, four 2048 cascades, 900 m view distance,
SSAO/validation off, frozen simulation and repeated 12 s OFF/ON phases. Values
are medians of 2 s profile windows (first three startup windows and mixed-toggle
windows excluded; 10-13 samples/mode). FIFO/display pacing limits these results;
depth GPU timestamps cover all cascades, **not** the complete frame GPU cost.

| Scene | Projected frame ms | CSM frame ms | CSM depth GPU ms | Geometry upload ms, OFF -> ON |
|---|---:|---:|---:|---:|
| Infantry village | 6.491 | 6.184 | 0.356 | 0.680 -> 0.441 |
| HMMWV road | 6.172 | 6.172 | 0.276 | 0.247 -> 0.249 |
| Heavy Metal, tank-mounted view | 6.173 | 6.172 | 0.407 | 0.109 -> 0.135 |

Steady-state transient allocations were zero. CSM removes projected draw/upload
work, explaining the village improvement; these are not uncapped throughput
claims. Two frame slots at four/2048 retain about 128 MiB of depth images.
Earlier `csm-perf-*` logs had managed stdout/stderr-pump stalls and are excluded:
repeating with direct file handles (`csm-direct-*`) removed those timing spikes.

Quality/readiness: usable as an opt-in developer enhancement, not a new default.
Keep GL33's four 2048 cascades, 0.00002 bias, 3x3 PCF, 40 m fade and existing
distance/LOD controls. No new scheduler or shadow LOD framework was justified.
Model silhouettes and alpha cutouts follow the shared caster system; terrain
receives but is not added as a caster. Low-detail caster silhouettes, fine
foliage self-shadow striping and finite-resolution shimmer remain visible in
both backends. Camera movement showed no severe cascade instability, but this
does not eliminate every LOD/cascade transition. Software TL reception modulates
its already-combined lighting, so it cannot isolate only the sunlight component
as precisely as a per-light shader. Sky, UI and cockpit/weapon overlays are
deliberately excluded. No claim of all-model/all-campaign coverage is made.

## Earlier backend qualification (before CSM)

Current status: Vulkan remains opt-in. Sustained mission-flow testing now covers
infantry, campaign transitions, tank driving/gunnery, helicopter flight/landing,
night combat and save/load. It exposed and fixed a combat crash from tracers
crossing the camera plane. See the final gameplay section for actual coverage,
assisted transitions and readiness limits; no genuine mission win is claimed.
Gameplay lighting/fog, projected shadows, ordered screen batching and composed
framebuffer gamma remain supported. The measured Infantry map result is
6.55 -> 47.49 FPS with 91.25% fewer screen draws. The dated milestones below are
historical; their earlier unsupported-feature lists are superseded by later work.

Repository migration: `vici1129/cwr-vulkan` is an independent clone with its own
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

## Texture filtering parity (2026-10-10)

The 8b2d705 baseline was reproduced with the existing six Infantry coastal
camera presets, copied profile and 800x600 (`build/shadow-live/filter-before-water`
and `filter-gl33-water`). An anisotropy-only A/B (`filter-aniso-water`) removes
the conspicuous horizontal distant-water smoothing and improves oblique terrain
detail toward GL33 without reviving the white shoreline artifact.

Vulkan now queries/enables samplerAnisotropy when supported and applies
min(16, maxSamplerAnisotropy) to the four existing linear samplers. Magnification
remains linear and minification trilinear, with zero LOD bias and unchanged
clamp/repeat combinations. Unsupported devices retain trilinear filtering.
The four point samplers remain nearest/nearest-mip with anisotropy disabled.
Secondary texture descriptors already use sampler 0 (linear/repeat), independently
of the primary sampler, matching GL33. No sampler manager/settings or new
texture versions were added. The RTX 4060 Ti reports 16x; the six-view run closed
normally with 3,351 submissions and zero core/synchronization validation issues.
The Vulkan build passes and driver-free guards pass 261 checks, including all
eight sampler combinations at fallback/8x/16x and capability-limit handling.

### Stored mip range and AI88 precision

Temporary upload traces established that GL33's TextureSourcePac stops before
either mip dimension reaches 4, whereas Vulkan sampled the entire stored chain.
The Vulkan image view now exposes the same tail (all original mip bytes are
still decoded/uploaded). Examples: detail_dx 128x128 exposes 5 of 7 levels;
specular_dx 256x256 6 of 8; silnice 256x64 4 of 6; more_anim.03 512x512 7 of 8.
Single-level/tiny UI images retain their base. GL33's observed maximum size was
4096 with largestUsed=0; no top-size limit explains these scenes. UseMipmap is
a GL33 residency request, not an explicit shader LOD; Vulkan remains fully
resident. ObjMipmapCoef differs (1.5 versus 1), but has no call sites in this
tree, so it was not changed. No bias, generated mipmaps or streaming was added.

The matched low-angle HMMWV road exposed another concrete cause: anisotropy
alone left broad gray bands. Stock silnice, detail_dx and specular_dx are AI88.
GL33 uploads their native 8-bit intensity/alpha as RG8; the shared CPU decoder
used by Vulkan converted them through ARGB4444, discarding half the bits.
Both memory-chain and file-mip decoding now expand native AI88 directly to
RGBA8. The new synthetic all-256-values/multiple-mip regression failed before
the fix and passes afterward, including alpha precision. Stock alpha classes
remain Blend; classification policy is unchanged. `filter-ai88-road/road-noon.png`
now retains the fine road grain seen in `filter-gl33-road`, unlike
`filter-before-road` and the anisotropy-only comparison. `filter-ai88-water`
also preserves coastal detail without the former white patches.

ShapeSecondaryMode now uses IsMultitexturing(), matching GL33's detail shader
selection and avoiding unused secondary uploads when disabled. Focused tests
cover enabled/disabled detail/specular behavior, explicit IsWater selection,
mip-view bounds and full AI88 bytes. Both builds pass: 34 cases / 301 assertions
Vulkan ON, 33 / 296 OFF; eight stock textures pass 265 assertions ON and the
driver-free Vulkan guards pass 266 checks. Temporary texture/road traces were
removed. No GL33 drawing code, stock assets, shaders or batching changed.

### Final filtering verification and performance

Matched 800x600 camera/date/weather/visibility comparisons used the copied GOG
profile: six Infantry coast views (dawn shoreline/bay, noon bay/second coast,
near water, fog at 300 m) and four HMMWV road/town views (noon, dusk, fog).
`build/shadow-live/filter-{before,aniso,ai88}-water` and the corresponding road
runs retain the before/intermediate/after captures; `filter-gl33-{water,road}`
are the default-GL33 references. Camera geometry/settings match, not mission
NPC/subtitle timing. Oblique water/terrain detail is substantially closer to
GL33; AI88 road bands are gone. Buildings, shoreline transitions and distant
water remain intact. No new white patches or obvious filtering seams were
seen. Live movement did not reveal excessive new shimmering; this is bounded
inspection, not a claim of pixel-identical rendering.

`filter-multi-off-{vk,gl33}` additionally uses a copied profile with
multitexturing=0. Both lose the secondary ground detail as expected while
retaining ordinary water, confirming the engine setting reaches shader choice.
The usual multitexturing=1 profile was not changed.

RTX 4060 Ti, 800x600, existing VSync-on/FPS-cap-240 profile, CWR_VK_PROFILE:

| Resident scene | Core + sync validation | 8b2d705 ms / FPS | Final ms / FPS |
| --- | --- | --- | --- |
| HMMWV oblique road/buildings/foliage | On | 7.952 / 125.75 | 8.010 / 124.84 |
| HMMWV oblique road/buildings/foliage | Off | 6.316 / 158.33 | 6.323 / 158.15 |
| Infantry noon coastal water | On | 6.315 / 158.35 | 6.312 / 158.43 |
| Infantry noon coastal water | Off | 6.317 / 158.30 | 6.335 / 157.85 |

These are means of the first three complete two-second intervals with zero
allocations and texture uploads after the matched camera setup, not whole-run
averages. Road geometry is matched at 615 lit draws / 1,922 shadow triangles;
HUD/script submissions vary. Evidence: `filter-perf-road-{before,after}-{on,off}3`
and `filter-perf-water-{before,after}-{on,off}2`. Later road intervals contain
large stalls and changing scene workloads: final-five interval ranges across
the four road runs were 79.90–158.02 FPS (7.938–12.515 ms validation-on,
6.328–11.630 ms off). These are retained in the logs, not attributed to the
filter change, and not evidence of a speedup. An earlier overlapping diagnostic
attempt (`filter-perf-road-*-on`, no numeric suffix) is excluded.

Road record_ms was 7.665 -> 7.746 on / 6.070 -> 6.078 off; geometry uploads
0.191 -> 0.191 ms on / 0.181 -> 0.190 off. Water uploads stayed 0.007 ms.
All reported resident intervals have zero recurring allocations or texture
uploads. Off-mode present waits are about 4.1 ms road / 5.4 ms water: the
normal runs are display-paced, so these results do not measure uncapped GPU
headroom. No disproportionate cost was demonstrated that warrants reducing
GL33-equivalent 16x anisotropy. No performance optimization was added.

Fresh normal gameplay checks (`filter-game-*`, not mission completion):

- Infantry: about 8 m movement, camera rotation, aim/fire (M16 30 -> 29), map
  open/zoom/close, HUD, third-person soldier/projected shadow, coast water
  captures ten seconds apart, pause/resume, 960x640 resize/minimize/restore.
  Normal close: 10,967 submissions, zero validation errors/warnings.
- Take the Car: about 5 m movement, rotation, aim/fire (30 -> 29), close foliage
  cutouts, translucent radio/tutorial HUD, map open/close, pause/resume and
  900x650 resize/minimize/restore. Normal close: 5,918 submissions, zero issues.
- HMMWV: cockpit/windshield, third-person driving (about 8 m), road/building
  textures, dust, animated flag, vehicle/object shadows, HUD/map, pause/resume
  and 900x650 resize/minimize/restore. Normal close: 6,272 submissions, zero issues.

Khronos core/synchronization validation was enabled through those shutdowns.
Vulkan menu/intro timed shutdown also passed (1,442 submissions, zero issues).
Vulkan-enabled and GL33-only builds and the focused tests above pass; the final
stock case passes 265 assertions ON / 173 OFF. The 266 driver-free guards include
an intentional mocked teardown error, not a live validation failure. Default
GL33 in both executables rendered the menu/intro and reached timed exit 0;
Vulkan remains opt-in. The known stale EOS overlay-manifest loader message is
an unrelated environment warning, not a Khronos validation finding.
All 7,606 protected game/resource files retain their initial count, length and
modification time. Stock missions, textures and localization were not edited.

Remaining differences: sky/cloud appearance and low-sun fence/foliage rendering
are visibly different from GL33 and predate these changes; Vulkan remains fully
resident rather than implementing GL33's texture-size/residency policy. Explicit
IsWater/full material-specular support remains limited as documented earlier.
Highest-value next visual-parity milestone: diagnose the low-sun object-lighting
difference on the matched HMMWV fence, using GL33 as reference, without further
global texture sharpening or speculative fog changes.

## Visual parity: fence winding (2026-10-10)

The matched HMMWV dusk fence was not a specular or alpha-threshold defect.
The stock `data3d\plutek.p3d` uses normal material 0 (specular RGB/power zero),
with `planky3.paa`/`planky3.pac` blended/cutout sections. The close fence is
software-transformed; suppressing only that diagnostic draw removed the
affected pixels. GL33 retains back-face culling in its screen/software pass
specifically to reject coplanar reverse faces. Vulkan had culling disabled in
both paths, allowing the sunlit reverse side to overwrite the shaded front.

Native and screen pipelines now use clockwise front faces and back-face culling,
matching GL33 after the existing Vulkan Y conversion. Projected-shadow pipeline
rasterization is unchanged. Native-only culling did not fix the fence and was
not retained as a partial solution. No colors, normals, alpha thresholds, mip
levels, fog or stock geometry were changed to conceal the artifact.

Evidence under `build/shadow-live/`: `visual-before-fence` versus
`visual-cull-screen` reproduces/removes the orange dusk face; eight matched
`visual-fence-final` / `visual-fence-gl33` views cover noon, 18:00 low sun,
19:00 dusk, 23:00 night, reduced visibility, closer/oblique and reverse views.
The shaded face now matches GL33; the opposite side remains correctly sunlit.
Roads, buildings, sky, stars and sun halo remain visible. The Vulkan run closed
normally with 3,219 submitted/presented frames and zero core/synchronization
validation errors or warnings. The build passes and 269 driver-free guards
include new winding/cull/shadow-policy checks. Temporary tracing was removed.
Foliage silhouette differences remain for separate investigation; these captures
use GL33's actual 0x-MSAA target, so they must not be attributed to MSAA coverage.

### Native material specular

The native lighting block now carries sun-diffuse times material-specular RGB
and the original material exponent. The vertex shader computes GL33's normalized
world-normal/view/sun half-vector response, gated by positive exponent and the
existing sun-enable/DisableSun state. The normal/detail fragment path adds this
separate term after diffuse texture/detail modulation and before night-eye,
fog and gamma. Zero-specular materials, opacity, inverse-transpose normals,
local lights, shadow shaders and water's corrected detail-alpha sampling remain
unchanged. Software/UI vertices supply zero specular, as before; their CPU
lighting currently supplies black specular RGB. No new material system is added.

`visual-material-{vk,gl33}` compares the stock HMMWV, nearby buildings and glass
at noon, low sun, a second angle, night and fog. HMMWV body materials in this
run have no specular term and correctly do not acquire a new gloss. Narrow
diagnostics confirmed nonzero stock native terrain (power 3) and animated-water
(power 6) material terms. `visual-water-{before,spec,gl33}` covers six Infantry
coast views: dawn shoreline/bay, noon bay/second coast, near water and fog.
No white shoreline patches returned. Highlights are view-dependent and subtle
in these views; this is not a claim of a large vehicle brightness improvement.
Vehicle and water Vulkan runs closed normally with 2,180 / 3,386 frames and zero
core/synchronization validation findings. The Vulkan build and 35 focused
cases / 310 assertions pass, including specular color/power, disabled sun,
black sun/material and std140 layout regression checks. Traces were removed.

Further foliage diagnosis: the same `str_jablon.p3d` object at
`[3061.324,230.263,6334.997]` uses LOD 0 / 55 vertices in both renderers.
GL33 loads `jablon_renovace.pac` from mip 2 (64x64); Vulkan retains its 256x256
base. The different stored-mip coverage contributes to the fuller GL33 crown.
Evidence: `visual-lod-{vk,gl33}` and `visual-mip-gl33`. This is not MSAA coverage,
nor evidence for changing the global 192/255 cutoff. GL33's demand-residency
policy remains outside this targeted lighting pass; anisotropy, AI88 precision
and the previously corrected mip-tail bounds are untouched.

### Sky texture interpolation and atmosphere

Matched Infantry coast captures traced a further real mismatch to texture
interpolation, not ClipLightSky/ClipFogSky or sunlight. GL33 uploads the stock
sky interpolation in ARGB1555 using PacLevelMem's integer 255-weight RGB555
arithmetic. Vulkan instead mixed expanded RGBA8 texels in floating point.
This changed the texture's color steps/gradient through weather transitions.
Vulkan now preserves source-format metadata and matches that packed arithmetic
for the corresponding P8/1555/compressed source families. Other formats retain
their existing path. A focused test compares the helper directly against
PacLevelMem::Interpolate over representative channel values and weather factors.

Importantly, GL33's CPU GetPixel sky/fog calculation interpolates the
source colors separately from its quantized GPU upload. Vulkan now retains
those source references/factor too, so matching the visible texture does not
quantize the scene's fog color. Average-color metadata also remains unquantized;
this does not implement GL33's source-header average-color policy.
Existing image-version/frame-fence ownership,
the 1/64 update tolerance, endpoint selection and single-level sky policy are
unchanged. Stock files, sky geometry and weather animation are untouched.

`visual-sky-{spec,packed,gl33}` compares before/after/reference at clear noon,
partial-overcast sunset, full-overcast noon, foggy overcast dawn, clear night
and a moon-facing night view. The fixed screen crop x=5..794, y=105..319 has
mean absolute RGB error 1.984 -> 0.408 /255 at clear noon, 2.310 -> 1.965 at
sunset and 0.718 -> 0.552 at night. These are screenshot observations, not a
universal parity score; cloud movement and mission timing are not bit-identical.
Full-overcast error barely changes (5.005 -> 4.974), as expected when the
endpoint texture is used. Sky/horizon transitions, stars, moon, sun halo and
fog remain present. The final Vulkan weather sweep closed normally with 3,507
frames and zero core/synchronization validation errors/warnings. Both builds
pass: 36 cases / 459 assertions ON, 35 / 454 OFF. No tracing remains.

Cloud geometry/layers, CPU cloud color and alpha-fog handling already follow
the engine; no new cloud system or global fog correction was justified.
Atmospheric parity is partial: GL33 applies gamma after framebuffer composition,
whereas Vulkan applies it per fragment before alpha blending. That remains a
specific compositing difference for translucent clouds/effects at non-unit gamma,
not evidence for altering original weather brightness or alpha thresholds.

### Final visual-pass verification

The final executable (SHA256 `A8249CC6ACA358A515956D5AA62B1E4E4374E8B2631D92AD11189E5881066A6E`)
was tested against the saved baseline executable
(`95555F63812F55F9DED61C60C2BBD3FF890BF534E8241F41011221A8E8E34B12`),
using the isolated GOG 3.05 data/profile, RTX 4060 Ti and 800x600 window.
`visual-perf-{road,water}-{before,final}-{on,off}` contains the existing matched
camera sampler's logs and screenshots. Each number below is the mean of the
first three complete two-second intervals with zero allocations/texture uploads;
loading intervals are excluded. VSync/profile settings are unchanged.

| Scene / validation | FPS before -> final | Frame ms | Recording ms | Geometry upload ms |
| --- | --- | --- | --- | --- |
| HMMWV oblique road / on | 131.20 -> 138.52 | 7.623 -> 7.219 | 7.387 -> 6.971 | 0.187 -> 0.142 |
| HMMWV oblique road / off | 158.46 -> 158.54 | 6.311 -> 6.308 | 6.079 -> 6.077 | 0.177 -> 0.140 |
| Infantry coast / on | 158.55 -> 158.71 | 6.307 -> 6.301 | 6.055 -> 6.058 | 0.007 -> 0.006 |
| Infantry coast / off | 158.43 -> 158.60 | 6.312 -> 6.305 | 6.028 -> 6.027 | 0.006 -> 0.006 |

These support no observed steady-rendering regression, not a claimed speedup.
The road's mission/shadow workload varies between launches (615 lit draws but
91 versus zero software-shadow draws in these intervals). The baseline repeat
`visual-perf-*-repeat-before-*` also exhibits that variation. Later intervals in
both binaries sometimes reach roughly 9-12 ms while recording remains around
6 ms and p95 near 6.2 ms; those application/frame-pacing stalls remain in the
logs and are not assigned to this patch. Validation-off steady samples are
presentation-limited, so they do not measure uncapped GPU headroom. Settled
transient allocations remain zero. Batching, state caching and draw order were
not redesigned or optimized in this pass.

Actual final Vulkan gameplay evidence (`build/shadow-live/visual-game-*`):

- Infantry: movement, camera turn, aim/fire/reload, two grenade throws and visible
  blast/smoke, soldier shadow, map opening/zoom/pan/closing, pause/resume,
  960x640 resize/minimize/restore and Mission Abort. Abort exits this test-mission
  launch normally; 16,563 submitted frames, zero validation findings.
- Take the Car: movement, aiming/firing/reload, buildings and vegetation, HUD/map,
  pause/resume, 900x650 resize/minimize/restore, normal close; 4,262 frames, zero.
- HMMWV: cockpit/windshield and third-person views, about 22 m of driving,
  braking, dust, fence/vehicle shadows, HUD/map, pause/resume and resize/restore;
  normal close, 5,144 frames, zero.
- Heavy Metal (`visual-game-heavy2`): on-foot movement and stock M1 tank views;
  the existing harness placed the player in the driver's seat, followed by
  keyboard driving/braking, vehicle shadows, map, pause/resume and resize/restore.
  Normal close: 9,352 frames, zero. An earlier unsupported `assignedVehicle`
  diagnostic caused a script-error exit and is excluded, not a renderer failure.
- Shadow Killer: stars/moon and dark foliage, night-vision toggle, movement,
  aiming/firing, HUD/map, pause/resume and resize/restore. Normal close:
  14,205 frames, zero. Default GL33's matching starting view and night vision
  are captured in `visual-game-shadow-gl33-repeat` (normal exit 0).

Core/synchronization validation was enabled through each accepted Vulkan
shutdown. Grenade blast smoke and vehicle dust were observed; a runtime
SmokeShell selection attempt did not produce a dedicated smoke-grenade throw,
so that particular effect is not claimed as tested. No mission files were edited.
Additional fixed noon cameras `visual-matched-{takecar,heavy}-{vk,gl33}` compare
the same buildings, foliage, M1 tank, shadows and sky with identical time,
weather, visibility and settings. These complement the HMMWV/Infantry sweeps
above; ordinary initial mission positions alone were not treated as matched.

Both builds pass. Final focused runs pass 459 assertions / 36 cases ON and
454 / 35 OFF; stock mip checks pass 265 / 173 assertions, and the driver-free
suite passes 269 guards (including its intentional mocked teardown error).
Final menu/intro captures `visual-final-menu-{vk,gl33,gl33-only}` render normally
and reach timed exit 0; Vulkan records 1,509 frames with zero validation findings.
Both executables still select GL33 when no renderer option is supplied.
The 7,606 protected stock/resource files retain their original count, lengths
and modification times; localization and assets are untouched. No white
shoreline patches or AI88 road banding returned in the inspected scenes.

Completed fixes are reverse-face rejection, native material specular and packed
sky interpolation. Foliage residency/coverage and atmospheric compositing are
still partial, not solved milestones. Highest-value next visual-parity work:
match GL33's framebuffer-level gamma/composition ordering for translucent
clouds/effects, with matched non-unit-gamma captures before extending scope.

### DXT1 interpolation rounding follow-up

A synthetic DXT1 case exposed a small remaining mismatch in `8f60a83`:
endpoints `0x1000/0x0000`, selector 2, blended halfway toward black produced
red 0 instead of GL33's 8 (the older floating-point result was 6). DXT1-to-RGBA8
decoding followed by five-bit truncation cannot reproduce the legacy decoder's
rounding before interpolation. The existing packed-color test missed that step.

Only DXT1 sources used in packed interpolation now lazily decode their top level
with `PacLevelMem::DecompressDXT1`, the same routine GL33 uses. The result is
cached separately; ordinary pixels, stored mips, alpha classification and CPU
sky/fog lookup retain their existing decode. Other formats and mixed-format
floating-point interpolation are unchanged. No shaders or GPU ownership changed.

The actual TextBankVK regression failed with 0 before the fix and passes with 8
afterward; ordinary red stays 11 and the CPU halfway lookup stays 5.5/255.
Additional checks cover all DXT1 selectors, transparent/equal-endpoint blocks,
partial blocks and truncated payloads. Both builds pass; focused checks pass
826 assertions ON / 814 OFF, stock mip checks 265 / 173. No new gameplay visual
improvement or performance result is claimed; the previous live validation
results were not rerun for this narrowly scoped CPU conversion fix.

## Framebuffer gamma: reproduction (2026-10-10)

This work starts from `45a6033` on main, retaining the repository-owner and DXT1
rounding follow-ups to requested baseline `209657c`. There was no unstaged
documentation edit at the start. The saved pre-change executable is
`build/vulkan-local/apps/cwr/Game/PoseidonGame-gamma-before.exe`, SHA256
`C045FADFEF8232D03806D783D9701A61566EC53D1FC667C44DC7DA02D752EFAA`.

GL33's ApplyGammaPass copies the completed default framebuffer to RGBA8 and
draws a nearest-sampled fullscreen triangle with pow(rgb, 1/gamma). It runs
after scene/HUD composition, with blending/depth/culling disabled. Vulkan's
shape.frag instead applied pow before fixed-function alpha/additive blending,
including software geometry and ordered screen batches. Projected shadows
darkened an already-corrected destination. For example, a 50% white fragment
over black at gamma 2 produced 0.5 instead of sqrt(0.5), approximately 0.707.
This is an ordering defect, not evidence for changing cloud colors or opacity.

The existing Infantry camera/weather sweep was repeated at gamma 0.6, 1.0,
and 1.6, with the same 800x600 copied profile, brightness 1.6, fixed cameras,
date/weather/visibility and frozen simulation. Evidence is in
`build/shadow-live/gamma-before-{vk,gl33}-{06,10,16}`: clear noon, partial sunset,
heavy overcast, foggy overcast dawn, night and moon-facing views. Baseline Vulkan
normal closes reported 3351/2947/3475 frames and zero core/synchronization
validation errors or warnings. The GL33 references also closed normally.

The fixed sky crop x=5..794, y=105..319 has Vulkan/GL33 mean absolute RGB error
in 8-bit levels of 1.953/4.670/2.393 at sunset for gamma 1/0.6/1.6, respectively;
heavy overcast is 5.721/8.459/4.288. These are observations, not a deterministic
parity score: cloud positions and subtitle/mission timing differ between runs.
Non-unit gamma amplifies some differences and masks others. Gamma-1 residuals
are explicitly retained as a separate comparison, not assigned to gamma.

An actual SmokeShell throw was also observed in `gamma-smoke-before-vk-16`:
the initial full Infantry inventory rejected the added magazine. Removing
HandGrenade magazines in the disposable runtime session allowed SmokeShell
selection; a mouse throw consumed it and produced a visible white smoke plume.
No stock mission or asset was edited. The run closed normally with 10512 frames,
zero validation findings. Further matched smoke/HUD comparisons and corrected
renderer results follow below; selection/eval success alone is not smoke coverage.

### Framebuffer gamma implementation

VulkanContext now owns one sampled scene-color image per swapchain image, with
the same extent/format and the existing depth/stencil attachment. The original
render pass still contains every native Shape, software TL draw, sky/water,
cloud/particle, projected shadow and ordered screen/HUD batch. EndFrame flushes
the last batch, ends that pass, then draws one fullscreen triangle into the
acquired swapchain image. Its shader applies pow(max(rgb, 0), 1/gamma) once,
after all ordinary alpha/additive blending and stencil shadow exclusion.
The Shape fragment shader no longer performs gamma; its old push-constant slot
is padding, so gamma changes no longer split batches or mutate scene constants.
The latest gamma setting applies to the whole finished frame, as in GL33.

Color-write to shader-read and prior-sample to next-color-write dependencies
cover target reuse. Final rendering retains the acquire wait, per-image present
semaphore and original frame-fence submission. The independent gamma layout
invalidates the command-local scene cache. No scene ordering, blend factors,
alpha thresholds, fog, material lighting, texture descriptors or asset lifetime
changed. All added images, views, framebuffers, descriptor sets/pool, pipeline
and layout are reused; idle-based swapchain recreation/shutdown owns cleanup,
including partial initialization. No recurring frame allocations are added.

The RTX surface remains B8G8R8A8_UNORM, so there is no automatic sRGB conversion
in either pass. The sampled image uses the exact swapchain format rather than
introducing an sRGB target. An sRGB-only surface retains its original blend
space, with hardware decode on sampling and encode on final output; that
fallback was not live-tested on this UNORM surface. Format/usage support is
queried before creating each color image. Shaders target SPIR-V/Vulkan 1.0.
Nearest/clamp sampling reuses the existing point sampler. Gamma 1 (including
GL33's 0.999..1.001 tolerance) skips pow but retains the simple fullscreen pass;
performance measurements below decide whether a second rendering path is needed.

Both builds pass. Focused Shape/decoder checks pass 820 assertions / 37 cases ON,
808 / 35 OFF; eight stock texture mip checks pass 265 / 173 assertions. The
driver-free suite passes 286 checks, including latest-gamma constants, identity
tolerance, acquired-image/full-extent selection, one final triangle/pass and
scene-cache invalidation. Its intentional mocked teardown error is not a live
validation failure.

`gamma-after-vk-{10,06,16}` repeats all six matched sky views and additionally
resizes to 960x640, minimizes/restores and closes normally. Respectively
3780/3509/3854 submitted frames, zero Khronos core/synchronization warnings or
errors through destruction. `gamma-live-update` changes the real Graphics UI
slider 1.0 -> 1.6 -> 0.6 -> 1.0, including resize/restore at 0.6; the setting and
whole-frame brightness update visibly without stale/black frames. Normal close:
9095 frames, zero validation findings. GL33 drawing code remains untouched and
Vulkan remains opt-in. Broader effect/gameplay/performance verification follows.

### Cloud and translucent-effect comparison

The same six sky views were compared before/after against GL33. Selected sky-crop
mean absolute RGB errors (8-bit levels; smaller is closer) are:

| View | Gamma | Before | After |
| --- | ---: | ---: | ---: |
| Partial sunset | 1.0 | 1.953 | 1.807 |
| Partial sunset | 0.6 | 4.670 | 1.460 |
| Partial sunset | 1.6 | 2.393 | 1.542 |
| Heavy overcast | 1.0 | 5.721 | 5.273 |
| Heavy overcast | 0.6 | 8.459 | 5.065 |
| Heavy overcast | 1.6 | 4.288 | 4.111 |
| Foggy dawn | 0.6 | 1.461 | 0.995 |
| Moon-facing night | 1.6 | 1.140 | 0.454 |

All six crops improve in these runs at all three settings, but this is not a
deterministic image test. In particular, the small gamma-1 movement is capture
variation, not a claim that identity gamma fixes clouds. The identity shader
copies the same normalized scene color without a power operation. No visible
gamma-1 transparency, horizon, shoreline or HUD regression was found.

Shared Landscape::DrawClouds builds the same layered software geometry and
TLVertexTable::DoCloudLighting supplies cloud brightness, alpha and alpha-fog
to both backends. Weather::MoveClouds advances cloud position by speed * deltaT;
freezing after different loading intervals does not make positions identical.
This explains shifted cloud features, but does not establish the entire cause
of the remaining heavy-overcast RGB difference. That residual is not assigned
to gamma, nor concealed with a cloud-color/opacity adjustment. Existing texture
detail/LOD differences elsewhere also remain outside this fix.

`gamma-effects2-{before,after,gl33}-{10,06,16}/smoke-plume.png` captures actual
SmokeShell plumes at the same coastal camera and weather. Runtime inventory
queries verified that a mouse throw consumed SmokeShell; merely selecting or
issuing a script fire command was not counted. The corrected smoke blends with
the already-composed shoreline/sea before gamma, with no opaque replacement,
missing layers or new edge halo. Particle positions/ages differ, so no numerical
smoke-parity score is claimed. Rectangular billboard edges are also visible in
GL33 and are not evidence for changing Vulkan alpha thresholds. Initial
`gamma-effects-*` selection-only captures are excluded. The 0.6 corrected run
completed via its normal 100-second timeout after a later unsuccessful frag
grenade attempt; its smoke capture is valid, its frag attempt is not coverage.
Every corrected smoke run reports zero validation findings through shutdown.

`gamma-material-{vk,gl33}-{10,16}` repeats the HMMWV noon, low-sun, side, night
and fog views. Glass retains the reference transparency, body lighting/specular
and projected shadows remain visible, and non-unit gamma affects the completed
vehicle/background composition. Vulkan normal closes: 2129/2098 frames, zero
core/synchronization warnings or errors. Smoke HUD/pause captures and the live
Graphics gamma-slider test retain readable fonts and translucent overlays.
No additional rendering change was justified by these matched comparisons;
cloud/weather simulation, alpha thresholds, blend factors and stock assets
remain untouched.

### Final gameplay and regression verification

The isolated GOG 3.05 installation, RTX 4060 Ti, copied profiles and exact-window
bounded input helpers were reused. No game data, mission, model or texture was
edited. Before/after inventories of 7596 stock-data files and 10 repository
resource files match in path, size and modification time. Audio was disabled for
these runs; audio parity is not claimed. The system-awake work lease did not
keep the display awake or change the power plan.

All five requested missions were exercised at gamma 1.6 with Khronos core and
synchronization validation:

| Evidence under build/shadow-live | Observed coverage | Submitted frames |
| --- | --- | ---: |
| gamma-play-infantry2 | Movement, views, HUD/map/zoom, pause/resume, resize/restore, real HandGrenade smoke and ground dust, Mission Abort | 4120 |
| gamma-play-takecar2 | Movement, rifle fire/reload, HUD/map/zoom, pause/resume, resize/restore, normal close | 2092 |
| gamma-play-hmmwv | Driving, exterior/interior, shadows, HUD/map/zoom, pause/resume, resize/restore, normal close | 2750 |
| gamma-play-heavy | On-foot controls, existing M1 driver/interior/exterior, driving/exhaust/shadows, map, pause/resume, resize/restore, normal close | 3164 |
| gamma-play-shadow | Movement/mouse look, firing/reload, night vision, HUD/map/zoom, pause/resume, resize/restore, Mission Abort | 2632 |

Each completed with exit code 0 and zero validation warnings/errors through
resource destruction. An abort/close can leave the final submitted frame
unpresented; that is not a fence or validation failure. These are bounded
gameplay checks, not complete mission playthroughs.

Evidence was checked rather than inferred from input receipts: the initial
keyboard fire attempt left ammunition unchanged and is not counted. Actual
mouse fire in Take the Car changed M16 30 -> 29, then reload restored 30.
Infantry's stock HandGrenade count changed 6 -> 5 and `grenade-effect-0.png`
shows the dark explosion plume and pale ground dust. This is separate from
the sustained SmokeShell coverage above. HMMWV's driving capture is partly
occluded by foliage; Heavy Metal provides an unobstructed tank/exhaust view.
One unprotected Take the Car run ended after the player was killed; it also
shut down cleanly. The repeat and later scripted scenarios disabled player
damage only in disposable runtime state, and Heavy Metal used the existing
test harness to enter an existing M1. AI, weather simulation and stock missions
were not rewritten.

`gamma-fence-{vk,gl33}-16` repeats the road, town, sun/night/fog and opposite
fence views. The reverse fence remains correctly visible with reference-like
cutouts, and the low-sun material/shadow views retain their previous behavior.
Vulkan closed after 3466 frames with zero findings. Residual foliage detail is
still visible and is not attributed to the final gamma pass. The sky/coast,
vehicle and map captures plus the existing focused tests cover packed sky
interpolation/DXT1 rounding, the shoreline correction, AI88 precision, native
material specular, anisotropic/mip policy, projected shadows, fonts and ordered
screen batching/state-cache behavior without changing those implementations.

`gamma-weather-live` resumes simulation and requests a ten-second overcast/fog
transition at the coastal camera. Successive captures show the horizon becoming
fogged while scene/cloud rendering continues; this is not a claim that the
legacy cloud simulation finishes every layer transition in ten seconds.
Normal close: 2334 frames, zero findings. The separately frozen heavy-overcast
and dawn views above cover those completed visual states.

Timed Vulkan menu, indexed-triangle and engine-Shape diagnostics
(`gamma-{menu,triangle,shape}-vk`) exit 0 with 1472/1627/1687 frames and zero
validation findings. Both `gamma-default-gl33-{on,off}` builds launch GL33
without `--render`, render their menu and exit 0 on timeout. Vulkan remains
opt-in. Both build targets and the focused suites pass as listed above; the
final suite rerun again passes 286 driver-free checks, 820/808 Shape/decoder
assertions and 265/173 stock-mip assertions. No GL33 source was changed.

### Gamma performance and remaining limits

`gamma-perf-{road,water}-{10,16}-{on,off}-{before,after}` contains 16 sequential
RTX 4060 Ti runs at 800x600, with the same copied profile/VSync setting and
existing camera sampler. The saved pre-change binary is `45a6033`, not a fresh
checkout of `209657c`; its two retained follow-ups are repository documentation
and DXT1 source-decoding rounding, not a different steady draw/gamma pipeline.
The corrected binary SHA256 is
`4FED6E00BB66A074D096E294E5E8C4C21722618EDCE15AC2DAA3DB0991E8BD4E`.

As in the preceding baseline report, these are means of the first three complete
two-second intervals with zero transient allocations and zero texture uploads.
They exclude startup, not inconvenient frame times. All later intervals remain
in the logs and were also inspected (including the last eight-window summaries).
The road's mission/shadow/overlay work is not fully deterministic, so its
confounded pairs are explicitly identified rather than interpreted as isolated
gamma cost.

| Scene | Gamma | Validation | Frame ms before -> after | CPU command ms before -> after |
| --- | ---: | --- | --- | --- |
| HMMWV road | 1.0 | On | 7.711 -> 8.485 | 6.458 -> 7.143 |
| HMMWV road | 1.0 | Off | 6.303 -> 6.324 | 1.738 -> 1.781 |
| HMMWV road | 1.6 | On | 8.157 -> 7.034 | 6.837 -> 5.936 |
| HMMWV road | 1.6 | Off | 9.745 -> 6.326 | 1.594 -> 1.756 |
| Infantry coast | 1.0 | On | 6.301 -> 6.311 | 2.003 -> 2.019 |
| Infantry coast | 1.0 | Off | 6.316 -> 6.309 | 0.603 -> 0.613 |
| Infantry coast | 1.6 | On | 6.312 -> 6.327 | 2.147 -> 2.212 |
| Infantry coast | 1.6 | Off | 6.321 -> 6.308 | 0.599 -> 0.634 |

The first road gamma-1 validation-on pair has 9 versus 80 screen batches, despite
615 lit draws and 97/91 native/software shadows in both selected windows. At
gamma 1.6 with validation, software shadows are 91 versus zero. The gamma-1.6
validation-off baseline already contains application/frame-pacing gaps (its
p95 frame interval is 6.197 ms despite the 9.745 ms average frame period).
None of those three pairs establishes a gamma slowdown or speedup. Later
windows in both binaries also show roughly 9-14 ms average frame periods while
recording/p95 remain much lower, as in the preceding baseline work. These
pre-existing pacing/workload differences were not changed in this task.

The reverse-order gamma-1 road repeat (`gamma-perf-road-10-on-repeat-*`) runs
the corrected executable first. Its selected windows have the same 615 lit
draws and 97/91 native/software shadows, with 11 versus 10 screen batches:
frame time 8.076 -> 8.081 ms, command recording 6.795 -> 6.805 ms and p95
8.379 -> 8.402 ms (numbers are always before -> corrected). This much closer
workload comparison does not reproduce the apparent 0.774 ms initial increase.
The reverse-order gamma-1.6 validation-off road pair likewise measures
6.325 -> 6.319 ms (command recording 1.865 -> 1.838 ms), with 615 lit draws,
97/91 shadows and 6 versus 1 screen batches. All 20 performance runs, including
these four repeats, close normally with zero core/synchronization findings
where validation is enabled and zero settled transient allocations.

The coast keeps 189 lit draws, zero shadows and one screen batch in all selected
windows. Together with the validation-off gamma-1 road sample, it shows no
meaningful ordinary steady-frame regression in the tested scenes. The small
gamma-1 differences do not justify a direct-to-swapchain fast path: identity
skips pow, and the single simple composition path is retained. These are
presentation-limited measurements around 158 FPS, not uncapped GPU timing or
claims about higher resolutions. No per-frame scene-target allocation occurs.

Remaining limits: the full heavy-overcast gamma-1 residual is not yet explained;
shifted cloud/particle phases and existing texture/LOD detail differences are
not fixed by framebuffer gamma. The sRGB-only surface fallback remains untested
on this UNORM display. The loader still reports the pre-existing missing Epic
overlay JSON and notices for intentionally disabled implicit layers; these are
separate from the zero Khronos core/synchronization findings. No renderer code
or machine-wide layer configuration was changed to conceal them.

The highest-value next parity milestone is a narrowly matched gamma-1 cloud
layer investigation: record identical cloud phase, per-layer textures/mips and
software vertex colors/fog before proposing any additional rendering change.
Do not compensate for those differences with arbitrary opacity/color tuning or
a new weather/particle system.

## Cloud and foliage input parity (2026-10-10)

This investigation starts at `24f691f` with a clean working tree. The isolated
GOG 3.05 data, RTX 4060 Ti, 800x600 window and existing Infantry coastal camera
are reused. It establishes a capture correction, not a new cloud-rendering fix.

### Matched clouds: phase and camera target both matter

`Weather::MoveClouds` integrates `_cloudsSpeed * deltaT` into `_cloudsPos`.
Freezing after loading leaves different accumulated positions in each backend.
A small temporary diagnostic pins only DrawClouds' local `cPos` to 123.25;
it does not change weather simulation or stock data. It logs layer/model,
position, azimuth/scale, ordered submissions, texture/resident mip, spec flags,
transformed positions, UVs and packed CPU colors including alpha-fog.

That phase-only comparison (`build/shadow-live/cloud-phase-{vk,gl33}-10`)
reduces heavy-overcast sky-crop error from the previous 5.273 to 1.041 /255.
The remaining transformed Y/Z differences exposed another capture variable:
`CamSetTargetVec` uses `GetPos`, which adds `SurfaceYAboveWater`. This coastal
target is over water, so different simulation clocks produce different target
heights and camera pitches even with identical script coordinates. Identical
camera position/daytime alone is therefore insufficient evidence of a match.

The existing `triSetSimTime 60` command, after `setAccTime 0` and before camera
creation/target assignment, removes that variable. With both controls,
`cloud-locked-{vk,gl33}-{10,06,16}` has identical cloud trace sequences in all
five weather/time states at all three gamma settings. Heavy overcast includes
35 candidate Draw calls, 17 surviving software submissions and 68 lit vertices;
all corresponding positions, UVs and packed RGBA values compare exactly.
The three layers retain their original ordering, transforms and 0.1 scale.
The four `mrak_war_{1,3,4,5}.paa` textures all use resident mip 0 in both paths
(256x128 except `_3`, 256x256). This is not the foliage residency discrepancy.

`DoCloudLighting` supplies identical sunlight/accommodation, brightness and
alpha, with SkyFog8 already attenuating vertex alpha. Both backend submissions
use flags `0xac128`: ordinary source-alpha blending, no additional RGB fog,
1/255 alpha cutoff and the original disabled-depth semantics. GL33's ordered
alpha queue and Vulkan's immediate software submissions preserve the same
cloud order. No missing software attribute or cloud-specific shader is needed.

The same sky-only crop x=5..794, y=105..319 gives mean absolute RGB differences
in 8-bit levels (not full-frame scores):

| Synchronized view | Gamma 1.0 | Gamma 0.6 | Gamma 1.6 |
| --- | ---: | ---: | ---: |
| Clear noon | 0.097 | 0.123 | 0.073 |
| Partial overcast noon | 0.361 | 0.457 | 0.264 |
| Partial-overcast sunset | 0.408 | 0.385 | 0.351 |
| Heavy overcast noon | 0.216 | 0.281 | 0.155 |
| Foggy overcast dawn | 0.120 | 0.127 | 0.100 |

Heavy-overcast gamma-1 error thus falls about 96% relative to the previous
unsynchronized 5.273 result without changing the renderer. Visual inspection
agrees: cloud features, translucency, horizon fog and sunset halo align.
The small residual is not bit-exact parity, but no visible cloud defect has
been isolated from it. It does not justify color/opacity tuning or changes to
packed sky interpolation, alpha thresholds, gamma, fog or the weather system.
Mission subtitles/timing outside the crop remain different and are excluded.

Both diagnostic builds pass. The three synchronized Vulkan sweeps close
normally with 2791 / 2714 / 2975 submitted frames and zero Khronos core or
synchronization warnings/errors through shutdown. GL33 references also exit 0.
These probes are diagnostic-only, not a proposed production rendering change.

### Foliage: residency explains one view, not every remaining edge difference

The previous apple-tree texture attribution was incomplete. The traced
`str_jablon.p3d` at engine position (3061.3242, 230.26288, 6334.9966) uses
`kura_jablon_asi_ne.pac` and **`jablon.pac`** in LOD 0 (55 vertices), not
`jablon_renovace.pac`. The latter is resident elsewhere in the scene. Forcing
only that texture to full resolution in a diagnostic GL33 build did not change
the target crown. Section-level tracing was necessary to identify the texture.

GL33's `PrepareTexture` / `UseMipmap` requests depend on projected texture area
and distance. The target's `jablon.pac` initially loads 64x64, then 128x128 in
the town view; approaching to 12 m loads 256x256 and retains it on subsequent
views. `jablon_renovace.pac` was 64x64 with coarser requested mips. The observed
budget was only about 6.8 MB of 543 MB, with no allocation/copy-pressure boost;
the largest-mip setting and 4096 texture-size limit were not restricting these
256 textures. This is demand-driven residency, not evidence of exhausted VRAM.
Vulkan retains the authored 256x256 base plus stored mip chain.

In the gamma-1 town view, requesting mip 0 for the actual apple textures in GL33
reduces the target-crown crop's mean RGB difference from 9.892 to 0.816 /255
(x=210..489, y=165..399). The reference then shows the same finer leaf detail as
Vulkan. The 12 m comparison is already close (0.778 /255, crop 180,102..619,499)
because unmodified GL33 has loaded the full texture by then. No production
residency override is retained, and Vulkan is not deliberately downgraded.

The sweeps also cover 25 m, 65 m, reverse and side views, adjacent vegetation,
town buildings, fence fronts/backs, low sun, night and fog. The 25 m crown still
differs with full residency: 10.207 /255 in crop 280,170..519,409, using the
later `foliage-target-vk` trace. Its side view is close, 0.493 /255 in crop
270,160..549,439; the earlier Vulkan side capture was not comparably close.
Sampled target traces show LOD 0 and matching section textures, but are not a
per-capture-frame proof of identical geometry/state. Nearby trees also change
LOD within the frozen-camera runs. These residuals are **not explained by the
town-view residency result**, nor sufficiently repeatable to assign exclusively
to filtering. A strictly draw-matched geometry/LOD and filtered-cutout comparison
remains necessary. Do not call this a confirmed Vulkan decoder bug or change a
global cutoff to conceal it.

The existing opt-in stock-bank test now includes the four cloud textures and
`jablon`, `jablon_renovace`, `n_strom_13` and `krovi6`. It checks authored mip
dimensions and translucent-versus-cutout classification, and compares every
legacy foliage mip's binary alpha with the Vulkan decoder: all agree exactly.
The foliage sources are DXT1. GL33 uploads them compressed, so RGB from a
forced legacy RGB555 decode is not an appropriate reference for normal foliage
sampling. Both backends use trilinear/16x anisotropic sampling without LOD bias;
Vulkan's sampled-mip bound already matches GL33's stop before a dimension <=4.
These checks rule out missing stored mips or differing source alpha masks, not
every possible driver filtering or draw-state difference.

Evidence is in ignored `build/shadow-live/foliage-{trace-vk,trace-gl33,
full-gl33,full-family-gl33,target-vk}`. All 13-view runs exit normally; the two
Vulkan traces submit 5703 and 5265 frames with zero validation findings through
shutdown. Temporary source probes and the GL33 full-res override were removed
after archiving their patch under `build/cloud-evidence/temporary-probes.patch`.
No runtime renderer change is justified by this investigation so far.

Both probe-free builds pass. Focused rendering/decoder tests pass 820 assertions
in 37 cases with Vulkan enabled and 808 in 35 with Vulkan disabled; the expanded
stock test passes 684 / 504 assertions respectively. The driver-free Vulkan
policy suite passes 286 checks (including its intentional mock teardown error).

### Probe-free gameplay and final verification (2026-10-11)

The final binaries have no engine/shader differences from `24f691f`. Vulkan
remains opt-in; GL33, stock resources and localization are unchanged. The
retained implementation work is the focused extension of the existing stock
test, not a visual compensation or new capture/streaming framework.

On the RTX 4060 Ti with the isolated GOG 3.05 installation:

| Run under `build/shadow-live/` | Gamma | Submitted frames | Exit | Core/sync findings through shutdown |
| --- | ---: | ---: | ---: | --- |
| `foliage-final-infantry` | 1.0 | 4566 | 0 | 0 errors, 0 warnings |
| `foliage-final-hmmwv` | 1.6 | 2131 | 0 | 0 errors, 0 warnings |
| `foliage-final-shadow` | 0.6 | 2196 | 0 | 0 errors, 0 warnings |
| `foliage-final-shadow-bright` | 1.6 | 2041 | 0 | 0 errors, 0 warnings |
| `foliage-final-shape-smoke` (timed exit) | default | 1677 | 0 | 0 errors, 0 warnings |

The gameplay runs exercise bounded movement/camera input, firing/reload inputs,
HUD/map and map zoom, pause/resume, 960x640 resize, minimize/restore and normal
window-close teardown. Infantry's stock HandGrenade count decreases after a
throw; this is not a claim of new SmokeShell or matched explosion coverage.
HMMWV's position changes from (3105.91,6335.33) to (3078.48,6339.36), with vehicle,
fence, bushes and building rendering inspected. Shadow Killer's gamma-0.6 view
is too dark for useful foliage judgment; the extra gamma-1.6 run visibly verifies
night vegetation, moon/stars and the green night-vision overlay. Map/briefing
fonts and transparent HUD overlays remain intact. Coast/cloud, low-sun fence
front/back, building and foliage screenshots from the matched sweeps supplement
these live checks. No new shoreline, shadow or sky-interpolation defect was
observed; exact filtering/coverage parity is not claimed.

Both Vulkan-enabled and GL33-only RelWithDebInfo builds pass. Each starts the
default GL33 menu without `--render` and exits 0 on its timer
(`foliage-final-default-gl33-{on,off}`). Focused suites listed above pass in both
configurations, including existing gamma, AI88, mip-bound, native-lighting and
render-state policy coverage. The pre-existing missing Epic overlay JSON loader
notice remains separate from Khronos findings; no system layer settings changed.

There is no changed runtime rendering behavior to performance-benchmark in
these commits: all phase/logging/residency experiments are removed. No new
frame-time delta or speedup is claimed; the previous gamma baseline remains the
relevant measured result. Gameplay profiling around input, map and resize events
is not presented as steady-state GPU performance. Source comparison against
`24f691f` confirms unchanged renderer, synchronization and command-state caching.

Before/after metadata inventories (path, size, UTC modification ticks) match for
all 7596 isolated stock files and 10 repository resource files. No unrelated
working-tree edit was present at task start. Only this document and the existing
stock test are retained changes; diagnostic captures remain ignored locally.

The remaining 25 m foliage comparison is a historical visual follow-up, not a
gameplay compatibility priority. Preserve Vulkan's full-resolution textures;
neither texture streaming nor global alpha/color tuning is warranted.

## Sustained gameplay and mission flow (2026-10-11)

Baseline `89714c8`, crash fix `4890bfa`, isolated GOG 3.05 assets, copied KyouKyou profiles, windowed
800x600/960x640. Normal menu selection and briefings were used for the coverage
below; direct mission startup was used only for crash replay. All Vulkan runs
enabled Khronos core and synchronization validation. Audio was disabled.

| Stock mission | Gameplay and lifecycle actually exercised |
| --- | --- |
| Steal the Car | Genuine objective attempt through town/buildings/vegetation, aiming/fire/reload, weapon switching and a confirmed frag throw (6 -> 5); enemy killed the player before reaching the car. Pause/save, movement away, load restoring position and resumed play; death/load and retry; abort to menu. |
| 1985 Training | Tutorial look/move stages, run to the repair truck and back (~140 m), action tutorial and first/third-person views. Development save succeeded; ending was then **script-triggered**, followed by stock cutscenes and the next briefing. |
| 1985 Flashpoint | Normal briefing, stock helicopter passenger flight/landing/unload, squad movement ~300 m through terrain/forest, ~4 minutes of simulation before load. Menu save/load restored the post-landing state and play resumed. **Script-triggered** ending reached Combined Arms briefing; campaign selection persisted across Vulkan and default-GL33 restarts. |
| Heavy Metal | Normal entry into M1A1 as driver, ~110 m driving into forest, interior/external views, moving shadows/exhaust, exit to foot, menu save/load/resume. Development-assisted gunner seat change; cannon (24 -> 23) and machine-gun fire. **Script-triggered** ending displayed debriefing and returned to menu. |
| Ground Attack (`04Helitrain`) | Walk to Cobra and enter as pilot, rotor startup, cockpit/external views, takeoff to ~70 m and ~1 km flight. Cannon, Hellfire and FFAR consumption confirmed; airborne menu save/load restored flight. Existing auto-hover action assisted a controlled landing (alive, zero damage, near-zero speed). Abort/menu; mission objectives not completed. |
| Shadow Killer | Normal night briefing, NVG, ~200 m approach, combat/reload, genuine enemy death and load/resume. Later protected combat against stock AI, with development-set dawn/overcast/fog/rain, exposed the tracer crash. Saved-state replay tested the fix, repeated save/load, map/HUD, resize/minimize/restore and shutdown. |

There were **zero genuine completions**. The Steal the Car attempt failed in
combat; tutorial stages and helicopter landing succeeded without rewriting
mission logic. `triEndMission 'end1'` checks are simulated progression, not wins
or proof that stock victory conditions work. No gameplay solver or mission
edits were introduced. Protection, seat placement, weather/time changes and
saved-state replay were limited to explicitly assisted coverage. A SmokeShell
throw was attempted but consumption was not verified; it is not counted.

### Combat crash and fix

Shadow Killer crashed twice in `Object::DrawLines -> EngineVK::DrawLine ->
SubmitScreen -> ScreenGeometry`. The first dump contains reciprocal-W values
`+0.220856249` and `-0.419688940` on the same tracer ribbon. The diagnostic replay
confirmed another positive/negative pair with projected, non-clipped vertices.
Vulkan's positive-only check threw on the behind-eye endpoint; this is valid
homogeneous geometry, not a missing texture or resource-lifetime failure.

`ScreenGeometry` now accepts finite **nonzero signed** reciprocal-W and retains
its sign for GPU clipping, matching GL33's existing `1 / aRhw` shader path.
Zero/non-finite inputs remain errors. No GL33, shared clipping, mission or
renderer architecture change is needed. The captured-coordinate regression
test fails on the baseline and passes with the fix, including the positive-W
near-plane intersection. Default GL33 replayed the pre-crash save under fire
and exited normally. Fixed Vulkan diagnostics processed two negative-W tracers
without throwing and shut down with 10,737 submitted/presented frames and zero
validation errors/warnings. Temporary instrumentation was removed.

### Verification and limits

Both Vulkan-enabled and GL33-only RelWithDebInfo builds pass. Focused shape,
PAA, factory, window metrics/placement suites pass: 74 cases/1,693 assertions
with Vulkan, 72/1,498 without; explicit stock-mip checks pass 684/504 assertions.
The driver-free Vulkan policy/lifetime suite passes all 286 checks (its deliberate
mock teardown error is expected). No broad performance campaign was run.

The initial multi-mission flow ran ~16 wall-clock minutes and exited normally
with 71,449 submitted/presented frames, zero validation findings; Flashpoint
private memory stayed ~2.630-2.634 GB with 802 handles in the sampled forest
segment. The broader vehicle/night session subsequently crashed as described
above, so it is **not** a clean shutdown-validation pass. Frame profiling showed
zero steady-state transient allocations between loading/movement events; these
bounded samples are not a claim of leak-free multi-hour play.

The final uninstrumented replay ran ~9 minutes across Shadow Killer, Heavy Metal,
Ground Attack and Steal the Car. It repeated combat/save/load, simulated
debrief/menu, tank driving/save/load/cannon fire, airborne load/cameras/missile
fire, a fresh normal-menu airborne save/load and zero-damage auto-hover-assisted
landing. One piloting attempt crashed the helicopter; it did not crash the
renderer. Temporary harness loading from that death screen left its UI open,
so replay was restarted from live gameplay; normal menu loading restored both
gameplay and UI correctly. Combat memory samples were 1.7877/1.7847 GB with 799
handles across another load (asset caches grew when changing missions).
Resize/minimize/restore and normal exit passed: 37,360 submitted / 37,359 presented,
zero validation errors/warnings through shutdown. The one unpresented submission
is consistent with the resize/out-of-date presentation path, not an outstanding
frame at shutdown. Both builds also started the default GL33 menu without a
renderer argument and timed out normally, exit 0 (`gameplay-final-default-gl33-{on,off}`).

One initial Steal the Car death showed a gray background and a shared-engine
"Ground drawing segment too big" warning, but death controls/load worked.
Subsequent Vulkan/GL33 death replays and the natural Shadow Killer death did not
reproduce it; a final Vulkan enemy-caused death at the original location also
rendered correctly. It remains an unisolated observation, not a verified fix.

Assessment: these checks support real single-player gameplay beyond rendering
smoke tests, but Vulkan should remain opt-in. Complete mission wins, an unassisted
campaign, multi-hour stability, fixed-wing aircraft and multiplayer are not
qualified by this run. No stock assets, missions or localization were changed;
all 7,596 stock files matched the starting path/size/UTC-mtime inventory.
Ignored evidence lives in `build/shadow-live/mission-flow-vk2`,
`mission-breadth-vk`, `tracer-{diag-vk,reference-gl33,fixed-diagnostic-vk}` and
`mission-final-vk` (screenshots, actions, profiles, logs and crash dumps).

## Optional SSAO: depth access milestone (2026-10-11)

The existing per-swapchain depth/stencil target is now sampled-capable and
stores depth after the scene pass. A separate depth-only view/point-clamp
descriptor leaves the depth/stencil attachment view and projected shadows
unchanged. The same D32S8/D24S8 preference is retained, requiring both attachment
and sampling support. Early/late depth writes become visible to fragment
sampling in depth/stencil-read-only layout at pass end. Views/descriptors follow
the existing idle-before-recreate/shutdown lifetime. No extra depth render pass
or visual effect is enabled by this milestone; gamma and UI ordering are unchanged.

Both builds and existing focused shape/PAA/factory/window tests pass; the Vulkan
policy suite passes 286 checks. Live Steal the Car and Heavy Metal checks cover
geometry, shadows, firing, map, 800x600 -> 960x640 resize and minimize/restore.
`ssao-depth-resize-vk` visibly resumed gameplay and exited 0 with zero core/sync
validation errors or warnings through shutdown. Evidence is ignored locally
under `build/shadow-live/ssao-depth*`.

### Basic SSAO pass

SSAO is opt-in with `CWR_VK_SSAO=1` before launch. The existing developer
console/harness can toggle/tune it with `triSSAO [1,1.2,1.5,0.03,80]`
(enabled, strength, radius metres, bias metres, fade distance metres); use
zero for enabled to disable. Invalid/nonfinite parameters are rejected without
changing state. GL33 returns false and its rendering code is unchanged.

One fullscreen draw reconstructs view positions/normals from depth, samples a
fixed eight-direction kernel, and multiplicatively blends restrained occlusion
into existing scene color. World geometry/shadows/transparency precede AO;
cockpit/weapon overlays, HUD/map/UI and the unchanged final gamma pass follow.
The world composite runs before the cockpit pass; interior depth clears also
guard against replacing its depth projection early. Menu/briefing/map/pause
rendering skips AO. A compatible LOAD pass
resumes color/depth/stencil afterward. No extra images, geometry pass, per-frame
allocations, blur, temporal history or material changes are introduced.

Initial Steal the Car off/on, movement/fire/reload, map/pause and resize/restore
checks passed (`ssao-basic2-vk`): normal exit 0, 4,224 submitted / 4,223 presented,
zero core/synchronization validation errors/warnings through shutdown. Both
builds pass; stock-backed focused tests pass 1,693 Vulkan / 1,498 GL33 assertions;
driver-free policy tests pass 295 checks, including AO toggle/parameter guards.
Broader visual tuning and performance qualification follow separately.

### Stock-scene tuning and gameplay checks

The opt-in defaults are strength **1.2**, radius **1.5 m**, bias **0.03 m**,
with fading from **48 to 80 m** and a 35% modulation ceiling. Paired 1920x1080
captures showed modest vehicle/ground, wall/fence and foliage contact shading,
without broad black outlines or replacing the original projected shadows.
The fixed sea-view off/on captures were identical. GL33 airfield, tank exterior
and nighttime references were also checked (qualitative, not pixel parity).

`ssao-quality2-vk` exercised normal single-mission menus/briefings and transitions
between these stock scenarios in one validation-enabled process:

- **Infantry / Ambush:** village/fence movement, aiming, firing/reload, pause,
  normal Save/Load restoring position and resumed movement.
- **HMMWV:** village/wooded terrain driving, interior glass and exterior cameras.
- **Heavy Metal:** M1 driver movement, gunner optics and cannon fire (24 -> 23).
- **Ground Attack:** airfield, Cobra cockpit/exterior, rotor, takeoff to ~20 m,
  cannon/missile fire (500 -> 499 / 8 -> 7), normal airborne Save/Load.
- **Shadow Killer:** loaded later combat, movement/fire/reload, NVG on/off,
  map, resize to 1280x720 and minimize/restore.

Developer assistance relocated Infantry to its village, selected vehicle seats,
protected some combat checks and restored a prior Shadow Killer combat save.
Fixed/frozen views were used only for image comparisons, not claimed as played
mission progression. This was not a mission-completion test. Normal shutdown
returned 0 with 62,340 submitted / 62,338 presented and zero Khronos core/sync
errors or warnings; the two unmatched presentations coincided with recreation.
Local raw captures/logs/profiles are under `build/shadow-live/ssao-quality2-vk`,
`ssao-reference-gl33` and `ssao-tank-gl33` (ignored, not stock-asset changes).

### Performance and final qualification

RTX 4060 Ti, driver 617.14, 1920x1080, warmed fixed views, VSync off, existing
240 FPS cap not reached, validation off. Medians of the existing two-second
profile intervals over 12-second off/on samples (`ssao-final-perf3-air-vk`,
`ssao-gpu-tank-vk`):

| Scene | Frame p95 off / on | AO GPU ms/pass | AO CPU recording ms/frame | New steady allocations/frame |
| --- | --- | --- | --- | --- |
| Ground Attack airfield | 6.227 / 6.216 ms | 0.263 | 0.012 | 0 |
| Heavy Metal exterior | 6.198 / 6.210 ms | 0.342 | 0.013 | 0 |

Whole-frame interval averages were noisy (final airfield 6.360 -> 6.366 ms, tank
13.429 -> 11.175 ms); repeated earlier pairs also changed sign. They do **not**
establish an end-to-end FPS penalty or speedup. The GPU interval and AO-specific
CPU recording cost are the useful isolated measurements; no optimization was
made on the basis of the noisy averages. There is no extra geometry/depth pass:
sampling reuses the original depth. Depth store/layout overhead was not isolated
from the full frame. Two timestamps per frame slot are allocated only when
`CWR_VK_PROFILE` is enabled and the graphics queue supports them, read after the
existing fence without a new wait, and destroyed with frame resources. The
`gpu_ms/pass=-1` diagnostic means no completed sample, not negative GPU time.

The final instrumented HMMWV check repeated driving, assisted exit/re-entry,
normal Save/Load, map, AO toggles and resize/minimize/restore: exit 0, 7,106
submitted / 7,105 presented, zero core/sync errors/warnings through shutdown.
SSAO with profiling disabled also passed live movement/fire/reload and shutdown
(3,519/3,519, zero messages). A further Steal the Car grenade throw (6 -> 5)
and developer-spawned stock smoke effect rendered with usable HUD and transparent
particles; its timed normal exit was clean (11,624/11,624, zero messages).
Final ordering review caught infantry optics bypassing the interior depth-clear
hook. Moving AO before the entire cockpit pass excludes these scopes and weapon
overlays too. Sniper Team M21 off/on optics, firing/map checks and Ground Attack
airborne cockpit/load checks passed with zero core/sync messages through normal
shutdown (7,693/7,693 and 4,904/4,904). Separate paired cockpit captures confirm
unchanged instruments. A later malformed harness ammo query ended one diagnostic
run in test-mode script-error shutdown; it is not counted as normal-exit evidence.
Both builds and default GL33 startup pass. Final focused suites pass 74/72 cases
(1,693/1,498 assertions), stock-mip checks 684/504 assertions and Vulkan policy
guards 295 checks. All 7,596 stock files retain their starting path/size/mtime.

Limitations: this is deliberately modest depth-only SSAO, not ambient lighting
replacement. Hidden/off-screen geometry cannot occlude; thin/cutout surfaces and
screen edges are approximate, with no temporal stabilization or blur. Transparent
world effects receive modulation based on the opaque depth behind them; they
have no separate AO mask. No serious transparency, water, shadow, UI or gamma
regression was observed in these checks, but arbitrary mods, unusual world-view
crops and multi-hour sessions are not qualified. Cockpit/weapon overlays are
excluded along with UI. Enablement is process-local/developer-console only,
not yet a persistent graphics-menu option. Keep the enhancement opt-in.
