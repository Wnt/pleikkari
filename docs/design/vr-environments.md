# VR environments around the stream (Oculus Go)

PLE-603. A study of how Skybox VR Player builds its cinema scenes on the Oculus Go, and
the original environments Pleikkari draws around the curved stream screen in the native
VrApi activity (PLE-602). Nothing from Skybox ships here: no mesh, texture, shader,
audio or code was copied. The numbers below were read from the APK's serialized Unity
data and decompiled scripts, and are written in our own words.

Source: `skybox-player-ovr-1.0.0-vc264.apk` (`xyz.skybox.player.ovr`, versionCode 264,
Unity 2018.3.6f1, Mono backend, armeabi-v7a), kept outside git under
`android-low-latency-examples/skybox-apk/` in the workspace. Tools: UnityPy 1.25 with
typetrees generated from the game assemblies, ilspycmd 9.1 on a user-local .NET 8
(per-type decompilation; the assembly is obfuscated and one method crashes whole-assembly
mode), and the exported GLES 3 shader text.

## 1. Skybox's scene inventory (what ships in the APK)

Skybox downloads extra scenes at run time (`DownLoadSceneAB`), so the APK holds the core
set. All scene files are Unity serialized files under `assets/bin/Data/`; the names are
the GameObject and mesh names inside them.

| Scene (root object) | Contents | Triangles | Textures (format, size) | Lit by the video |
| --- | --- | ---: | --- | --- |
| Cinema hall (`CinemaSceneModel_3Cam`) | one hall mesh, seat block split per viewpoint into two detail parts, five transparent light fixtures, 64 seat colliders in 8 rows, three camera positions | 61k to 69k per viewpoint (hall 6.6k plus the two parts for that seat) | main lights 4096² ASTC 8x8 (5.6 MB), other lights 2048² ASTC 8x8 (1.4 MB), transparent parts 2048² ASTC RGBA 5x5 (3.6 MB) and 1024² ASTC 6x6 (0.6 MB) | yes, both light layers |
| Space station lounge (`SpaceScene`) | interior 28.5k, exterior station shell, animated ring, three ships, glass with a 256² cubemap, 2000 star quads, a planet | about 37k | screen light 2048² ASTC 8x8, other light 2048² ASTC 4x4 (5.6 MB), station 2048² ASTC RGBA 4x4, Jupiter 2048² ASTC RGBA 4x4; about 18 MB | yes (`screenlightshader`) |
| Lunar surface (`LunarCinemaMesh`) | rocks 21k, terrain 28k, a chair 7k, a lander 31k, a far plane | about 87k | a 2048 cubemap with lights off and on merged (ASTC 8x8, 8.4 MB), rocks 2048² ASTC 10x10, two screen-light maps 2048² ASTC 6x6; about 14 MB | yes, via the chair and lander maps and the cubemap |
| Home living room (`HomeModelPrefab`) | one interior mesh 16.3k, a sky sphere 648 | about 17k | one 4096² ASTC 6x6 emissive bake plus a 4096² overlay (10 MB each), sky 1024²; about 21 MB | no; fully baked emissive with a gamma slider |
| Void (`VoidSceneBackground`, `VoidBackground`) | one inside-out sphere (17k triangles at radius 68 m) and a 60 m mask sphere | 17k | none besides a gradient | no |

Every scene MeshRenderer has shadow casting and receiving off. The Unity quality level in
use is the top one with 4x MSAA, one pixel light, hard shadows allowed but no real-time
light in any scene, anisotropic filtering per texture, vsync off (VrApi paces), LOD bias
2. OVRManager does not use the SDK's recommended MSAA level, leaves adaptive resolution
off (min 0.7, max 1.0 unused) and keeps "queue ahead" on. The eye cameras clear to solid
black, near plane 0.01 m, far 1000 m. The texture budget is ASTC everywhere with 4x4 for
detail maps and 8x8 or 10x10 for lightmaps; 11 to 21 MB of texture per scene.

Seat geometry, in the hall's own units (the hall prefab is scaled 1.3): rows every 2.2 m,
seats every 0.87 m, 8 by 8 seat colliders; the three viewpoints are the front seat
(0, +1.38, -4.50), the middle seat (0, +0.03, 0) and the back seat (0, -1.32, +4.50).
Skybox moves the hall by those offsets and keeps the camera at the origin. The floor
collider under the middle seat is 1.40 m below the eye. In the space lounge the sofa seat
is 0.78 m below the eye and reclined 28 degrees, floor at -1.27 m; on the moon the chair
seat is at -1.26 m and the ground at -1.55 m; at home the sofa is at -1.28 m and the floor
at -1.47 m. The screen is lowered 0.7 m in the cinema and raised 0.3 m in the other rooms
(`cinemaSceneScreenOffset`, `nonCinemaSceneScreenOffset`).

## 2. Screen geometry

The picture is a prefab under `TextureMesh/CinemaMesh/Plane`. The `Plane` node sits at
(0, 2.80, 12.42) with scale 1.4 in x and y; the flat screen quads `PlaneL`/`PlaneR` are
5.2 further along z and face back to the viewer, so the flat picture's plane is 17.6 m
ahead and 2.8 m above eye level before the user moves it. The curved variants
`CurveL`/`CurveR` are children of the flat quads, placed 19.81 units behind the picture
plane, which puts the cylinder axis about 2.2 m behind the viewer: the curvature is
close to viewer-centred. The curved mesh is Unity's unit quad reshaped at run time by
`CurveScreenSetter`; there is no baked curved mesh, so the segment count is not in the data.

`MediaScreenController`: aspect 1.77, base scale (3.0 x 1.77, 3.0) applied to the quad's
parent, scale steps 0.5, 0.6, 0.7, 0.8, 0.9, 1, 1.5, 2, 2.5, 3, 3.5 (default index 5 = 1.0),
zoom along the view axis -18 to +18, tilt -90 to +90 degrees, screen rotation -180 to
+180 degrees, minimum height above the floor 0.5 m, minimum screen centre height 0.3 m,
drag limits 85 degrees sideways and +80/-20 degrees vertically, and a spring-back. The
picture follows the head at speed 4 (`lerpSpeed`) when unlocked; "Lock screen" pins it.
Luminance slider 0.2 to 1.8 (shown as -10 to 10), ambience luminance 1 to 5.

Because the app was not run (the operator's Go is read-only for Skybox and the app runs
only in the headset), the effective default picture width in metres was not measured.
The study gives the placement (17.6 m, +2.8 m), the curvature centre (near the head),
the aspect (16:9) and the scale ladder; those are what we adopted.

## 3. Lighting technique

There is no real-time light. Each room material carries two baked light layers and a
per-frame colour taken from the picture:

- `_mediaframecolor`: every third frame (`frameInterval` 3) the video texture is blitted
  into an 8x8 render target, read back to the CPU, averaged, multiplied by a per-scene
  table indexed by the ambience slider (cinema 0.2 to 1.5, others 0.9 to 3.0), floored
  at 0.15 grey and lerped toward the material at 8 per second. `ScreenLight` does the
  same for a Unity point light in scenes that have one (colour x1.2, intensity 1 to 3).
- Cinema opaque shader: colour = 1 - (1 - frameColour x mainLights) x (1 - 0.95 x
  otherLights x luminance). A "screen" blend of the two lightmaps; the video tints the
  main-light bake, the slider scales the house-light bake.
- Space `screenlightshader`: per pixel, L is the direction to the midpoint of two
  opposite screen corners passed as uniforms (`_Apos` .. `_Dpos`), and screen light =
  clamp(N.L) x screenLightMap.r x 50 / distance² x frameColour; the result is screen-
  blended with 0.5 x otherLights x luminance and the whole thing doubled. Passing the
  corners means the bake follows the picture when the user moves or scales it.
- Lunar `RockandLMshader`: albedo x clamp(lerp(1, frameColour, map.alpha), 0.17, 5) x
  (angularSize + 0.06), where the vertex shader computes |A-C| / |P - mid(A,C)|, the
  picture's apparent size from that vertex. `LunarSurfaceShader` samples a lights-off/on
  cubemap by direction and multiplies by the clamped frame colour.
- Home: albedo = pow(bake, gamma). Nothing from the video.

So the "video lights the room" effect is one colour for the whole room, updated at 20 Hz
with a CPU readback, and pre-baked masks decide where it lands.

## 4. Performance budget observed

Per eye and frame on the Go (Adreno 530, 72 Hz, 4x MSAA; the eye buffers are 1.4 times the
runtime's 1024x1024, because `MainProcedure` sets `eyeTextureResolutionScale` 1.4 on an
Oculus Go, as PLE-721 found):
about 60k to 90k triangles for the rooms, 12 to 20 draw calls, 11 to 21 MB of ASTC
textures resident, one 8x8 downsample blit every third frame, one CPU readback of 64
pixels every third frame. The video itself is drawn on a mesh (a quad reshaped into a
cylinder section). PLE-721 corrected an earlier reading: the menus do not use a compositor
layer either. The `OVROverlay` cylinder prefab (8.36 x 3.19 units at 5.8 m) is compiled in
but switched off by a hard-coded `false`. The UI is world-space canvases in the eye buffers,
which Skybox renders 1.4 times larger on the Go (`vr-ui.md` §8).

Thin geometry: the transparent light fixtures are separate meshes with their own
lightmaps and MSAA does the rest; there are no sub-pixel bars in the hall, the sconces
and steps are chunky boxes.

## 5. What we adopted, what we changed

Adopted:

- Viewer-centred curvature (radius equals distance by default) and a 16:9 picture.
- Baked light, no real-time shadows, two light components: the room's own lights and
  the picture's glow; a floor under the glow so a black frame never turns the room off;
  temporal smoothing of the glow colour.
- Seats behind and beside the viewer, never in front of the picture; a matte black
  frame around the picture; dark rooms with a few warm emissive fixtures; a big dark
  sphere for the void with a gradient, and stars as points.
- Low triangle counts and few draw calls; 4x MSAA is left to PLE-602's swapchain.

Changed, for latency and for the Go's budget:

- The glow map stays on the GPU. `begin_frame` renders the decoder's external texture
  into an 8x8 RGBA target with a 4x4 tap grid per texel and blends it into the previous
  map with constant alpha 0.13 (Skybox's 8/s lerp at 60 Hz), then a mip chain gives the
  frame average. No CPU readback, no extra copy of the frame, and the decoder's
  SurfaceTexture is sampled in place exactly as the plain screen samples it.
- The glow is directional: the room vertex shader finds the point of the picture nearest
  to each vertex and samples the 8x8 map there, so the left wall takes the left side's
  colour. Skybox uses one colour for the whole room.
- Light is evaluated per vertex from the configured screen rectangle (Lambert x facing x
  area / distance²) instead of a baked mask, so it follows the screen distance, width
  and height settings without a rebake. Room lights are point lights evaluated once per
  vertex while the mesh is built (inverse-square, Lambert), which is what a lightmap bake
  of the same room would give without bounce.
- No textures at all besides the glow map: procedural geometry with vertex colours, a
  striped curtain and dithering in the fragment shader against banding on the Go's panel.

## 6. Our environments

All procedural, in `android/app/src/main/cpp/vr-environment.cpp`; metres, y up, viewer
at the origin, picture centred on -z. Triangle counts are per eye with the default
screen; the picture strip is 96 triangles and the glow pass one triangle.

| Environment | What it is | Room triangles | Draw calls per eye |
| --- | ---: | ---: | ---: |
| plain (default) | black plus the picture: PLE-602's picture | 0 | 1 |
| void | 60 m dome with a per-pixel gradient tinted by the picture, an additive halo behind the picture, matte frame | 96 | 4 |
| cinema | screening room: pit and stage front below the picture, striped curtain wall, tessellated side walls, ceiling with a dim house light, five rows of theatre seats (the viewer in the front row, 15 seats a row), sconces every 2.5 m, aisle strips on the risers | about 8,300 | 2 |
| terrace | night terrace: dark stone floor to 14 m, low parapet under the picture, two path lights, star dome | about 1,700 | 5 |

Seated eye height is 1.2 m above the row floor; rows behind rise 0.32 m per 1.15 m
pitch; the hall is at least 10 m wide and grows with the picture; the curtain wall is 1 m
behind the picture; the ceiling is 5 m above the floor or 1.6 m above the picture's top,
whichever is higher. Seats are 0.52 m wide, 0.50 m deep, cushion at 0.44 m, backrest to
1.05 m; the viewer's own seat sits at the origin so its armrests give a parallax cue.
The stage front stops 0.35 m below the picture's bottom edge so nothing ever crosses it.
Emissive fixtures are 6 cm or larger, so they never shimmer at 1024 pixels per eye.

Settings (offered only when `Build.DEVICE == "pacific"`; the phone's settings screen
does not change): environment (plain, void, cinema, terrace), screen distance 80 to
1200 cm (default 300), screen width along the surface 50 to 1600 cm (default 419, that
is 80 degrees at 3 m), curve radius 0 = flat or 50 to 1600 cm (default 300), screen height
above eye level -200 to 200 cm (default 0), glow 0 to 100 (default 60), room lights 0 to
100 (default 35). The defaults reproduce PLE-602's plain screen exactly.

## 7. Integration with PLE-602

The renderer has no VrApi dependency. PLE-602's native `Cinema` (`src/vr/cpp/vr-cinema.cpp`,
built only with the licensed SDK present) keeps its EGL context, swapchains and the
decoder's external texture, and since this ticket calls the renderer as follows:

- `StreamVrActivity`'s render thread reads `Preferences.vrEnvironmentConfig().toNative()`
  once after `VrCinemaNative.create` and passes it to `VrCinemaNative.setEnvironment`.
  With the default `plain` the native side drops the renderer and its own path runs
  byte for byte as PLE-602 landed it (rule 4). Anything else builds the renderer on
  the GL thread with `pleikkari_vr_environment_create(&cfg, GL_TEXTURE_EXTERNAL_OES)`;
  a room that fails to build logs `GoCinema` and keeps the plain screen.
- `Cinema::draw` calls `begin_frame` once per frame before the eye loop (the glow map
  refresh, only when the Kotlin side reports a new decoder frame), then per eye binds
  the swapchain framebuffer, sets the viewport and calls `draw_eye` with
  `view * screen` (the recentred screen space PLE-602 already uses for its strip) and
  the eye projection, both transposed to column-major. `draw_eye` clears the eye,
  attaches its own depth renderbuffer for the duration of the eye and detaches it after
  an invalidate, draws the room and the picture (or a dark idle panel without video).
  PLE-602's own strip pass then draws only when there is a message or the menu, on top.
- Every 720 frames (10 s at 72 Hz) `Cinema` logs `Environment frame: gpu … ms, … draws,
  … triangles` from `pleikkari_vr_environment_stats` (`GL_EXT_disjoint_timer_query`, one
  frame late). PLE-601's rounds should read it next to the frame timing. Since PLE-651
  `Cinema` calls `pleikkari_vr_environment_end_frame` after the eye loop (before
  `glFlush`), so the figure spans the whole eye frame, strip, border clears and MSAA
  resolves included; before it stopped after the second `draw_eye` and under-reported
  (~2.5 ms against VrApi `App=` 7.5-8.6 ms, PLE-623). The host tool never calls it and
  keeps the old span.
- `pleikkari_vr_environment_destroy` runs in `Cinema`'s destructor, on the GL thread,
  before `vrapi_LeaveVrMode`.

The library is `libpleikkari-vr-environment.so` (CMake target `pleikkari-vr-environment`,
built for every ABI in every gate); `pleikkari-vr` links it and `GoVrSupport` loads it
before `pleikkari-vr`. The wired `vr-cinema.cpp` was first type-checked against a local
stub of the VrApi declarations; since PLE-617 fetched SDK 1.35.0 it compiles and links
against the real headers and `libvrapi.so` with no changes and no `-Wall -Wextra`
warnings, and against 1.50.0's headers too (PLE-623).

## 8. Verification

- Host: `android/vr-environment/render-host.sh --check` builds the same source against
  Mesa's EGL/GLES 3.2 (llvmpipe, surfaceless) and renders every environment as a stereo
  pair at two head yaws to `build/vr-environment/*.png`, asserting the picture centre
  shows the test pattern, the plain surroundings are black, the rooms are lit but darker
  than the picture, the lit wall next to the picture takes the picture's colour, and no
  GL error is raised.
- Device: the debug-only `VrEnvironmentPreviewActivity` draws the stereo pair on a flat
  display with a synthetic picture and logs `gpu=` (timer query), `frame=`, `draws=` and
  `tris=` under `VrEnvPreview` every 72 frames:

  ```
  adb shell am start -n fi.madekivi.pleikkari/.stream.VrEnvironmentPreviewActivity --es environment cinema
  adb logcat -s VrEnvPreview
  ```

  Proven on the API 36 emulator (software GL, so the `gpu=` field reads 0 there: no
  timer-query extension). Measured on the Go on 2026-09-27 (serial 1KWPH802EW8203,
  Adreno 530, the preview on the 2D panel at 72 Hz, two 1024x1024 eyes per frame,
  no MSAA, `logcat -s VrEnvPreview`; captures and log excerpts under
  `build/proof/go-preview-*.png` and `build/proof/go-logcat-*.txt` of the worktree):

  | Environment | GPU per stereo frame | Draw calls | Triangles |
  | --- | ---: | ---: | ---: |
  | plain | 0.8 to 1.1 ms | 2 | 192 |
  | cinema | 4.8 to 5.6 ms | 5 | 16,805 |
  | void | 6.3 to 7.2 ms | 9 | 1,925 |
  | terrace | 7.6 to 8.5 ms | 11 | 5,109 |

  The rooms are fill-rate bound, not triangle bound: the void and the terrace cost more
  than the cinema with a tenth of its triangles because their dome shader runs on every
  pixel of both eyes (plus the star and halo overdraw), while the cinema's walls are
  cheap lit triangles. At 72 Hz the frame is 13.9 ms and TimeWarp needs its share, so
  the cinema is the room to A/B first; the void and terrace need the dome drawn after
  the picture with the depth test on (skipping the picture's pixels) before they are
  cheap enough for a stream (§9).

  PLE-630 re-measured the void and the terrace after PLE-622 on the same Go and
  preview, interleaving the variants in one session (12 one-second `gpu=` samples each,
  temporary builds, none landed): void 4.9 to 5.1 ms, terrace 5.3 to 5.5 ms per stereo
  frame; plain 1.0 to 1.2 ms.

  | Sky variant | void | terrace |
  | --- | ---: | ---: |
  | as shipped (gradient + hash dither) | 5.0 ms | 5.4 ms |
  | no dither (a switch through set_config) | 5.1 ms | 5.4 ms |
  | constant colour, no maths at all | 4.2 ms | 5.0 ms |
  | dome not drawn | 2.6 ms | 4.1 ms |

  The sky's fragment maths is at most 0.8 ms (void) and 0.3 ms (terrace), and the
  dither costs nothing measurable. Most of the dome's 2.4 ms (void) and 1.3 ms
  (terrace) is paid for drawing it at all, so a cheaper shader or a baked cubemap
  cannot reach the ~2 ms target; the next suspect is the draw itself (a full-sphere,
  culling-off mesh on the far plane with depth test on and depth writes off). Single
  back-to-back runs drift by up to 1 ms as the GPU warms, so compare variants only
  interleaved in one session.

  PLE-650 put the sky's draw variants behind a debug switch
  (`pleikkari_vr_environment_debug_set_sky_variant`, `PLEIKKARI_VR_SKY_*` in
  `vr-environment.h`; the shipped dome is variant 0 and the only one outside the preview)
  and measured them interleaved on the Go (serial 1KWPH802EW8203, same preview,
  `--ei sky_variant N` sent to the running preview every 5 s, four rounds per room, the
  first sample after each switch dropped, two sessions). Medians of ~17 to 24 one-second
  `gpu=` samples each, session 1 / session 2, per stereo frame:

  | Sky variant | void | terrace |
  | --- | ---: | ---: |
  | 0: 32x12 dome, culling off (shipped) | 4.95 / 5.00 ms | 5.28 / 5.32 ms |
  | 1: same dome, back faces culled | 5.04 / 4.96 ms | 5.38 / 5.39 ms |
  | 2: 8x4 dome | 4.87 / 4.91 ms | 5.30 / 5.36 ms |
  | 3: one fullscreen triangle, direction from the inverse view-projection | 4.99 / 4.99 ms | 5.40 / 5.29 ms |
  | 5: shipped dome, constant colour | 3.43 / 3.50 ms | 4.61 / 4.48 ms |
  | 6: fullscreen triangle, constant colour | 3.29 / 3.39 ms | 4.51 / 4.38 ms |
  | 4: no sky drawn | 2.65 / 2.56 ms | 3.99 / 4.01 ms |

  The geometry is not the cost: culling, a quarter of the triangles or a single
  fullscreen triangle all land within 0.1 ms of the shipped dome (the dome already
  covers each sky pixel exactly once from the centre, so culling removes no fragments).
  The dome's 2.4 ms (void) / 1.3 ms (terrace) splits into about 0.8 / 0.5 ms of plain
  fill for the uncovered pixels, which any per-pixel sky pays, and about 1.5 / 0.85 ms of
  gradient maths (`inversesqrt`, `exp`, the dither hash) on those pixels. This
  contradicts PLE-630's table above (constant colour only 0.8 ms under the gradient, and
  1.6 ms above no sky); both PLE-650 sessions agree with each other within 0.15 ms, and
  PLE-630's constant-colour build was a temporary one that was not kept, so its exact
  shader is unknown. So a cheaper sky shader *is* the lever: the per-pixel maths
  can move to the vertices (only the elevation matters, and the dome's stacks follow
  it), and the fill floor goes only if a flat sky colour is folded into the clear the eye
  already does.
- Device, inside the VrApi cinema (PLE-623): the debug preview of `StreamVrActivity`
  (`third_party/ovr_sdk_mobile/README.md`, "Debug preview") runs the real cinema with a
  synthetic 60 fps picture through the decoder's `SurfaceTexture`. On the Go on
  2026-09-27 (serial 1KWPH802EW8203, SDK 1.35.0, tip 1ab2b5b8 with PLE-622's reorder and
  PLE-615's 4x MSAA, 72 Hz, 1024x1024 eyes, `vrapi_SetClockLevels(2, 2)`; the runtime
  ran the GPU at level 2 to 3, 315 to 401 MHz), VrApi's own per-second `App=` GPU time:

  | Environment | VrApi `App=` GPU (median) | Stale frames/s (mean, max) | `Environment frame: gpu` |
  | --- | ---: | ---: | ---: |
  | plain | 0.55 ms | 0, 1 | (not logged) |
  | cinema | 7.54 ms | 2.5, 15 | 2.55 ms |
  | void | 8.59 ms | 4.9, 11 | 2.55 to 2.57 ms |
  | terrace | 8.53 ms | 4.8, 14 | 2.50 ms |

  Every room costs 7 to 8 ms of GPU per frame on top of the plain screen in VrApi, and
  each drops frames (TimeWarp reuses a stale eye buffer) at the clock level the cinema
  asks for; the plain screen does not. `Environment frame: gpu` does not see this: it
  reads about 2.5 ms for all three rooms. Its `GL_TIME_ELAPSED` query ends right after
  the second eye's draws are issued, before that eye's tiled pass (and its MSAA
  resolve) runs, so in VrApi read `logcat -s VrApi` `App=` instead. Captures under
  `build/ple-623/go/run6-preview/` of the workspace.
- MSAA share (PLE-653): the preview's debug extra `--ei environment_msaa 1|4`
  (default 4) sets the rooms' sample count for that run. On the Go (1KWPH802EW8203,
  same setup, 2026-09-27), eight 35 s runs interleaved 4x/off, two per cell, median
  `App=` over the last ~31 s of each:

  | Environment | 4x MSAA `App=` | MSAA off `App=` | Stale frames/s at 4x (mean) | off |
  | --- | ---: | ---: | ---: | ---: |
  | cinema | 9.01, 7.58 ms | 3.51, 3.50 ms | 5.5, 5.0 | 0.3, 0.0 |
  | void | 8.61, 8.62 ms | 3.33, 3.34 ms | 1.1, 0.5 | 0.1, 0.1 |

  4x MSAA is about 4 to 5.5 ms of the 7 to 8 ms room cost, well over half; without it a
  room costs about 3 ms over the plain screen's 0.55 ms and drops almost no frames.
  Logs under `build/ple-653/go/` of the workspace.
- Sky variants inside VrApi (PLE-666): with the preview, `--ei sky_variant N` draws
  PLE-650's debug sky in the real cinema (0 shipped dome, 3 fullscreen triangle,
  4 no dome, 5 constant-colour dome, 7 PLE-665's per-vertex gradient). On the Go
  (1KWPH802EW8203, tip d23b066a plus this change, 4x MSAA, 72 Hz, 2026-09-27), two
  interleaved rounds of 36 s runs per cell, median `App=` after 3 warm-up seconds,
  split by the GPU clock the runtime picked (315 or 401 MHz, per run, on its own):

  | Run | `App=` @315 MHz (s) | `App=` @401 MHz (s) | Stale/s mean, max | `Environment frame: gpu` |
  | --- | ---: | ---: | ---: | ---: |
  | void, dome (0) | 7.55 ms (2) | 7.60 ms (62) | 2.5, 14 | 2.37 ms |
  | void, fullscreen (3) | 9.46 ms (32) | 7.60 ms (32) | 0.9, 6 | 2.58 ms |
  | void, no dome (4) | 7.47 ms (64) | - | 0.6, 5 | 1.18 ms |
  | void, constant (5) | 8.58 ms (23) | 7.02 ms (41) | 9.4, 24 | 1.92 ms |
  | void, vertex gradient (7) | 9.11 ms (33) | 7.38 ms (31) | 1.4, 15 | 2.44 ms |
  | terrace, dome (0) | 9.48 ms (32) | 7.55 ms (32) | 1.4, 14 | 2.63 ms |
  | terrace, fullscreen (3) | - | 7.51 ms (63) | 2.5, 15 | 2.30 ms |
  | terrace, no dome (4) | 9.11 ms (3) | 7.50 ms (61) | 2.8, 17 | 2.28 ms |
  | terrace, constant (5) | - | 7.68 ms (64) | 3.7, 13 | 2.48 ms |
  | terrace, vertex gradient (7) | 7.52 ms (1) | 7.54 ms (62) | 2.9, 18 | 2.34 ms |
  | plain | 0.54 to 0.57 ms | | 0 | (not logged) |

  The terrace is clean: at 401 MHz every variant, no dome included, reads 7.50 to
  7.68 ms, so there the sky is worth about 0.1 ms of the ~7 ms room cost. The void is
  confounded by the clock: its no-dome runs both landed at 315 MHz and read 7.47 ms,
  against 8.6 to 9.5 ms for the dome variants at 315 MHz, which would put the void's
  sky at 1 to 2 ms, but its shipped dome at 401 MHz reads the same 7.6 ms. A void
  re-run pinned to one GPU clock would settle it. PLE-650's preview split (2.4 / 1.3 ms
  of sky) does not carry over to VrApi as-is; MSAA (PLE-653) stays the bigger lever.
  `Environment frame:` does see the dome (void 2.37 -> 1.18 ms without it) but, as
  above, under-reports the frame. Logs, `run.sh` and `sum.py` under `build/ple-666/` of
  the workspace.
- Device, live PS5 stream (PLE-654, `docs/verification/PLE-654.md`): the cinema room
  with a live 1080p60 picture measured VrApi `App=` 8.02 ms (median) and 3.0 stale
  frames/s (max 26), as in PLE-623's preview. `Environment frame: gpu` still read 2.40
  to 2.90 ms with PLE-651's change in the build, so it still reads about a third of
  `App=`.
- Unit: `VrEnvironmentConfigTest` pins the defaults to PLE-602's screen, the clamps and
  the native enum values.

## 9. Open points

- The void and the terrace cost 6 to 8.5 ms of GPU per stereo frame on the Adreno 530
  (§8) before PLE-622 and about 5 ms after it; PLE-650 showed the remaining sky cost is
  about a third fill and two thirds gradient maths, and none of it geometry (§8), so a
  cheaper sky shader (the gradient per vertex) is the next step, not a cheaper mesh
  (this supersedes PLE-630's reading that the shader is not the lever).
  Inside the VrApi cinema the terrace's sky variants, no dome included, are within
  about 0.1 ms of `App=` at a fixed clock; the void is unsettled by the runtime's clock
  choice (PLE-666, §8).
  Inside the VrApi activity every room, the cinema included, costs 7.5 to 8.6 ms of
  GPU per frame and drops 2.5 to 5 frames a second at `vrapi_SetClockLevels(2, 2)`
  (§8, PLE-623, and on a live stream PLE-654); 4x MSAA is 4 to 5.5 ms of that (§8,
  PLE-653), so a lower sample count (2x) or MSAA off is the first lever; a higher GPU
  level while a room is on, and a timer that covers the whole frame are open (PLE-651's
  timer change still reads 2.4 to 2.9 ms against `App=` 8.0 ms, PLE-654).
- Eye buffer size belongs to PLE-602's swapchain. MSAA is done (PLE-615): while an
  environment is active the VrApi cinema renders its eyes through
  `GL_EXT_multisampled_render_to_texture` at 4x, like Skybox, with a matching
  multisampled depth buffer; the plain screen keeps its single-sample FBOs. The
  `VrApi cinema entered` log line prints the sample count actually used.
- A cylinder compositor layer for the picture (`VRAPI_LAYER_TYPE_CYLINDER2`) would
  sample the decoder texture once instead of through the eye buffer; Skybox does not do
  it for the picture either, but it is the next sharpness step for PLE-602.
