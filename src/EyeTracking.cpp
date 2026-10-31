#include "EyeTracking.h"

namespace
{
	// Matches vr::HmdVector2_t.
	struct HmdVector2
	{
		float v[2];
	};

	// "IVRSystem_026" from OpenVR SDK 2.15.6. GetEyeTrackedFoveationCenter is the 36th virtual
	// method of IVRSystem (vtable slot 35, zero-based); IVRSystem has no overloaded virtuals, so
	// MSVC's vtable order is the declaration order.
	constexpr const char* kIVRSystemVersion = "IVRSystem_026";
	constexpr int         kSlotGetEyeTrackedFoveationCenter = 35;

	using VR_GetGenericInterface_t = void*(__cdecl*)(const char* pchInterfaceVersion, int* peError);
	using GetEyeTrackedFoveationCenter_t = bool (*)(void* self, HmdVector2* pNdcLeft, HmdVector2* pNdcRight);

	// Retry roughly every few seconds if SteamVR wasn't ready yet.
	constexpr uint32_t kRetryFrames = 300;

	// NDC (x right, y up, [-1, 1]) for one eye -> normalized position in the combined target.
	void NdcToCombined(const HmdVector2& ndc, float eyeOffsetX, float& outX, float& outY)
	{
		float ex = std::clamp(ndc.v[0] * 0.5f + 0.5f, 0.0f, 1.0f);
		float ey = std::clamp(0.5f - ndc.v[1] * 0.5f, 0.0f, 1.0f);
		outX = eyeOffsetX + ex * 0.5f;
		outY = ey;
	}
}

const char* EyeTracking::GetStateText() const
{
	switch (mState) {
	case State::kNotStarted:
		return "Not started";
	case State::kNoOpenVR:
		return "OpenVR not found (is SteamVR running?)";
	case State::kInterfaceMissing:
		return "SteamVR too old: no eye-tracked foveation API (IVRSystem_026)";
	case State::kNoGaze:
		return "No gaze: turn on eye tracking on the headset and SteamVR > Steam Link > Share eye tracking data";
	case State::kTracking:
		return "Tracking gaze";
	}
	return "Unknown";
}

void EyeTracking::TryInit()
{
	HMODULE openvr = GetModuleHandleW(L"openvr_api.dll");
	if (!openvr) {
		mState = State::kNoOpenVR;
		return;
	}
	auto getInterface = reinterpret_cast<VR_GetGenericInterface_t>(GetProcAddress(openvr, "VR_GetGenericInterface"));
	if (!getInterface) {
		mState = State::kNoOpenVR;
		logger::warn("[EyeTracking] openvr_api.dll has no VR_GetGenericInterface export");
		return;
	}
	int   error = 0;
	void* system = getInterface(kIVRSystemVersion, &error);
	if (!system || error != 0) {
		mState = State::kInterfaceMissing;
		logger::warn("[EyeTracking] {} not available from the SteamVR runtime (error {}); eye-tracked foveation disabled", kIVRSystemVersion, error);
		return;
	}
	mSystem = system;
	mState = State::kNoGaze;
	logger::info("[EyeTracking] Got {} from SteamVR; waiting for valid gaze", kIVRSystemVersion);
}

bool EyeTracking::Poll(float& leftX, float& leftY, float& rightX, float& rightY)
{
	if (!mSystem) {
		if (mFramesUntilRetry > 0) {
			--mFramesUntilRetry;
			return false;
		}
		TryInit();
		if (!mSystem) {
			mFramesUntilRetry = kRetryFrames;
			return false;
		}
	}

	auto vtable = *reinterpret_cast<void***>(mSystem);
	auto getCenter = reinterpret_cast<GetEyeTrackedFoveationCenter_t>(vtable[kSlotGetEyeTrackedFoveationCenter]);

	HmdVector2 ndcLeft{ { 0.0f, 0.0f } };
	HmdVector2 ndcRight{ { 0.0f, 0.0f } };
	if (!getCenter(mSystem, &ndcLeft, &ndcRight)) {
		if (mState == State::kTracking) {
			logger::info("[EyeTracking] Gaze lost after {} valid frames", mValidFrames);
		}
		mState = State::kNoGaze;
		if ((++mInvalidFrames % 5000) == 1) {
			logger::info("[EyeTracking] No valid gaze yet ({} frames). Check that eye tracking is enabled on the headset and shared with PC apps in SteamVR's Steam Link settings.", mInvalidFrames);
		}
		return false;
	}

	NdcToCombined(ndcLeft, 0.0f, leftX, leftY);
	NdcToCombined(ndcRight, 0.5f, rightX, rightY);
	mState = State::kTracking;
	++mValidFrames;
	if (!mLoggedFirstGaze) {
		mLoggedFirstGaze = true;
		logger::info("[EyeTracking] First valid gaze: left NDC ({:.3f}, {:.3f}) right NDC ({:.3f}, {:.3f})", ndcLeft.v[0], ndcLeft.v[1], ndcRight.v[0], ndcRight.v[1]);
	}
	return true;
}
