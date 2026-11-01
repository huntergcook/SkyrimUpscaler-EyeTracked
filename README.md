# Skyrim Upscaler VR - Eye Tracked

A fork of PureDark's Skyrim Upscaler VR (DLSS / FSR / XeSS) that adds eye-tracked foveation for the
Steam Frame:

- **Gaze-following DLSS area.** The foveated DLSS box follows where you look instead of sitting at the lens
  centre. Everything outside the box keeps the original mod's behaviour (Skyrim's TAA).
- **Optional gaze-following shading rings** (NVIDIA variable rate shading). Off in the shipped settings.
- **Gaze sources**, tried in order each frame:
  1. SteamVR's `IVRSystem::GetEyeTrackedFoveationCenter` (OpenVR 2.15+, `IVRSystem_026`). Needs eye tracking
     enabled on the headset and "Share eye tracking data to other apps on this PC" turned on in SteamVR's
     Steam Link settings.
  2. OSC over UDP (Steam Link's eye tracking share), as a fallback.

Gaze is read through the OpenVR vtable, so it works with Skyrim VR's old `openvr_api.dll`.

## Settings

`settings/SkyrimUpscaler.ini` is the configuration this was tuned with: DLSS quality, a 0.5 gaze-following box,
shading rings off. The eye tracking keys:

| Key | Meaning |
|---|---|
| `mEyeTracked` | Read gaze at all |
| `mGazeFollowDLSS` | Move the DLSS box with your gaze |
| `mGazeBoxRecenter` | How far gaze must move from the box centre (in box widths) before it re-centres. Each re-centre resets DLSS history for that eye |
| `mGazeDeadzone` | Ignore gaze jitter smaller than this (shading rings only) |
| `mFoveatedScaleX/Y` | DLSS box size, as a fraction of one eye |
| `mGazeOffsetX/Y` | Calibration nudge |
| `mEnableFixedFoveatedRendering` | Shading rings (coarse shading outside the gaze). Off here: it makes textures outside the gaze blocky |

## Building

GitHub Actions builds on every push to `main` (Windows, vcpkg, `cmake --preset skyrim`). The log and the
built DLL are published to the `ci-output` branch (`dist/SKSE/Plugins/`). Drop them into your mod's
`SKSE/Plugins/` folder with Skyrim closed.
