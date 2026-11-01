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
  2. OSC over UDP (Steam Link's eye tracking share), as a fallback. Listens on **UDP port 9015** by default
     (`mOSCPort` in the ini). In SteamVR > Settings > Steam Link, set the OSC port to Custom (out port 9015).

Gaze is read through the OpenVR vtable, so it works with Skyrim VR's old `openvr_api.dll`.

## Using it with FUS (Wabbajack modlist)

This is how it is set up with the FUS modlist (tested on the Cangar profile). It replaces the upscaler plugin from
FUS's DLSS mod, so keep that mod: it provides the DLSS files this plugin still needs.

**Before you start**
1. Use FUS's DLSS mod (section 1.2, "DLSS (Nvidia RTX)"). Leave it enabled.
2. On the Steam Frame, turn on eye tracking (VR settings > General).
3. On your PC, in SteamVR > Settings > Steam Link, turn on **Share eye tracking data to other apps on this PC**.
4. Close Skyrim if it is running.

**Install**
1. Get the plugin: open the `ci-output` branch of this repo and download `dist/SKSE/Plugins/SkyrimUpscaler.dll`
   (the `.pdb` next to it is optional). Also download `settings/SkyrimUpscaler.ini` from `main`.
2. In Mod Organizer 2, right-click the bottom of the mod list and choose **Create empty mod**. Name it
   `SkyrimUpscalerVR EyeTracked` and enable it.
3. Right-click the new mod > **Open in Explorer**. Create the folders `SKSE\Plugins` inside it and put the DLL and the
   ini there.
4. Make sure the new mod is **below** FUS's DLSS mod in the left pane, so its `SkyrimUpscaler.dll` wins. The bottom of the
   list is fine.
5. Launch Skyrim VR from Mod Organizer as usual.

**Check it works**
1. In the headset, press **End** to open the upscaler menu and tick **Follow Eye Gaze** and **Move DLSS Area With Gaze**
   (the ini already has them on).
2. The menu should say it is tracking gaze, and "Valid gaze frames" should keep counting up. The DLSS area (the
   cleaner image) should follow your eyes.
3. Box size is **Foveated Scale X/Y** in the menu (0.5 is the FUS default). Use **Gaze Offset X/Y** if the sharp spot
   sits a little off from where you look.

**If it doesn't work**
- *No gaze:* check both eye-tracking toggles above. If both are on and gaze still doesn't arrive, restart SteamVR,
  or reboot. This fixed it for the author.
- *Black box:* FUS's own tip applies. Press **End**, switch "Use TAA For Periphery" off and back on.
- *Textures outside your gaze look blocky:* untick **Enable Fixed Foveated Rendering** (the shading rings). It is off in
  the supplied ini.
- To go back to the original, disable the new mod. FUS's DLSS mod takes over again.

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
| `mOSCPort` | UDP port for the OSC fallback (default 9015) |
| `mEnableFixedFoveatedRendering` | Shading rings (coarse shading outside the gaze). Off here: it makes textures outside the gaze blocky |

## Building

GitHub Actions builds on every push to `main` (Windows, vcpkg, `cmake --preset skyrim`). The log and the
built DLL are published to the `ci-output` branch (`dist/SKSE/Plugins/`). Drop them into your mod's
`SKSE/Plugins/` folder with Skyrim closed.
