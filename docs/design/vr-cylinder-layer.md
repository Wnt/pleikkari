# VrApi cylinder layer for the stream picture (Oculus Go)

PLE-614 is a design study with no code change. It asks whether the stream picture in the native VrApi
cinema (PLE-602, `android/app/src/vr/cpp/vr-cinema.cpp`) should become a
`VRAPI_LAYER_TYPE_CYLINDER2` compositor layer, with PLE-603's environment left in the
eye buffer underneath. Facts below come from the Go-compatible SDK headers obtained by
PLE-617 (`build/sdk/ovr_sdk_mobile/VrApi/Include/`, outside git).

## Today's path

1. The decoder writes into a `Surface` built on a `SurfaceTexture` whose OES texture is
   `Cinema::video` (`StreamVrActivity.kt`, GL thread).
2. Each app frame calls `updateTexImage()`, then `draw()` renders the curved strip into
   each eye's `RGBA8` swapchain at the suggested eye size. PLE-603's environment samples
   the same OES texture for its screen and its 8x8 glow map.
3. TimeWarp then resamples the eye buffer with distortion and chromatic aberration.

So the picture is filtered twice: once into the eye buffer and once by TimeWarp. It also
reaches the display only at app frame rate, one app frame after it was latched.

## What the cylinder layer would change

- **Producer:** `vrapi_CreateAndroidSurfaceSwapChain(w, h)` (or `...2(w, h, false)`) returns
  a swapchain backed by a SurfaceTexture that VrApi owns. `vrapi_GetTextureSwapChainAndroidSurface`
  returns its `jobject Surface`, which is handed to `session.attachToSurface()` in place of
  today's `Surface(consumer)`. The app no longer calls `updateTexImage()`. The compositor
  latches the newest decoder buffer.
- **Quality:** TimeWarp samples the decoder buffer once, straight from the source texels.
  That removes the eye-buffer resample, which blurs the picture most on the Go's 1280x1440-per-eye panel.
- **Latency:** the picture should update at TimeWarp's latch point, not one app frame
  later. This could save up to about one 72 Hz frame (about 14 ms) when the app frame is
  late. Nobody has measured this. It needs the Go and a capture.
- **Submission:** `frame.LayerCount = 2`. Layer 0 is today's projection layer (environment,
  or black, plus the menu or message strip). Layer 1 is the cylinder, with
  `SrcBlend = ONE, DstBlend = ZERO` inside its rect. `HeadPose` is set from `tracking`, as
  the projection layer already does.

## Hard parts (why this is not a drop-in)

1. **Geometry is fixed by the runtime.** The header says the cylinder mapping is
   "hard-coded to 180 degrees around and 60 degrees vertical FOV". Size, distance and
   recentring must be expressed through `TexCoordsFromTanAngles` (built from the
   `screen` model matrix times the eye view) and `TextureMatrix`/`TextureRect`, so that
   the picture occupies only a subrect of the hemicylinder. Today's `Segments` strip
   curvature and PLE-603's screen distance setting would have to match that radius. If
   they do not, the environment's frame and the picture separate as the head moves.
2. **Transparent border.** `TextureRect` clamping "makes no guarantees" outside the rect,
   so the source needs a transparent or black border. A decoder buffer has none. Shrink
   the rect by one texel, or accept edge smear.
3. **Colour and orientation.** The SurfaceTexture transform matrix (crop, flip) is no
   longer applied by our shader. It must be folded into `TextureMatrix`, and the app
   cannot read it, because VrApi owns the SurfaceTexture. Decoder crop padding (for example
   1920x1088) would then show. It has to be handled in `TextureRect`, from the codec's
   output format. Also check that the sRGB flag handling
   (`VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER` today) is still correct for a video layer.
4. **PLE-603 glow map loses its input.** `pleikkari_vr_environment_begin_frame` samples
   `video`. With a VrApi-owned SurfaceTexture, the app has no OES texture to sample. The
   options are: glow off in cylinder mode, a second consumer (not possible, because one
   Surface has one consumer), or sampling the swapchain handle, which is undocumented for
   Android-surface chains. Plan on glow off, or a constant tint.
5. **Menu over the picture.** The menu follows the head and is drawn in the projection
   layer, which sits *under* the cylinder. While the menu is open, either drop the
   cylinder layer (showVideo=false already switches to the message texture) or add a
   third layer.
6. **Frame pacing telemetry.** The app no longer knows when a new frame arrived
   (`newFrame` comes from `onFrameAvailable`). Any per-frame stats the VR path logs would
   need the codec's release callback instead.

## Recommendation

Worth doing, but only as a setting (`vr_picture_layer = eye_buffer | cylinder`, default
`eye_buffer`, AGENTS.md rule 4). It must be proven on the Go by someone wearing it:
sharpness side by side, plus logcat `vrapi` stats for TimeWarp misses. Nothing in this
note has run on a Go. Implementation estimate: 5 points (JNI for the Surface, Kotlin
surface routing, layer maths, glow fallback, menu handling).
