# VR UI on the Oculus Go: how Skybox does it, and the toolkit we build

PLE-721, a study with no app change. The operator wants the whole app to use VR-native UI
when it runs on the Oculus Go ("make the whole UI of the app use VR native ui elements only
when it's being run on Oculus", 2026-09-27), again taking Skybox VR Player as the model.
This note records how Skybox draws its menus, pointer, keyboard and layouts, what the
VrApi 1.35 runtime on the Go offers for the same job, and ends with the toolkit
specification PLE-722 and its follow-ups build from (§10).

Nothing from Skybox ships here: no sprite, icon, font, prefab, shader or code. The numbers
below were read from the APK's serialized Unity data and decompiled scripts and are
restated in our own words. Where Skybox uses a third-party library, that is named, because
it tells us which parts are Skybox's design and which are defaults it inherited.

Sources:

- **Skybox.** The same `skybox-player-ovr-1.0.0-vc264.apk` PLE-603 studied
  (`docs/design/vr-environments.md`; Unity 2018.3.6f1, Mono), kept outside git under
  `android-low-latency-examples/skybox-apk/` in the workspace. The assembly is obfuscated,
  every class carries decoy copies of its methods with altered constants, and whole-assembly
  decompilation crashes. So types were decompiled one at a time with ilspycmd 9.1, and only
  code reached from Unity entry points, public API or delegate wiring was trusted. Scene and
  prefab values were read with UnityPy 1.25, with typetrees generated from the game
  assemblies.
- **Meta.** The Mobile SDK 1.35.0 headers and samples PLE-617 fetched
  (`build/sdk/ovr_sdk_mobile/`).
- **The Go.** VrApi stats lines already in the workspace's Go captures (`build/`).

Skybox was not run for this study, so every Skybox cost below is inferred, not measured.

## 1. Where we start

Everything the Go shows today outside the picture is one texture on the stream's own
curved strip (PLE-602, PLE-690; `android/app/src/vr/`):

- `StreamVrActivity.CinemaThread.uploadText` draws the text with `android.graphics.Canvas`
  into a 1536x864 bitmap (background `rgb(12, 15, 22)`, white 36 px text, 64 px line pitch,
  centred) and uploads it with `GLUtils.texImage2D` on the GoCinema GL thread whenever the
  text changes.
- `vr-cinema.cpp` draws that texture on the same 64-segment strip as the picture: a
  3 m radius cylinder 80.2 degrees wide and 43 degrees high. That is 19 texels per degree,
  so the 36 px text is a 1.9 degree em and a line is 3.3 degrees. Into the 1024x1024 eye
  buffer it is minified without mipmaps, then TimeWarp resamples it again.
- With the menu open the strip follows the head (`model` = head orientation plus position),
  so Back always brings it into view. Closed, the strip is world-locked in the recentred
  screen space.
- Input is polled in `Cinema::input`: the Go controller's Back press edge toggles the menu,
  and a touchpad click is sorted by the horizontal third of the touchpad it was made on
  (left, centre, right). The menu reads "Resume | Recentre | Disconnect" and the Library
  chooser (`GoVrLibraryFlow`) moves with the left and right thirds and picks with the
  centre. A Bluetooth pad drives the chooser with the D-pad, A and B; during a stream every
  pad key goes to the console through `StreamInput`.
- There is no pointer, no hover, no focus and no layer besides the one projection layer
  (`LCnt=1` in VrApi's stats).

So the Go has one text screen and a three-way touchpad. What the operator asks for (menus,
settings, the console list, linking, all in VR) needs panels, a pointer, focusable widgets,
scrolling and a keyboard.

## 2. Skybox: panels

**Which code runs.** The pointer and keyboard classes the ticket names (`GvrLaserPointer`,
`GvrPointerInputModule`, `GvrPointerScrollInput`, `GvrKeyboard`, and others) are Google's
Daydream (GVR) Unity SDK. They are compiled into the APK, but nothing on the Go path
instantiates them. The Go build runs Skybox's own modified copy of Oculus's
`OVRInputModule`, which `PlayerInitScript.Awake` adds at start-up; an `OVRRaycaster` on
every canvas; and a reskin of the Weelco VR Keyboard asset (§6). `GvrKeyboard` could not
work on a Go anyway: it needs Google's `com.google.android.vr.inputmethod` package and a
native shim that is not in the APK.

**Geometry.** All 40 canvases are Unity world-space canvases, flat, world-locked, with no
curvature. Everything hangs under `Player Canvas` in `level0`: 2600x2000 units at 4.6 m
straight ahead, 3 mm per unit. Three state roots rescale it. Home is x1.25 (3.75 mm per
unit, 1.2 m further away, at 5.8 m); the library opened during playback and the player
controls are x1.1 (3.3 mm per unit, at 4.6 m). Angles below are 2 atan(size / 2d):

| Panel | Canvas units | Distance | Angular size | Centre elevation |
| --- | --- | ---: | --- | ---: |
| Home library, centre | 1280x850 | 5.8 m | 45.0 x 30.7 degrees | +3.7 |
| Home side panels (channels left, recent right) | 450x850 each | 0.49 m nearer, yawed 30 degrees in | the three together 79.6 degrees wide | +3.7 |
| Library during playback | same three | 4.6 m | centre +-24.7, all three 86.6 degrees | +7.1 |
| Player controls | 1280x466 | 4.6 m | 49.3 x 19.0 degrees | -1.1 from the gaze |
| Player's top row of round buttons | 128-unit circles, 111 apart | 4.6 m | 5.3 degree circle, 3.0 degree icon | +5.9 |
| Cinema scene picker | 1280x560 | 4.6 m | 49.3 x 22.8 degrees | +8.3 |
| Keyboard (§6) | 1217x428, scaled 0.55 x 0.6 | 2.2 m | 48.5 degrees wide, top at eye level | -9.7 |
| Toast | 66 units tall | 1.3 m, tilted 10 degrees | 5.2 degrees tall | -10.5 |
| Recentre countdown ring | 100 units | 3.1 m | 5.5 degrees | 0 |

The wrap-around look is a triptych: two flat side panels yawed 30 degrees toward the user
and pulled 0.49 m closer, not a curved sheet. The dormant overlay prefab's
8.3625 x 3.1875 x 5.8 scale (§8) is the home centre panel, not a separate UI.

**Placement.** Panels stay world-locked. When the player controls are summoned, the whole
interface root turns to face the gaze (yaw and pitch in the VR video modes; the screen's own
rotation in cinema mode) and then stays put. The in-app Reset View shows a 3 s filling ring,
lets the UI's yaw follow the head while it fills, then recentres the pose. A "Click to reset
screen" prompt appears when the user has looked away from a flat screen for more than
0.5 s. On the Home floor, a recentre button fades in only while the head is pitched 30 to
90 degrees down.

Menus sit far away (4.6 to 5.8 m), well in front of a picture that is 17.6 m out; only the
keyboard (2.2 m) and the toast (1.3 m) come closer.

## 3. Skybox: readability

**Fonts.** Calibri Bold carries almost everything; Calibri regular, DengXian (CJK) and
Arial are bundled too, swapped per language by the I2 Localization package. Calibri's cap
height is 0.638 em.

**Sizes.** The 466 text components (rich-text `<size=N>` overrides included) cluster as
follows:

- 30 is the body standard, with 161 uses.
- 28 to 34 are labels, and 36 to 37 are panel titles.
- 25 to 26 are used in dense lists.
- 48 is the keyboard keys, 56 the recentre prompt, and 64 to 68 dialog headlines.
- Line spacing is 1.0 in 435 of 472 cases.

At the panel centre one canvas unit is 2.2 arcminutes at Home (27.0 units per degree) and
2.5 arcminutes in playback (24.3 units per degree):

| Font size | Home: em, cap height | Playback: em, cap height |
| ---: | --- | --- |
| 25 | 0.93 degrees, 35' | 1.03 degrees, 39' |
| 30 (body) | 1.11 degrees, 43' | 1.23 degrees, 47' |
| 34 | 1.26 degrees, 48' | 1.40 degrees, 53' |
| 37 (titles) | 1.37 degrees, 52' | 1.52 degrees, 58' |

So body text is a 1.1 to 1.25 degree em with a 43 to 47 arcminute cap height. At the Go's
12.8 to 14.2 display pixels per degree, that is 9 to 11 pixels of cap height. The toast
breaks the scale: its size-28 text sits at 1.3 m and comes out at a 2.2 degree em.

**Rasterisation.** The canvas scalers use a dynamic pixels-per-unit of 1, so a size-30 font
is rasterised at about 30 px per em, with no mipmaps, and then shown at 15 to 18 px per em.
That is a slight minification, which the 1.4x eye buffer and 4x MSAA soften (§8).

**Colours.**

- Text is white, or white at alpha 0.78 in many buttons' normal state.
- Titles are #c5c5c7; secondary text is #8a8a8a, brightening to #e1e1e1 on hover.
- Disabled states are alpha fades to 0.16 to 0.31 (for example, the seat picker's
  rgba(0.5, 0.5, 0.5, 0.188)). At that alpha they nearly vanish on the Go.

## 4. Skybox: pointer model

**Ray.** Each frame the controller root takes the runtime's controller pose
(`OVRInput.GetLocalControllerRotation`/`Position`). On the Go that pose already carries the
system's 3DoF arm model. Skybox adds no arm model and no smoothing. The ray starts at the
controller root and points along its forward axis; handedness follows the system setting.
Head gaze is used only for the Gear VR headset's touchpad.

**Hit testing.** Each UI element is tested against its own rectangle's plane; Unity's
`Graphic.Raycast` then applies masks. Results are sorted by depth, and hits behind the
camera or blocked ones are dropped. Tilted or nearer sub-panels therefore work without a
shared canvas plane. The UI hit test has no length limit; physics hits go to 999.9 m. The
cursor's shown distance is clamped to 5 m in the player and 8 m at Home.

**Laser.** A bent tube (Daydream Elements' "flex laser"):

- It starts 4.1 cm in front of the controller model's origin and ends 20% of the way to
  the cursor, so it never reaches the panel.
- It is opaque for its first 10% and fades to nothing by 90%.
- It is white at alpha 0.64 and 5 to 7 mm thick.
- It is drawn last, with the depth test off.

**Reticle.**

- A 16x16 px disc texture on a quad, scaled by its distance so its angular size stays
  constant: 0.63 degrees idle in the player, 0.80 degrees over something clickable or
  pressed; 0.52 and 0.69 degrees at Home. Size changes tween over 0.1 s.
- It lies flush with the plane of the element it hit, and is drawn last with the depth test
  off.
- It shows only over UI, and lingers 2 s (player) or 0.5 s (Home) after the ray leaves.
- Tooltips appear after 0.75 s of hover, fading in over 0.28 s.

**States.**

- A click fires on release, only on the element that was pressed, and only if the trigger
  or the touchpad click was really held. A bare touch never clicks.
- A drag starts once the hit has moved 3 degrees, measured from the head, away from the
  press point (`angleDragThreshold = 3` in `PlayerInitScript.Awake`). A drag cancels the
  click. Sliders act only while a button is held.
- Buttons (`SpriteButton`, 695 of them) have normal, hover, pressed and disabled states, each
  in a selected and an unselected variant. Each state is a sprite swap plus a colour tween of
  0.3 s (0.15 s on small items). A typical dark button goes rgb(40, 41, 43) normal,
  rgb(58, 59, 62) hover, rgb(36, 37, 39) pressed; a typical label goes alpha 0.78, 1.0, 0.88.
- About 65 elements pop 20 units (6 cm) toward the viewer on hover and about 14 grow by 5%,
  both over 0.3 s; most do neither.
- There are no UI sounds and no haptics.

**Controller model.** A dot on the model's touchpad follows the finger, but is updated only
every 10th frame. A battery indicator has four levels. Hiding the player controls hides the
controller model, and the laser with it.

## 5. Skybox: controls

| Input | Skybox on the Go |
| --- | --- |
| Trigger or touchpad click | Select, on release; both work everywhere |
| Hold a button and move 3 degrees | Drag: scroll a list, move a slider, move the video screen |
| Touchpad drag, player | After the finger has moved half the pad: horizontal seeks (a 5 s step, adjustable 1 to 60 s; holding the click repeats a step every 0.4 s), vertical scales the screen (0.5x to 3.5x) |
| Touchpad drag, lists | Nothing: lists scroll only by pointer drag, elastic (0.1) with inertia (deceleration 0.135) |
| Touchpad swipe | Nothing on the Go (left/right swipe events are dispatched only for the Gear VR touchpad) |
| Click on empty space | Resolved 0.3 s later: single toggles the player controls, double is play/pause |
| Back, short | Pops a last-in-first-out handler stack: close the dialog, go up a folder, leave the player for Home; at Home "click again to exit", then exit |
| Back, held over 0.75 s | Recentre the video screen on the gaze |
| Oculus button | System only; on the runtime's recentre, Skybox resets the UI rotation and tilt |
| Gamepad | Effectively unsupported (below) |

Back is read through Unity's key path (Escape), which sees the button's down and up, so
Skybox can time a long press itself. The player controls hide 3 s after the pointer last
touched them.

**Gamepad.** Unity's navigation events are enabled (10 moves a second, 0.6 dead zone), but
nothing ever selects a first element and Skybox's buttons are not `Selectable`s, so a pad
moves nothing. Only Space (click) and Escape (back) do anything.

## 6. Skybox: keyboard

One keyboard is live: `SkyboxVR.VRKeyboardFullCustom`, a reskin of the Weelco VR Keyboard
asset. It appears only inside the network-share (SMB) login form, for a user name and a
password. The Weelco originals are compiled in but have no instances, and there is no
Android IME or `TouchScreenKeyboard` use at all.

**Layout.** 44 keys in five rows, with no stagger:

| Row | Letters page |
| --- | --- |
| 1 | 1 2 3 4 5 6 7 8 9 0 |
| 2 | q w e r t y u i o p |
| 3 | sym a s d f g h j k l |
| 4 | shift z x c v b n m backspace |
| 5 | .com @ space . enter |

The symbols page replaces rows 1 to 4 with punctuation, brackets and currency signs. `abc`
takes the place of `sym`, and a `№` key takes the place of shift.

- Keys are 120 units wide on a 122 pitch; shift and backspace are 180, `.com` and enter 181,
  space 608.
- On the Go the keyboard is about 2.0 x 0.77 m at 2.2 m: 48.5 degrees wide, running from eye
  level down to -19 degrees. That makes a key about 5.1 x 3.7 to 4.2 degrees, with
  0.1-degree gaps and a 2.2-degree label.
- It is a flat slab tilted 4.9 degrees top-away, like a lectern facing the eye. The form card
  above it is tilted the other way.
- The non-uniform 0.55 x 0.6 scale squashes the glyphs by 8%.
- Labels are Calibri at size 48, white at alpha 0.78. Keys are #28292b on a #3b3c3f plate.

**Input.**

- A key types on release, with the trigger or the touchpad click. Hover and press are
  colour tweens over 0.3 s (key #28292b, then #3a3b3e, then #242527).
- Keys neither pop out nor grow.
- There is no key repeat, no caret (text is only appended), and a 30-character limit.
- Shift is a caps lock with no lit state.
- The password shows in clear text.
- Enter submits the form rather than moving to the next field. Back is not handled.

**PIN pad.** A separate 3x4 pad is used for a hidden-folder code: 1 to 9, clear, 0,
backspace.

- Cells are about 6.8 x 5.2 degrees at 2.3 m. It flies in from behind while fading in, over
  0.3 s.
- Four slots show underlines; the next empty slot's underline is lit.
- Clear and backspace are disabled while the code is empty; OK is enabled only at 0 or 4
  digits.
- A wrong code turns the title red and shakes it for 1 s.

Numeric and IP layouts exist only as unused vendor data; nothing in Skybox takes an address.

## 7. Skybox: layouts and transitions

**Hierarchy.** Skybox has three state roots, and the same library panel moves between them.

- **Home** is the triptych:
  - channels on the left (videos, VR videos, online, local network, AirScreen, hidden
    files, shortcuts; rows 410x89);
  - the library in the centre (title, "all files / directory" pill tabs, a breadcrumb bar of
    up to five crumbs, then a grid);
  - favourites and history on the right.
- **The grid** has three file cards a row, each 340x304 units (12.5 degrees wide at Home)
  with a 340x210 thumbnail, name and duration. Directories and network devices get four
  cards a row. List mode uses 1140x86 rows.
- **Global settings** replace the grid with a scroll of 540x110 or 540x160 rows. There are no
  dropdowns anywhere. Choices are radio groups of toggles, and on/off is an 80x42 switch.
- **Player controls:**
  - a top row of six round buttons (close, channels, recentre, mirror, favourite, change
    scene), each with a hover tooltip;
  - a control pad with title, progress bar and previous/play/next;
  - popovers: a 3x3 video-format grid (2D, side-by-side, top-bottom by flat, 180, 360), an
    advanced tree of sliders (tilt, height, zoom, 3D offset, luminance, saturation,
    contrast, speed), and subtitle and audio-track lists.
- **Cinema scene picker:** four scene cards in a row, a luminance slider (1 to 5) and a
  front/middle/back seat choice.

**Sliders** are one setting per 560x140 box:

- a header line with a 40-unit icon, the label and the value;
- a 10-unit visible track on a 58-unit hit area, with a 26x46 capsule handle;
- minus and plus step buttons (52x70) at the ends, stepping by whole numbers.

The step buttons matter on a 3DoF laser.

**Scroll views** are vertical only, elastic, with inertia, clipped with 52-unit gradient
fades at the top and bottom.

**Motion.** DOTween's default ease is set to OutQuad.

- Panels move between state roots over 0.4 s.
- The library fades in after a 0.1 s wait over 0.4 s, and out over 0.3 s.
- Hover tints and pops take 0.3 s.
- A toast fades in over 0.2 s, holds 2 s and fades out over 1 s.
- The intro animation values serialized on `MainProcedure` (`homeUIFadeTime`,
  `homeUIMoveShifting` and so on) are never read by this build.

**Style.**

- Panels are near-black: #19191a on a rounded 9-slice sprite with a corner radius of about
  0.4 degrees. Popovers are #1e1e1e to #262627, the toast #303030.
- One warm accent: amber #ffd570 for selection and slider handles (hover #fff1c7, pressed
  #dfab2c), and #ffe090 for primary buttons and the selected tab.
- Round buttons swap from a dark disc to an amber disc on hover, and their icon turns dark.
- Setting rows are outlined at white alpha 0.03, which is invisible on the Go.
- Soft shadow sprites give depth.

## 8. Skybox: rendering and its cost

**The menus are in the eye buffers.** Every canvas renders in the ordinary eye cameras,
with UI shaders that write no depth. The pointer visuals are drawn last with the depth test
off, so they always sit on top.

On an Oculus Go, `MainProcedure` sets `XRSettings.eyeTextureResolutionScale = 1.4` (1.2 if
the file `/SKYBOX/s0.a` exists). That is about twice the eye-buffer pixels. The active
quality level uses 4x MSAA, and `useRecommendedMSAALevel` is off. So Skybox buys crisp
eye-buffer text with roughly twice the fill for everything it draws, rooms and picture
included. It was not run for this study, so that cost is inferred, not measured. For
comparison, our rooms at 1024x1024 with 4x MSAA already cost 7.5 to 8.6 ms of `App=`
(`vr-environments.md` §8).

**A compositor UI layer exists but is switched off.** `OVROverlayCreator` gates every
overlay path behind a static property hard-coded to `false`. With it false, `Start` adds the
overlay's Unity layers to the eye cameras and never instantiates `UIPrefabs/OverlayMainRoot`.

The dormant design:

- A disabled `OVROverlay` of type Overlay and shape Cylinder, scaled 8.3625 x 3.1875 x 5.8
  (the home centre panel).
- An orthographic UI camera rendering into a RenderTexture sized at twice the Go's display
  density: 4699x1582 ARGB32 with depth and automatic mipmaps, about 30 MB (40 MB with
  mips).
- `OVROverlay` forces a render-texture source to "dynamic", which means a full copy into the
  layer's swapchain every frame, and a helper adds a second blit per frame. The layer is
  created with one mip level, so the mipmaps would be wasted.
- Its two halves disagree on the cylinder radius: 2.9 m in the helper script, 5.8 m in
  Oculus's layer.
- To show the pointer over the layer, it would move the cursor into the UI camera and bake
  it into the texture. A ray-to-cylinder unroll for hit testing exists too, with no callers.

`vr-environments.md` §4 (PLE-603) read this prefab as the live UI path; it is not.

**The only live compositor layer is for 360 video.** It is an equirect layer fed from an
Android surface. It is used only when the "360 Pro-HD Quality" setting is on (default off),
the video is plain 360 and no menu, dialog, keyboard or tip is on screen. So Skybox never
composes UI and a layer together.

## 9. What VrApi 1.35 gives us

From the SDK headers PLE-617 fetched (`build/sdk/ovr_sdk_mobile/VrApi/Include/` of the
workspace, outside git) and Meta's samples in the same SDK.

**Layers.** `vrapi_SubmitFrame2` takes up to 16 layers (`ovrMaxLayerCount`), composited in
submission order by TimeWarp with the newest head pose. The types are projection,
cylinder, cube, equirect, loading icon and fish-eye (`VRAPI_LAYER_TYPE_*2`); there is no
quad type. A flat quad is a projection layer whose `TexCoordsFromTanAngles` comes from
`ovrMatrix4f_TanAngleMatrixFromUnitSquare(view * model)`; the header calls this the path
for "high quality movie screens and user interface planes", and Meta's VrCinema sample
submits its movie screen that way. A cylinder layer maps the view direction onto a
hemicylinder ("hard-coded to 180 degrees around and 60 degrees vertical FOV"), then
`TextureMatrix` and `TextureRect` pick the sub-rectangle; the VrCompositor sample sets the
arc from a density in texels per full turn (arc = 360 degrees x texture width / density)
and the height from the model matrix scale, so texels stay square. Its texture needs a
transparent border because `TextureRect` clamping "makes no guarantees" outside the rect.
Blending is per layer (`SrcBlend`/`DstBlend`: one, zero, source alpha, one minus source
alpha), so a panel can sit over the eye buffer (overlay) or under it with a hole in the
eye buffer's alpha (underlay, what VrCinema does for its movie).

**Surfaces the compositor latches itself.** `vrapi_CreateAndroidSurfaceSwapChain(w, h)`
returns a swapchain backed by a SurfaceTexture that VrApi owns, and
`vrapi_GetTextureSwapChainAndroidSurface` its `android.view.Surface`. Whatever a producer
posts to that Surface (a decoder, `Surface.lockHardwareCanvas()`, a `VirtualDisplay`) is
latched by the compositor at TimeWarp time: the app's GL thread uploads nothing and draws
nothing for it. PLE-614 considered this for the picture; for UI it removes the
`texImage2D` upload §1 does today.

**Go controller input** (`VrApi_Input.h`). The Go remote is an
`ovrControllerType_TrackedRemote` with `ovrControllerCaps_ModelOculusGo`, orientation
tracking and no position tracking. Per frame, `vrapi_GetCurrentInputState` gives:

| Field | Go meaning |
| --- | --- |
| `ovrButton_A` | trigger pulled ("Set for trigger pulled on the Gear VR and Go Controllers") |
| `ovrButton_Enter` | touchpad click |
| `ovrButton_Back` | Back, only a short press: set for one frame when Back comes up within 0.25 s; long presses belong to the system |
| `TrackpadStatus` | a finger on the touchpad |
| `TrackpadPosition` | raw absolute touch position, 0 to `TrackpadMaxX`/`TrackpadMaxY` from the capabilities |
| `RecenterCount` | rises each time the remote is recentred (long press of the Oculus button) |

Home, volume and the Oculus button never reach the app. `vrapi_GetInputTrackingState`
(device, predicted display time) returns the remote's pose for the same predicted time the
frame is rendered for; Meta's VrInput sample takes its laser origin and direction straight
from that pose, casts 10 m, stops the beam 2.5 cm short of a hit, draws a 3.2 cm wide textured
beam and a 0.1-scale sprite at the hit, and hides the gaze cursor while the remote is
active. Feedback cannot lean on vibration: `vrapi_SetHapticVibrationSimple` exists, but only
for a remote reporting `ovrControllerCaps_HasSimpleHapticVibration`, which the toolkit must
check instead of assuming.

**Meta's own sample GUI** (the SDK's `SampleFramework/Src/GUI`, drawn into the eye buffers)
places menus 1.45 m from the eyes at 500 texels per metre, which is 12.7 texels per degree
at that distance.

**What a layer costs on the Go.** VrApi logs one stats line a second with the compositor's
own GPU time (`TW=`) and the layer count (`LCnt=`). Earlier captures under the workspace's
`build/` hold thousands of them. The table groups the 72 Hz lines by the GPU clock the
runtime chose:

| Who | Layers | GPU clock | `TW=` median | Lines | Source |
| --- | ---: | ---: | ---: | ---: | --- |
| Pleikkari cinema | 1 | 315 MHz | 1.70 ms | 1,300 | `build/ple-623/go/run6-preview/`, `build/captures/ple654/`, `build/ple-702/` |
| Pleikkari cinema | 1 | 401 MHz | 1.49 ms | 1,210 | same |
| Pleikkari cinema | 1 | 510 MHz | 1.36 ms | 726 | same |
| Oculus Home (`com.oculus.vrshell`) | 2 | 315 MHz | 1.79 ms | 67 | `build/captures/ple668-preview/`, `build/ple-702/live/` |
| Oculus Home | 3 | 315 MHz | 2.54 ms | 121 | `build/ple-702/*/stream/start-logcat.txt` |

Oculus Home submits two or three layers at 72 FPS with no stale frames and `App=` 0.87 ms.
Its layers are not ours (sizes and types unknown), so read the table as
"an extra layer costs the compositor between 0.1 and 0.75 ms of GPU time at 315 MHz", not
as a price list; PLE-722 measures its own. The compositor's time is not the app's `App=`,
but both share the GPU inside one 13.9 ms refresh.

**Eye-buffer resolution.** The eye buffers are 1024x1024
(`VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_*`). If the eye field of view is VrApi's default
90 degrees (the logs do not print it), the eye buffer holds about 9 texels per degree at
the centre of view and 18 at the edges, because the projection is planar. The Go's display
has 1280 pixels per eye; at 90 to 100 degrees across that is 12.8 to 14.2 pixels per degree
on average, and more at the centre. A panel drawn into the eye buffer is therefore
resampled twice (texture to eye buffer, eye buffer to display) and holds about 9 texels per
degree where the user looks. A layer is sampled once, straight to the display. This is the
gap Skybox closes by rendering 1.4 times larger eye buffers (§8).

## 10. The toolkit spec for Pleikkari

PLE-722 builds the toolkit and its first screen, the in-stream menu. The other screens
(§10.8) follow in their own tickets. Every number here is a starting value to check on the
Go, not a measured optimum.

### 10.1 Scope

- **Go only.** The toolkit runs only inside `StreamVrActivity`, which already requires
  `GoVrSupport.available()`: `Build.DEVICE == "pacific"`, an arm64 process, the VR libraries
  loaded, and the native cinema setting on. Its Kotlin lives in the `vr` source set and its
  native part in `src/vr/cpp`, next to the cinema. Phones never load it.
- **Unit tests.** The gate compiles the `vr` Kotlin into the unit tests even without the
  SDK, so layout, focus navigation, hit mapping and keyboard state belong in plain classes
  with JVM tests.
- **The flat fallback stays.** The Oculus TV screen (`GoVrSupport.panelIntent`, the 2D app)
  stays reachable only as a fallback. It is the last row of Home and of Settings, as the
  chooser's last row is today, and it opens automatically when the cinema cannot start
  (`cinemaFailed`). Flows that need a web page (PSN sign-in) stay there.
- **Rule 4.** The operator's ruling makes the VR UI the product on the Go. PLE-722 should
  still keep today's strip menu selectable through a Go-only switch until its head-loop A/B
  (§10.9) is in, so the two can be measured against each other. The switch's default is the
  operator's call. It needs its `flag-gate-allowlist.txt` entry if it defaults off.

### 10.2 Rendering path

Three separate choices: where the panel is composed, who draws its pixels, and how the
pointer shows over it.

**Composition: compositor layers, not eye-buffer quads.**

| | Layer (cylinder, or quad = projection + unit square) | Quad in the eye buffer (Skybox's way) |
| --- | --- | --- |
| Sharpness | Sampled once, at display density | Resampled twice. About 9 texels per degree at the centre of a 1024 eye buffer, unless the eye buffer grows (Skybox: 1.4x, about 2x the fill) |
| App GPU | Nothing per frame for the panel itself | The panel's fill in both eyes every frame, plus MSAA if a room is on |
| Compositor GPU | About 0.1 to 0.75 ms `TW=` per extra layer at 315 MHz (§9) | Nothing extra |
| Dropped app frame | Recomposed from its own texture at display rate, whatever the app frame does | Reprojected from the last eye buffer, like the room |
| Curvature | A cylinder is free and viewer-centred, so every glyph is at the same distance and focus | Needs a tessellated mesh |
| Cost of a closed menu | Nothing: no layer submitted | Nothing |

Layers win on the two things the operator ranked first: crisp text, and not taxing the head
loop. Skybox's eye-buffer route only reaches its sharpness by doubling the eye-buffer
pixels, which our rooms cannot afford (`vr-environments.md` §8). Wide panels (30 degrees or
more) are cylinder layers; small flat things (a toast, the stats readout) can be quads.

**Pixels: our own small Kotlin toolkit drawing with `Canvas` into a VrApi Android-surface
swapchain.**

| Producer | For | Against |
| --- | --- | --- |
| **A. Kotlin widgets on `Canvas`** (recommended). `Surface.lockHardwareCanvas()` (API 23+; the Go is API 25) on the Surface of `vrapi_CreateAndroidSurfaceSwapChain` | No GL-thread work and no uploads (the compositor latches the Surface). System fonts, `StaticLayout` and every locale, as `uploadText` already uses. Full control of VR states (hover, press, focus ring). Layout, focus and hit-testing logic is plain Kotlin, unit-testable on the JVM. Redraws only when something changed | We write layout and about a dozen widgets, plus scrolling |
| B. Android Views on a private `VirtualDisplay` plus `Presentation`, rendering into the same Surface | Real layouts, `RecyclerView` scrolling, Android focus navigation for the pad, accessibility, possible reuse of phone screens | Hover and touch `MotionEvent`s must be synthesised from ray hits. `EditText` would raise the system IME on display 0, where the Go shows nothing, so we need our own keyboard anyway. Dialogs and popups from reused code open on the wrong display. HWUI redraws of a large invalidated tree run on the GPU outside our control. Phone screens are not VR-native, which is what the operator asked for |
| C. Native GL widgets in C++ (glyph atlas or SDF text) | One thread, deterministic | Text shaping, fonts and i18n from scratch, or a bundled font; UI strings and logic live in Kotlin; per-frame GL work on the head-loop thread |

Option B is the escape hatch if a large flat screen (all of Settings) ever has to come into
VR unchanged. It is not the base.

**Pointer over a layer: panels as underlays, pointer in the eye buffer.** A layer
submitted after the eye buffer covers everything in the eye buffer, so a reticle drawn
there would vanish over the panel. Skybox's dormant design moved the cursor into the panel
texture, which means a redraw per pointer move and a frame of lag. Instead:

1. Submit the panel layers first, back to front, and the projection layer last. Panel
   textures come out of `Canvas` premultiplied and blend `ONE` / `ONE_MINUS_SRC_ALPHA`.
2. While any panel is open, the projection layer blends `ONE` / `ONE_MINUS_SRC_ALPHA`, so the
   result is the eye buffer plus what is below it times one minus the eye buffer's alpha.
   With no panel open it stays `ONE` / `ZERO`, as today.
3. After the room and the picture, draw each open panel's footprint: the same cylinder
   section, a few dozen triangles. Use
   `glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ZERO)`, with source
   alpha set to the panel's own alpha. That alpha is the analytic rounded rectangle `Canvas`
   fills, times the panel's opacity and its fade. Across the footprint the eye buffer's
   colour becomes scene times one minus alpha, and its alpha becomes 0. The compositor then
   adds the whole premultiplied panel on top of exactly the part of the scene that should
   show through it. Outside the footprints nothing lies below, so the eye buffer's alpha
   there does not matter.
4. Draw the laser and the reticle last, depth test off, with
   `glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`.
   Their colour and alpha stay premultiplied, so they cover the panel below in proportion.

The result:

- The pointer is drawn in the same frame as the controller pose it comes from.
- A translucent panel shows the picture through it correctly, including its anti-aliased
  corners.
- The menu adds one layer.

A picture layer (PLE-614) would change step 3: the eye buffer's alpha would also have to be
0 over the picture, and the panels would stack between the two.

The fallback, if the underlay's alpha misbehaves on the Go: overlay panels plus a reticle as
its own small quad layer, a static disc texture whose pose is set per frame. That costs one
more layer and still needs no redraw.

A fade scales the layer header's `ColorScale` (all four channels, since the panel is
premultiplied) and the footprint's alpha by the same factor, per frame, so it needs no
redraw.

**Practicalities for PLE-722's first commit:**

- Give every panel texture a 2-texel transparent border (cylinder `TextureRect` clamping).
- Set `VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER` like the projection layer, so Canvas
  colours match.
- Draw an asymmetric test pattern first, to settle the Surface's orientation and alpha.
- Give each panel its own swapchain, sized to the panel at 16 texels per degree (§10.3).

### 10.3 Panel geometry and placement

**Shape.** A panel is a section of a viewer-centred cylinder, so the radius is the distance
and every point is equally far.

- The default radius is 2.0 m, but never more than 80% of the picture's distance and never
  under 0.6 m. The menu must sit in front of the picture it covers, or stereo and occlusion
  disagree (the screen-distance setting allows 0.8 to 12 m).
- Skybox's 4.6 to 5.8 m menus sit in front of a 17.6 m picture; ours is at 3 m.

**Density.** 16 texels per degree in both directions, which is 5,760 texels per turn in the
VrCompositor sample's terms. That is at or a little above the Go's display density
(12.8 to 14.2 pixels per degree on average, more at the centre), so the compositor
minifies slightly and never magnifies. PLE-722 should A/B 24 texels per degree with
`VRAPI_FRAME_LAYER_FLAG_FILTER_EXPENSIVE` if text looks soft.

**Sizes.**

| Panel | Size | Texels |
| --- | --- | --- |
| Sheet (a main panel) | up to 56 x 32 degrees | 896x512 |
| In-stream menu | 50 x 28 degrees | 800x448 |
| Keyboard | 48 x 17 degrees | 768x272 |
| Toast | 24 x 3.6 degrees | 384x58 |

Skybox's single panels are 45 to 49 degrees wide and 19 to 31 degrees tall. Anything wider
than 56 degrees becomes side panels, as in Skybox, not a wider sheet.

**Placement.**

- A panel opens centred 6 degrees below the gaze, at the head's yaw, with its pitch clamped
  to +-25 degrees. It then stays world-locked; it is summoned, not head-locked. Today's menu
  strip follows the head rigidly, which makes a laser useless and should go.
- A panel placed above or below the horizon is rotated about the eye by its pitch, so it
  faces the eye.
- Panels are placed again on every runtime recentre (`VRAPI_SYS_STATUS_RECENTER_COUNT`
  rising, as the screen already is since PLE-702) and on the Recentre item.
- Frequent targets stay within +-25 degrees of yaw and -25 to +15 degrees of pitch.
- The keyboard opens under its form. The form moves up so that both fit within that range.

### 10.4 Typography and style

**Type.** Roboto, the Go's system sans (`Typeface.DEFAULT` and `sans-serif-medium`, Apache
2.0); CJK falls back to the system Noto. We bundle no font. Roboto's cap height is 0.711 em
against Calibri's 0.638, and its regular weight is lighter than Skybox's Calibri Bold, so
labels use Medium.

| Role | Em | Cap height | Texels at 16 per degree |
| --- | ---: | ---: | ---: |
| Title | 1.6 degrees | 68' | 26 |
| Label, body (Medium) | 1.2 degrees | 51' | 19 |
| Secondary, value | 1.05 degrees | 45' | 17 |
| Minimum, including the stats readout | 1.0 degree | 43' | 16 |
| Key labels | 1.9 degrees | 81' | 30 |
| PIN digits | 2.4 degrees | 102' | 38 |

Line height is 1.3 em. The body's 51' cap height is a little above Skybox's playback body
(47'), to make up for the lighter weight. Its 1.2-degree em is well under today's
1.9-degree strip text.

**Colour.** One theme with the app (PLE-247), taken from `colors.xml` rather than invented:

- The panel is `stream_control_dock_background` (#1C1B1F), drawn opaque.
- Rows are one step lighter, hover a second step, pressed a step darker.
- Text is off-white (about #ECEAF0) rather than pure white, which flares in the Go's Fresnel
  lenses. Secondary text is about #B5B1BA.
- Disabled is a solid mid-grey (about #7A7680), not an alpha fade: Skybox's alpha 0.19
  disabled states and alpha 0.03 outlines vanish on the Go.
- `accent` (#ffaaee) marks selection and the pad's focus ring; `stream_quit` (#FFB4AB)
  marks destructive actions.
- Every text colour must reach WCAG AA against its background, as the phone's layout guard
  requires.

**Shape.** Corner radius 0.5 degrees on panels and 0.35 degrees on rows. A focus ring is a
0.15 degree accent outline, shown only after pad input and hidden once the pointer moves.
Hover is a tint only, never a pop-out or scale, so hover redraws stay cheap and text never
moves.

**Motion.**

- A hover tint takes 120 ms, which is a handful of redraws of the panel.
- A panel open or close fades over 150 ms through the layer's `ColorScale` (§10.2), with no
  redraw.
- A page push or pop is a 150 ms cross-fade inside the panel.
- A toast fades in over 0.2 s, holds 2 s and fades out over 0.6 s.
- Nothing flies in from a distance (Skybox's PIN pad does).

### 10.5 Widgets

All sizes are in degrees at the panel.

| Widget | Spec |
| --- | --- |
| Sheet | A panel with a 4.5-degree header: title, a back chevron when nested, a close control. Owns the Back stack entry |
| Button | At least 3.6 tall and 8 wide. Primary, secondary and destructive styles. Activates on release |
| Icon button | 4.0 square, with a tooltip after 0.75 s of hover (as Skybox) |
| Row | 3.6 tall, full width: label on the left; value, chevron, switch or slider on the right. The whole row is the hit target |
| List | Vertical scroll of rows with 0.4 gaps, 1.5-degree fades at the clipped edges, a 0.4-wide scroll bar while scrolling. Pooled rows |
| Toggle | A row with a 3.6 x 2.0 switch; the row toggles it |
| Slider | A row with the value text, a 0.3-tall track, a 1.6 thumb and a 3.6-tall hit band. Minus and plus step buttons (3.6 square) at the ends, as Skybox has, because a 3DoF laser is imprecise. Snaps to steps |
| Choice | 2 to 6 segments in one row, each at least 7 wide (the room picker) |
| Dialog | A modal sheet with one to three buttons. Dims what is below it through `ColorScale` |
| Toast | Non-interactive, one line, bottom of view (§10.3) |
| Progress | A 2.5-degree spinner, or a determinate bar |
| Text field | One line with a caret and horizontal scroll, masked for secrets with a show toggle. Opens the keyboard (§10.7) |
| Stats readout | A small quad layer at the picture's top-left corner, redrawn once a second |

### 10.6 Input model

**Pointer.**

- Once per frame on the GoCinema thread, read the remote's pose with
  `vrapi_GetInputTrackingState` at the frame's own predicted display time. The same pose
  drives the hit test and the laser, so they never lag each other.
- No smoothing, and no arm model of our own: the runtime's pose already carries one, which
  is what both Skybox and Meta's VrInput sample rely on.

**Hit test.** Analytic, in each panel's frame (the inverse of its layer model matrix):

- Intersect the ray with the cylinder (take the far root, since the origin is inside) and
  check the panel's arc and height.
- Map the hit to `u = angle / arc + 0.5` and `v = 0.5 - height / panelHeight`, then to
  texels.
- The nearest panel wins. There is no length limit.
- The native side hands the toolkit a (panel, u, v) and the button and touchpad state every
  frame, through one small JNI call like today's `input()`. The toolkit maps that to a
  widget.

**Laser.**

- About 4 mm wide at the controller, running a quarter of the way to the hit (0.4 m with no
  hit), opaque for its first 10% and faded out by 90% (Skybox).
- White at alpha 0.6, drawn after the hole-punch.

**Reticle.**

- A disc 0.6 degrees across when idle over a panel, and 0.8 degrees over an interactive
  widget (a 100 ms tween).
- Constant angular size, at the hit point, facing the eye.
- Shown only while the ray is on a panel, and hidden 0.5 s after it leaves.

**Widget states.**

- Idle becomes hover when the ray enters the widget's rect. It leaves hover only 0.3
  degrees outside the rect, so the Go remote's jitter cannot flicker it.
- Select down while hovering is pressed. Select up while still inside activates, and
  outside cancels.
- Moving 3 degrees (Skybox's threshold, measured from the head) turns a press into a drag.
  That cancels the activation, unless the widget is a slider or a list, which take the drag.
- Disabled widgets take hover but not press.

**Buttons.**

| Action | Go remote | Bluetooth pad |
| --- | --- | --- |
| Point | the remote's orientation | (focus instead of a pointer) |
| Select | trigger (`ovrButton_A`) or touchpad click (`ovrButton_Enter`), on release | A / Cross, `DPAD_CENTER`, `ENTER` |
| Back (pop the stack: keyboard, dialog, sub-page, then close the menu) | Back, short press | B / Circle, `BACK` |
| Move focus | (none) | D-pad or left stick: 0.5 threshold, repeat after 400 ms every 150 ms, nearest focusable in that direction |
| Scroll a list | touchpad drag while hovering (below), or drag with select held | right stick; L1/R1 page |
| Adjust a slider | drag with select held, the step buttons, or touchpad left/right while hovering | D-pad left/right on the focused slider |
| Open the in-stream menu | Back (as today); also a click with no panel open (Skybox's "click shows the controls") | see the conflict below |
| Recentre | long Oculus button (the runtime; panels and screen follow `RECENTER_COUNT`), a long Back if the Go reports one, the Recentre item | the Recentre item |

Today a touchpad click with the menu closed recentres the screen. With a pointer, recentring
moves to the Recentre item, the long Oculus button and a long Back, so a stray click can no
longer move the screen.

**Touchpad scrolling** is our addition; Skybox scrolls lists only by pointer drag.

- While the ray is on a scrollable list, a finger dragged on the touchpad scrolls it 1:1: a
  full pad height scrolls one viewport height.
- On release, the drag's velocity carries on with friction of 5 per second (the value in
  Meta's sample `ScrollManager`).
- Normalise positions with `TrackpadMaxX`/`TrackpadMaxY`.
- A touchpad click that moved less than 15% of the pad is a click, not a drag.

**Skybox habits we leave out.** No 0.3 s wait on every single click for a possible double
click. No half-pad threshold before scrolling starts. No swipe events that never fire on the
Go.

**Long Back.**

- VrApi's header says `ovrButton_Back` reports only presses shorter than 0.25 s, with long
  presses left to the system.
- The Go also delivers Back to Android as `KEYCODE_BACK` with a down and an up.
  `StreamVrActivity.dispatchKeyEvent` swallows that today; it can time a long press
  (0.75 s, as Skybox does) instead.
- Whether a long Back reaches the app on the Go is to be checked there (§11).

**The pad during a stream: a conflict to settle.** PLE-722's brief opens the menu with the
pad's Menu or Options key. But during a stream every pad key goes to the console, and the
default mappings send `BUTTON_START` to Options, `BUTTON_SELECT` to Share and `BUTTON_MODE`
to PS (`Preferences.mapping*`). Taking Options would break every game's pause menu.

- Proposal: hold Share and Options together for 0.8 s. Both still reach the console as
  normal presses, so no game input is lost; only the hold opens the menu.
- The alternative is a key no mapping uses.
- While the menu is open, the pad drives the menu and the console receives neutral input.
- This is the operator's call.

**Headless tests.**

- `adb shell input gamepad keyevent KEYCODE_BUTTON_A` injects with the gamepad source, which
  `StreamVrActivity.dispatchKeyEvent` checks for. A plain `input keyevent` is a keyboard
  source and would miss it. The `gamepad` source is not yet verified on the Go's Android
  7.1 (§11).
- The remote cannot be injected, so a debug-only property, for example
  `debug.pleikkari.vr_pointer` = yaw,pitch in degrees, should place a synthetic ray. That
  lets screencaps show hover, focus and press states. It needs a flag-gate allowlist entry,
  like `vr_full_pose`.
- The toolkit logs under tag `GoVrUi`: panel open and close, focus, hover, activation and
  value changes, and once a second its redraw count and redraw time.

### 10.7 Keyboard

Three layouts on one keyboard panel, each its own layer below its form:

- **PIN pad** (3x4: 1 to 9, clear, 0, backspace), for the console's login PIN, which today
  sends the user back to Oculus TV. Cells are 6 x 4.8 degrees.
  - A row of slots with underlines, the next one lit.
  - Clear and backspace are disabled while empty; OK is enabled once the length is right.
  - A wrong PIN turns the title `stream_quit` and gives it a short 0.3-degree shake (0.4 s).
- **Address pad**, for a manual console: digits, `.`, `:`, `-`, and an `abc` page for host
  names.
- **Text** (QWERTY in four rows plus a row with space and Done): 4.8 x 4.0 degree cells with
  0.3-degree gaps.
  - One-shot shift, and a lit caps lock on a double press.
  - Backspace repeats after 500 ms at 12 a second.
  - Done moves to the next field, and submits on the last one.
  - Secrets are masked.

Keys activate on release, with hover and press tints like any button. Nothing moves or
grows. On a pad:

| Pad button | Keyboard action |
| --- | --- |
| D-pad | moves between keys |
| A | types |
| X | backspace |
| Y | shift |
| B | closes the keyboard, as Back does |
| Start | Done |

### 10.8 Screens and hierarchy

As phone Home does since the UX work (`docs/design/UX.md` product rules), one console has
one primary action, Play, and states replace buttons.

| Screen | Contents | Ticket |
| --- | --- | --- |
| In-stream menu | Resume; Recentre; Room (a Choice of plain, void, cinema, terrace, plus lounge and hall when they land); screen distance and size (Sliders); Match 60 Hz (a Toggle, PLE-636); Stats overlay (a Toggle); Disconnect (destructive, at the bottom). Two columns: actions on the left, settings on the right | PLE-722 |
| Home (Library launch) | A card per linked console with its state (awake, rest mode, not found) and Play; Settings; the Oculus TV row. Replaces `GoVrLibraryFlow`'s text chooser | follow-up |
| Finding, waking, connecting | A status sheet with a Progress and Cancel, replacing today's status text | follow-up |
| Console login PIN | The PIN pad on `StreamStateLoginPinRequest` | follow-up |
| Settings | The Go subset, reached from the in-stream menu's Settings button (Back returns to the menu): room and screen, Match 60 Hz, stats; stream (resolution, frame rate, codec, bitrate with Auto), stored for the next stream; "Oculus TV screen" at the bottom left for the rest, which ends the VR task | PLE-732 |
| Add a console | The address pad; linking and PSN sign-in stay on the Oculus TV screen until a VR flow is designed | follow-up |
| Errors and quit reasons | A dialog with the reason and Retry or Home | follow-up |

### 10.9 Performance budget

The head loop comes first. Each PLE-722 proof reads VrApi's stats lines (`FPS`, `Prd`,
`Stale`, `TW`, `App`, `LCnt` and the GPU clock). It compares 60 s with the menu closed,
60 s open and idle, and 60 s open with the pointer moving, in the plain room and in the
cinema room. Medians are reported per GPU clock, because the runtime moves between 315 and
401 MHz by itself (PLE-666).

| Figure | Budget |
| --- | --- |
| FPS | unchanged: 72, or 60 with the match mode |
| Stale frames per second | the open-menu mean no more than 0.2 above the closed mean |
| `Prd` | median unchanged |
| `TW=` | no more than +0.8 ms per open panel at 315 MHz |
| `App=` | no more than +0.3 ms (pose read, hit test, footprint, laser, reticle) |
| Layers | 1 with nothing open, at most 3 with UI open (eye buffer, sheet, and one of keyboard or stats) |
| GoCinema thread | no Canvas, no texture upload, no wait on the toolkit; UI work under 0.2 ms a frame |
| Toolkit thread | redraws only on change; no more than 3 ms a redraw at 800x448; at most 60 a second, and only while a list scrolls |
| Memory | under 8 MB for all panel swapchains together |

### 10.10 Follow-up tickets this spec implies

- VR Home: console cards and status sheets in place of `GoVrLibraryFlow`'s text chooser.
- The PIN pad for the console login PIN (today the Go user is sent to Oculus TV).
- The Go Settings sheet, with the Oculus TV row for the rest.
- The address keyboard for a manual console.
- The stats readout as a small layer.
- A measurement ticket on the Go comparing:
  - a cylinder against a quad;
  - an underlay against an overlay with a reticle layer;
  - 16 against 24 texels per degree with `FILTER_EXPENSIVE`;
  - one against two open panels, reading `TW=` and `Stale`.

## 11. Open questions for the Go

- **Eye field of view.** The Go's suggested eye FOV is not in any log. PLE-722 should log
  `VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X/Y` once, to settle the eye-buffer density
  figure in §9.
- **Long Back.** Does a long press of Back reach the app, through `ovrButton_Back` or
  `KEYCODE_BACK` down and up? Skybox's Unity path acts on a 0.75 s hold.
- **Gamepad injection.** Does `input gamepad keyevent` on the Go's Android 7.1 produce
  `SOURCE_GAMEPAD` events?
- **Screencaps.** Does a display-0 screencap of the native cinema include compositor layers
  other than the projection layer? Today's screencaps only ever had one layer to show. If
  they do not, UI proofs need another capture path.
- **Canvas into the Surface swapchain.** A `Canvas`-drawn Surface in a VrApi Android-surface
  swapchain still needs its orientation, premultiplied alpha and sRGB handling checked with a
  test pattern (§10.2).
- **The pad's menu key** (§10.6) is the operator's decision.
