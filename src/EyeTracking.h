#pragma once

#include <cstdint>

// Reads per-eye gaze from SteamVR (IVRSystem::GetEyeTrackedFoveationCenter, OpenVR SDK 2.15+)
// and converts it to the combined side-by-side render target that Skyrim VR uses
// (left eye in x = [0, 0.5], right eye in x = [0.5, 1], y = 0 at the top).
//
// Skyrim VR ships an old openvr_api.dll and CommonLibVR bundles old OpenVR headers, so this
// file deliberately does not include openvr.h. It asks the runtime for the newer interface
// version by name and calls the one method it needs through the interface's vtable.
class EyeTracking
{
public:
	static EyeTracking* GetSingleton()
	{
		static EyeTracking singleton;
		return &singleton;
	}

	enum class State
	{
		kNotStarted,
		kNoOpenVR,           // openvr_api.dll not loaded or missing VR_GetGenericInterface
		kInterfaceMissing,   // SteamVR too old for IVRSystem_026
		kNoGaze,             // interface present, but no valid gaze (eye tracking off / not shared with PC apps)
		kTracking            // valid gaze this frame
	};

	// Call once per frame. Returns true and fills normalized combined-target centers when gaze is valid.
	bool Poll(float& leftX, float& leftY, float& rightX, float& rightY);

	State       GetState() const { return mState; }
	const char* GetStateText() const;
	uint64_t    GetValidFrames() const { return mValidFrames; }

private:
	EyeTracking() = default;

	void TryInit();

	void*    mSystem = nullptr;  // vr::IVRSystem (IVRSystem_026)
	State    mState = State::kNotStarted;
	uint32_t mFramesUntilRetry = 0;
	uint64_t mValidFrames = 0;
	uint64_t mInvalidFrames = 0;
	bool     mLoggedFirstGaze = false;
};
