#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Provides per-eye gaze, converted to the combined side-by-side render target that Skyrim VR uses
// (left eye in x = [0, 0.5], right eye in x = [0.5, 1], y = 0 at the top).
//
// Two sources, tried in order each frame:
//  1. SteamVR's IVRSystem::GetEyeTrackedFoveationCenter (OpenVR SDK 2.15+, IVRSystem_026).
//  2. OSC over UDP (default port 9015), which is how Steam Link's "Share eye tracking data to other
//     apps on this PC" delivers the Steam Frame's gaze. VRChat-style eye tracking addresses are
//     understood (/tracking/eye/LeftRightPitchYaw, CenterPitchYaw, LeftRightVec, CenterVec).
//     Every distinct address received is logged once, so unknown formats show up in the log.
//
// Skyrim VR ships an old openvr_api.dll and CommonLibVR bundles old OpenVR headers, so this
// code does not include openvr.h; it calls the few IVRSystem methods it needs through the vtable.
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
		kNoGaze,             // no valid gaze from SteamVR and no recent OSC gaze
		kTrackingOpenVR,     // gaze from SteamVR's foveation API
		kTrackingOSC         // gaze from OSC (Steam Link eye tracking sharing)
	};

	// Call once per frame. Returns true and fills normalized combined-target centers when gaze is valid.
	bool Poll(float& leftX, float& leftY, float& rightX, float& rightY);

	State       GetState() const { return mState; }
	const char* GetStateText() const;
	uint64_t    GetValidFrames() const { return mValidFrames; }

	// OSC diagnostics for the settings menu.
	int         GetOSCPort() const { return mOSCPort; }
	bool        IsOSCListening() const { return mOSCListening.load(); }
	uint64_t    GetOSCPacketCount() const { return mOSCPackets.load(); }
	std::string GetOSCLastAddress();

	// Settings
	int   mOSCPort = 9015;
	bool  mEnableOSC = true;
	float mOffsetX = 0.0f;  // calibration nudges, normalized within one eye (applied to both eyes)
	float mOffsetY = 0.0f;

	void Shutdown();

private:
	EyeTracking() = default;
	~EyeTracking() { Shutdown(); }

	void TryInitOpenVR();
	void StartOSC();
	void OSCThread();
	void HandleOSCPacket(const char* data, int size, int depth);
	void HandleOSCMessage(const char* data, int size);
	bool ProjectDirection(int eye, float x, float y, float z, float& outX, float& outY);
	void LoadProjection();

	// OpenVR
	void*    mSystem = nullptr;     // vr::IVRSystem (any version; we only use stable vtable slots)
	bool     mHasFoveationAPI = false;
	uint32_t mFramesUntilRetry = 0;
	float    mProjRaw[2][4] = {};   // left, right, top, bottom tangents per eye
	bool     mHaveProjection = false;

	// OSC (written by the listener thread)
	std::thread           mOSCThread;
	std::atomic<bool>     mOSCRunning{ false };
	std::atomic<bool>     mOSCListening{ false };
	std::atomic<uint64_t> mOSCPackets{ 0 };
	uintptr_t             mOSCSocket = ~uintptr_t(0);
	std::mutex            mOSCMutex;
	bool                  mOSCHaveGaze = false;
	float                 mOSCDir[2][3] = {};  // unit gaze direction per eye: x right, y up, z forward (negative)
	int64_t               mOSCGazeTimeMs = 0;
	std::string           mOSCLastAddress;
	std::vector<std::string> mOSCSeenAddresses;

	State    mState = State::kNotStarted;
	uint64_t mValidFrames = 0;
	uint64_t mInvalidFrames = 0;
	bool     mLoggedFirstGaze = false;
};
