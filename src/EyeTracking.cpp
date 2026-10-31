#include "EyeTracking.h"

#include <cmath>
#include <cstring>

namespace
{
	// Matches vr::HmdVector2_t.
	struct HmdVector2
	{
		float v[2];
	};

	// IVRSystem vtable slots. Slot 2 (GetProjectionRaw) has been stable across every IVRSystem
	// version; slot 35 (GetEyeTrackedFoveationCenter) exists from IVRSystem_026 (OpenVR SDK 2.15).
	// IVRSystem has no overloaded virtuals, so MSVC's vtable order is the declaration order.
	constexpr int kSlotGetProjectionRaw = 2;
	constexpr int kSlotGetEyeTrackedFoveationCenter = 35;

	using VR_GetGenericInterface_t = void*(__cdecl*)(const char* pchInterfaceVersion, int* peError);
	using GetProjectionRaw_t = void (*)(void* self, int eye, float* left, float* right, float* top, float* bottom);
	using GetEyeTrackedFoveationCenter_t = bool (*)(void* self, HmdVector2* pNdcLeft, HmdVector2* pNdcRight);

	constexpr uint32_t kRetryFrames = 300;
	constexpr int64_t  kOSCStaleMs = 250;  // drop OSC gaze older than this

	constexpr float kDegToRad = 0.01745329252f;

	int64_t NowMs() { return (int64_t)GetTickCount64(); }

	// NDC (x right, y up, [-1, 1]) for one eye -> normalized position in the combined target.
	void NdcToCombined(const HmdVector2& ndc, float eyeOffsetX, float& outX, float& outY)
	{
		float ex = std::clamp(ndc.v[0] * 0.5f + 0.5f, 0.0f, 1.0f);
		float ey = std::clamp(0.5f - ndc.v[1] * 0.5f, 0.0f, 1.0f);
		outX = eyeOffsetX + ex * 0.5f;
		outY = ey;
	}

	// ---- Minimal Winsock access, loaded at runtime so we don't fight the Windows.h/winsock headers ----
	struct SockAddrIn
	{
		short          family;
		unsigned short port;
		unsigned long  addr;
		char           zero[8];
	};
	using WSAStartup_t = int(__stdcall*)(unsigned short, void*);
	using socket_t = uintptr_t(__stdcall*)(int, int, int);
	using bind_t = int(__stdcall*)(uintptr_t, const void*, int);
	using recvfrom_t = int(__stdcall*)(uintptr_t, char*, int, int, void*, int*);
	using closesocket_t = int(__stdcall*)(uintptr_t);
	using setsockopt_t = int(__stdcall*)(uintptr_t, int, int, const char*, int);
	using WSAGetLastError_t = int(__stdcall*)();

	struct Winsock
	{
		bool              loaded = false;
		WSAStartup_t      WSAStartup = nullptr;
		socket_t          socket = nullptr;
		bind_t            bind = nullptr;
		recvfrom_t        recvfrom = nullptr;
		closesocket_t     closesocket = nullptr;
		setsockopt_t      setsockopt = nullptr;
		WSAGetLastError_t WSAGetLastError = nullptr;

		bool Load()
		{
			if (loaded)
				return true;
			HMODULE ws = LoadLibraryW(L"ws2_32.dll");
			if (!ws)
				return false;
			WSAStartup = (WSAStartup_t)GetProcAddress(ws, "WSAStartup");
			socket = (socket_t)GetProcAddress(ws, "socket");
			bind = (bind_t)GetProcAddress(ws, "bind");
			recvfrom = (recvfrom_t)GetProcAddress(ws, "recvfrom");
			closesocket = (closesocket_t)GetProcAddress(ws, "closesocket");
			setsockopt = (setsockopt_t)GetProcAddress(ws, "setsockopt");
			WSAGetLastError = (WSAGetLastError_t)GetProcAddress(ws, "WSAGetLastError");
			if (!WSAStartup || !socket || !bind || !recvfrom || !closesocket || !setsockopt || !WSAGetLastError)
				return false;
			alignas(8) char wsaData[1024]{};
			if (WSAStartup(0x0202, wsaData) != 0)
				return false;
			loaded = true;
			return true;
		}
	};
	Winsock g_ws;

	constexpr uintptr_t kInvalidSocket = ~uintptr_t(0);

	// ---- OSC helpers (big-endian, 4-byte aligned) ----
	uint32_t ReadU32(const char* p)
	{
		auto b = reinterpret_cast<const unsigned char*>(p);
		return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
	}
	float ReadF32(const char* p)
	{
		uint32_t u = ReadU32(p);
		float    f;
		memcpy(&f, &u, 4);
		return f;
	}
	// Returns the length including padding, or -1 if the string isn't terminated within size.
	int PaddedStringLength(const char* p, int size)
	{
		for (int i = 0; i < size; ++i) {
			if (p[i] == 0)
				return ((i + 1) + 3) & ~3;
		}
		return -1;
	}

	// VRChat convention: positive pitch looks down, positive yaw looks right (degrees).
	void PitchYawToDir(float pitchDeg, float yawDeg, float out[3])
	{
		float up = -pitchDeg * kDegToRad;
		float yaw = yawDeg * kDegToRad;
		out[0] = sinf(yaw) * cosf(up);
		out[1] = sinf(up);
		out[2] = -cosf(yaw) * cosf(up);
	}
	// VRChat/Unity vectors: +x right, +y up, +z forward. Ours: forward is -z.
	void UnityVecToDir(float x, float y, float z, float out[3])
	{
		float len = sqrtf(x * x + y * y + z * z);
		if (len < 1e-6f) {
			out[0] = 0;
			out[1] = 0;
			out[2] = -1;
			return;
		}
		out[0] = x / len;
		out[1] = y / len;
		out[2] = -z / len;
	}
}

const char* EyeTracking::GetStateText() const
{
	switch (mState) {
	case State::kNotStarted:
		return "Not started";
	case State::kNoOpenVR:
		return "OpenVR not found (is SteamVR running?)";
	case State::kNoGaze:
		return "No gaze: turn on eye tracking on the headset and SteamVR > Steam Link > Share eye tracking data";
	case State::kTrackingOpenVR:
		return "Tracking gaze (SteamVR)";
	case State::kTrackingOSC:
		return "Tracking gaze (OSC)";
	}
	return "Unknown";
}

std::string EyeTracking::GetOSCLastAddress()
{
	std::lock_guard lock(mOSCMutex);
	return mOSCLastAddress;
}

void EyeTracking::TryInitOpenVR()
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

	// Prefer the newest IVRSystem (has the foveation API); fall back to older ones for projection only.
	const char* versions[] = { "IVRSystem_026", "IVRSystem_022", "IVRSystem_021", "IVRSystem_020", "IVRSystem_019" };
	for (auto version : versions) {
		int   error = 0;
		void* system = getInterface(version, &error);
		if (system && error == 0) {
			mSystem = system;
			mHasFoveationAPI = (strcmp(version, "IVRSystem_026") == 0);
			logger::info("[EyeTracking] Got {} from SteamVR (foveation API: {})", version, mHasFoveationAPI);
			break;
		}
	}
	if (!mSystem) {
		logger::warn("[EyeTracking] No IVRSystem interface available from SteamVR");
		mState = State::kNoOpenVR;
		return;
	}
	LoadProjection();
	mState = State::kNoGaze;
}

void EyeTracking::LoadProjection()
{
	if (!mSystem)
		return;
	auto vtable = *reinterpret_cast<void***>(mSystem);
	auto getRaw = reinterpret_cast<GetProjectionRaw_t>(vtable[kSlotGetProjectionRaw]);
	for (int eye = 0; eye < 2; ++eye) {
		getRaw(mSystem, eye, &mProjRaw[eye][0], &mProjRaw[eye][1], &mProjRaw[eye][2], &mProjRaw[eye][3]);
	}
	mHaveProjection = (mProjRaw[0][1] - mProjRaw[0][0]) > 0.1f && fabsf(mProjRaw[0][3] - mProjRaw[0][2]) > 0.1f;
	logger::info("[EyeTracking] Projection L: l={:.3f} r={:.3f} t={:.3f} b={:.3f}  R: l={:.3f} r={:.3f} t={:.3f} b={:.3f}",
		mProjRaw[0][0], mProjRaw[0][1], mProjRaw[0][2], mProjRaw[0][3], mProjRaw[1][0], mProjRaw[1][1], mProjRaw[1][2], mProjRaw[1][3]);
}

// Direction in eye space (x right, y up, -z forward) -> normalized position within that eye's view.
bool EyeTracking::ProjectDirection(int eye, float x, float y, float z, float& outX, float& outY)
{
	if (z > -0.05f)
		return false;  // pointing sideways/backwards; ignore
	float tanX = x / -z;
	float tanYDown = -y / -z;
	float l = -1.19f, r = 1.19f, t = -1.19f, b = 1.19f;  // ~100 degree fallback
	if (mHaveProjection) {
		l = mProjRaw[eye][0];
		r = mProjRaw[eye][1];
		t = mProjRaw[eye][2];
		b = mProjRaw[eye][3];
	}
	outX = std::clamp((tanX - l) / (r - l), 0.0f, 1.0f);
	outY = std::clamp((tanYDown - t) / (b - t), 0.0f, 1.0f);
	return true;
}

void EyeTracking::StartOSC()
{
	if (mOSCRunning.load() || !mEnableOSC)
		return;
	if (!g_ws.Load()) {
		logger::warn("[EyeTracking] Could not load Winsock; OSC gaze disabled");
		mEnableOSC = false;
		return;
	}
	uintptr_t s = g_ws.socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 17 /*IPPROTO_UDP*/);
	if (s == kInvalidSocket) {
		logger::warn("[EyeTracking] OSC socket() failed ({})", g_ws.WSAGetLastError());
		mEnableOSC = false;
		return;
	}
	int reuse = 1;
	g_ws.setsockopt(s, 0xffff /*SOL_SOCKET*/, 4 /*SO_REUSEADDR*/, (const char*)&reuse, sizeof(reuse));
	DWORD timeoutMs = 500;
	g_ws.setsockopt(s, 0xffff /*SOL_SOCKET*/, 0x1006 /*SO_RCVTIMEO*/, (const char*)&timeoutMs, sizeof(timeoutMs));

	SockAddrIn addr{};
	addr.family = 2;
	addr.port = (unsigned short)(((mOSCPort & 0xff) << 8) | ((mOSCPort >> 8) & 0xff));
	addr.addr = 0;  // INADDR_ANY
	if (g_ws.bind(s, &addr, sizeof(addr)) != 0) {
		logger::warn("[EyeTracking] OSC bind to UDP port {} failed ({}). Is another app (e.g. VRChat) using it?", mOSCPort, g_ws.WSAGetLastError());
		g_ws.closesocket(s);
		mEnableOSC = false;
		return;
	}
	mOSCSocket = s;
	mOSCRunning = true;
	mOSCListening = true;
	logger::info("[EyeTracking] Listening for OSC eye tracking on UDP port {}", mOSCPort);
	mOSCThread = std::thread([this]() { OSCThread(); });
}

void EyeTracking::OSCThread()
{
	std::vector<char> buffer(65536);
	while (mOSCRunning.load()) {
		SockAddrIn from{};
		int        fromLen = sizeof(from);
		int        n = g_ws.recvfrom(mOSCSocket, buffer.data(), (int)buffer.size(), 0, &from, &fromLen);
		if (n <= 0)
			continue;  // timeout or error; loop checks mOSCRunning
		++mOSCPackets;
		HandleOSCPacket(buffer.data(), n, 0);
	}
	mOSCListening = false;
}

void EyeTracking::HandleOSCPacket(const char* data, int size, int depth)
{
	if (size < 4 || depth > 4)
		return;
	if (size >= 16 && memcmp(data, "#bundle", 8) == 0) {
		int pos = 16;  // "#bundle\0" + 8-byte time tag
		while (pos + 4 <= size) {
			int elementSize = (int)ReadU32(data + pos);
			pos += 4;
			if (elementSize <= 0 || pos + elementSize > size)
				break;
			HandleOSCPacket(data + pos, elementSize, depth + 1);
			pos += elementSize;
		}
		return;
	}
	HandleOSCMessage(data, size);
}

void EyeTracking::HandleOSCMessage(const char* data, int size)
{
	int addrLen = PaddedStringLength(data, size);
	if (addrLen <= 0 || data[0] != '/')
		return;
	std::string address(data);

	// Type tags
	const char* tags = "";
	int         pos = addrLen;
	if (pos < size && data[pos] == ',') {
		int tagLen = PaddedStringLength(data + pos, size - pos);
		if (tagLen <= 0)
			return;
		tags = data + pos + 1;
		pos += tagLen;
	}

	// Collect float/int args (others skipped)
	float args[16];
	int   numArgs = 0;
	std::string argText;
	for (const char* t = tags; *t && numArgs < 16; ++t) {
		if (*t == 'f' && pos + 4 <= size) {
			args[numArgs++] = ReadF32(data + pos);
			pos += 4;
		} else if (*t == 'i' && pos + 4 <= size) {
			args[numArgs++] = (float)(int32_t)ReadU32(data + pos);
			pos += 4;
		} else if (*t == 's' && pos < size) {
			int l = PaddedStringLength(data + pos, size - pos);
			if (l <= 0)
				break;
			pos += l;
		} else if (*t == 'T' || *t == 'F' || *t == 'N' || *t == 'I') {
			// no payload
		} else {
			break;
		}
	}
	for (int i = 0; i < numArgs && i < 6; ++i)
		argText += std::format("{}{:.3f}", i ? ", " : "", args[i]);

	float dir[2][3];
	bool  isGaze = false;
	if (address == "/tracking/eye/LeftRightPitchYaw" && numArgs >= 4) {
		PitchYawToDir(args[0], args[1], dir[0]);
		PitchYawToDir(args[2], args[3], dir[1]);
		isGaze = true;
	} else if (address == "/tracking/eye/CenterPitchYaw" && numArgs >= 2) {
		PitchYawToDir(args[0], args[1], dir[0]);
		memcpy(dir[1], dir[0], sizeof(dir[0]));
		isGaze = true;
	} else if (address == "/tracking/eye/LeftRightVec" && numArgs >= 6) {
		UnityVecToDir(args[0], args[1], args[2], dir[0]);
		UnityVecToDir(args[3], args[4], args[5], dir[1]);
		isGaze = true;
	} else if ((address == "/tracking/eye/CenterVec" || address == "/tracking/eye/CenterVecFull") && numArgs >= 3) {
		UnityVecToDir(args[0], args[1], args[2], dir[0]);
		memcpy(dir[1], dir[0], sizeof(dir[0]));
		isGaze = true;
	}

	std::lock_guard lock(mOSCMutex);
	mOSCLastAddress = address + " (" + argText + ")";
	if (mOSCSeenAddresses.size() < 64 && std::find(mOSCSeenAddresses.begin(), mOSCSeenAddresses.end(), address) == mOSCSeenAddresses.end()) {
		mOSCSeenAddresses.push_back(address);
		logger::info("[EyeTracking] OSC address seen: {} tags=,{} args=({}){}", address, tags, argText, isGaze ? " [used as gaze]" : "");
	}
	if (isGaze) {
		bool bad = false;
		for (auto& e : dir)
			for (float c : e)
				bad |= std::isnan(c);
		if (!bad) {
			memcpy(mOSCDir, dir, sizeof(dir));
			mOSCHaveGaze = true;
			mOSCGazeTimeMs = NowMs();
		}
	}
}

bool EyeTracking::Poll(float& leftX, float& leftY, float& rightX, float& rightY)
{
	StartOSC();

	if (!mSystem) {
		if (mFramesUntilRetry > 0) {
			--mFramesUntilRetry;
		} else {
			TryInitOpenVR();
			if (!mSystem)
				mFramesUntilRetry = kRetryFrames;
		}
	}

	bool ok = false;

	// 1) SteamVR foveation API
	if (mSystem && mHasFoveationAPI) {
		auto       vtable = *reinterpret_cast<void***>(mSystem);
		auto       getCenter = reinterpret_cast<GetEyeTrackedFoveationCenter_t>(vtable[kSlotGetEyeTrackedFoveationCenter]);
		HmdVector2 ndcLeft{ { 0.0f, 0.0f } };
		HmdVector2 ndcRight{ { 0.0f, 0.0f } };
		if (getCenter(mSystem, &ndcLeft, &ndcRight)) {
			NdcToCombined(ndcLeft, 0.0f, leftX, leftY);
			NdcToCombined(ndcRight, 0.5f, rightX, rightY);
			mState = State::kTrackingOpenVR;
			ok = true;
		}
	}

	// 2) OSC (Steam Link eye tracking sharing)
	if (!ok) {
		float dir[2][3];
		bool  fresh = false;
		{
			std::lock_guard lock(mOSCMutex);
			fresh = mOSCHaveGaze && (NowMs() - mOSCGazeTimeMs) <= kOSCStaleMs;
			if (fresh)
				memcpy(dir, mOSCDir, sizeof(dir));
		}
		if (fresh) {
			float ex[2], ey[2];
			if (ProjectDirection(0, dir[0][0], dir[0][1], dir[0][2], ex[0], ey[0]) &&
				ProjectDirection(1, dir[1][0], dir[1][1], dir[1][2], ex[1], ey[1])) {
				leftX = std::clamp(ex[0] + mOffsetX, 0.0f, 1.0f) * 0.5f;
				leftY = std::clamp(ey[0] + mOffsetY, 0.0f, 1.0f);
				rightX = 0.5f + std::clamp(ex[1] + mOffsetX, 0.0f, 1.0f) * 0.5f;
				rightY = std::clamp(ey[1] + mOffsetY, 0.0f, 1.0f);
				mState = State::kTrackingOSC;
				ok = true;
			}
		}
	}

	if (!ok) {
		if (mState == State::kTrackingOpenVR || mState == State::kTrackingOSC) {
			logger::info("[EyeTracking] Gaze lost after {} valid frames", mValidFrames);
		}
		if (mSystem)
			mState = State::kNoGaze;
		if ((++mInvalidFrames % 5000) == 1) {
			logger::info("[EyeTracking] No valid gaze yet ({} frames). OSC listening: {} on port {}, packets received: {}",
				mInvalidFrames, mOSCListening.load(), mOSCPort, mOSCPackets.load());
		}
		return false;
	}

	++mValidFrames;
	if (!mLoggedFirstGaze) {
		mLoggedFirstGaze = true;
		logger::info("[EyeTracking] First valid gaze via {}: left ({:.3f}, {:.3f}) right ({:.3f}, {:.3f})",
			mState == State::kTrackingOSC ? "OSC" : "SteamVR", leftX, leftY, rightX, rightY);
	}
	return true;
}

void EyeTracking::Shutdown()
{
	if (mOSCRunning.exchange(false)) {
		if (mOSCSocket != kInvalidSocket && g_ws.closesocket)
			g_ws.closesocket(mOSCSocket);
		mOSCSocket = kInvalidSocket;
		if (mOSCThread.joinable())
			mOSCThread.detach();
	}
}
