#include <Geode/Geode.hpp>
#include <Windows.h>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace {
	using NtSetTimerResolution_t = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
	using NtQueryTimerResolution_t = LONG(NTAPI*)(PULONG, PULONG, PULONG);

	struct ThrottleState {
		ULONG Version;
		ULONG ControlMask;
		ULONG StateMask;
	};

	NtSetTimerResolution_t pSet = nullptr;
	NtQueryTimerResolution_t pQuery = nullptr;
	bool g_applied = false;

	bool resolveNt() {
		if (pSet && pQuery) return true;
		HMODULE ntdll = GetModuleHandleA("ntdll.dll");
		if (!ntdll) return false;
		pSet = reinterpret_cast<NtSetTimerResolution_t>(GetProcAddress(ntdll, "NtSetTimerResolution"));
		pQuery = reinterpret_cast<NtQueryTimerResolution_t>(GetProcAddress(ntdll, "NtQueryTimerResolution"));
		return pSet && pQuery;
	}

	void setThrottling(bool ignore) {
		// ProcessPowerThrottling = 4. StateMask 0 + ControlMask bit => throttling of timer resolution is disabled.
		ThrottleState st{ PROCESS_POWER_THROTTLING_CURRENT_VERSION,
			PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
		if (!ignore) st.ControlMask = 0; // back to system default
		SetProcessInformation(GetCurrentProcess(), static_cast<PROCESS_INFORMATION_CLASS>(4), &st, sizeof(st));
	}

	void release() {
		if (!g_applied || !resolveNt()) return;
		ULONG cur = 0;
		pSet(0, FALSE, &cur);
		g_applied = false;
	}

	void apply() {
		auto mod = Mod::get();
		if (!mod->getSettingValue<bool>("enabled")) {
			release();
			setThrottling(false);
			return;
		}
		if (!resolveNt()) {
			log::error("Could not resolve NtSetTimerResolution");
			return;
		}

		ULONG minRes = 0, maxRes = 0, curRes = 0;
		pQuery(&minRes, &maxRes, &curRes); // 100 ns units; maxRes = finest supported

		double ms = mod->getSettingValue<double>("resolution");
		ULONG want = static_cast<ULONG>(std::llround(ms * 10000.0));
		want = std::clamp(want, maxRes, minRes);

		ULONG actual = 0;
		LONG status = pSet(want, TRUE, &actual);
		if (status != 0) {
			log::error("NtSetTimerResolution failed: 0x{:X}", static_cast<unsigned>(status));
			return;
		}
		g_applied = true;
		setThrottling(mod->getSettingValue<bool>("ignore-throttling"));
		log::info("Timer resolution set to {:.4f} ms (requested {:.4f} ms, finest supported {:.4f} ms)",
			actual / 10000.0, want / 10000.0, maxRes / 10000.0);
	}
}

$on_mod(Loaded) {
	apply();
	listenForSettingChanges<bool>("enabled", [](bool) { apply(); });
	listenForSettingChanges<double>("resolution", [](double) { apply(); });
	listenForSettingChanges<bool>("ignore-throttling", [](bool) { apply(); });
}
