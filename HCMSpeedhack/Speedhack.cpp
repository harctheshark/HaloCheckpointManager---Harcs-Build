#include "pch.h"
#include "Speedhack.h"
#include "safetyhook.hpp" // hooking
#include <atomic>         // the timeHacker seqlock below

// this implementation of a speedhack is adapted from https://github.com/Letomaniy/Speed-Hack
// it hooks QueryPerformanceCounter, GetTickCount, GetTickCount64, and TimeGetTime,
// and basically uses a timeHacker class to keep track of a seperate flow of "fake time" to return to anyone asking the hooks



// ================================================================================================================
// ⚠ THE THREE FIELDS BELOW ARE READ BY EVERY THREAD IN THE PROCESS AND WRITTEN BY ONE. THAT IS A DATA RACE, AND
// IT IS NOT BENIGN.
//
// getCurrentTime() runs inside the QueryPerformanceCounter / GetTickCount hooks, so it is called from the game's
// render thread, its audio thread, and on this title NVIDIA's frame-generation pacer - well over a hundred times
// a second, concurrently. setSpeed() updates offset, last-update and speed as three separate stores. A reader
// that lands between them mixes a NEW offset with an OLD last-update (or vice versa), and since the result is
// `speed * (now - lastUpdate) + offset`, a mismatched pair does not produce a slightly wrong time - it produces
// a timestamp that can jump BACKWARDS BY THE WHOLE SESSION. Monotonic-clock consumers respond to that by
// dividing by a negative delta, stalling, or asserting.
//
// The obvious fix - the std::scoped_lock that is commented out at all four call sites in this file - is the
// wrong one: it puts a contended mutex in the hot path of the process's clock, taken by every thread on every
// query. So this is a SEQLOCK instead. Readers take no lock at all; they read a version counter either side of
// the payload and retry if it moved. Writers are rare (a toggle) and cheap.
//
// Same construction as HCEGetCameraData's PovSnapshot in HCMInternal, for the same reason.
// ================================================================================================================
template<class DataType>
class timeHacker {
	DataType time_offset;
	DataType time_last_update;

	double speed_;

	// Even = stable, odd = a write is in progress.
	std::atomic<uint32_t> sequence{ 0 };

public:


	timeHacker(DataType currentRealTime, double initialSpeed) {
		time_offset = currentRealTime;
		time_last_update = currentRealTime;

		speed_ = initialSpeed;
	}

	// Re-seed to a fresh clock reading, discarding any accumulated offset.
	//
	// This exists because the seqlock's std::atomic member deletes copy-assignment, and construction used to
	// re-initialise these objects by assigning a whole new timeHacker over the top. That was only safe because
	// it happens before the hooks are installed; doing it through the seqlock means it stays safe even if it
	// is ever called while readers are live.
	void reset(DataType currentRealTime, double initialSpeed) {
		sequence.fetch_add(1, std::memory_order_acq_rel);
		std::atomic_thread_fence(std::memory_order_release);

		time_offset = currentRealTime;
		time_last_update = currentRealTime;
		speed_ = initialSpeed;

		std::atomic_thread_fence(std::memory_order_release);
		sequence.fetch_add(1, std::memory_order_acq_rel);
	}

	void setSpeed(DataType currentRealTime, double speed) {
		// Computed BEFORE the write window opens, so the seqlock covers only the three stores.
		const DataType newOffset = getCurrentTime(currentRealTime);

		sequence.fetch_add(1, std::memory_order_acq_rel);            // -> odd: readers back off
		std::atomic_thread_fence(std::memory_order_release);

		time_offset = newOffset;
		time_last_update = currentRealTime;
		speed_ = speed;

		std::atomic_thread_fence(std::memory_order_release);
		sequence.fetch_add(1, std::memory_order_acq_rel);            // -> even: published
	}

	DataType getCurrentTime(DataType currentRealTime) {
		DataType offset{};
		DataType lastUpdate{};
		double speed = 1.0;

		// Bounded, NOT an unbounded spin. If a writer were descheduled mid-update, spinning here would hang
		// whichever thread asked for the time - including the render thread. A handful of retries is far more
		// than a three-store window ever needs, and taking a torn read on the last attempt is survivable in a
		// way that a stalled clock is not.
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			const uint32_t before = sequence.load(std::memory_order_acquire);
			if (before & 1u) continue;                               // mid-write, try again

			offset = time_offset;
			lastUpdate = time_last_update;
			speed = speed_;

			std::atomic_thread_fence(std::memory_order_acquire);
			if (sequence.load(std::memory_order_relaxed) == before) break;   // consistent snapshot
		}

		const DataType difference = currentRealTime - lastUpdate;
		return (DataType)(speed * difference) + offset;
	}


};



class SpeedhackImpl : public SpeedhackImplBase
{
private:

	static inline std::mutex hookMutex{};



	static inline safetyhook::InlineHook queryPerformanceCounterHook{};
	static inline safetyhook::InlineHook getTickCountHook{};
	static inline safetyhook::InlineHook getTickCount64Hook{};
	static inline safetyhook::InlineHook timeGetTimeHook{};

	static inline timeHacker<DWORD>     speedHack{GetTickCount(), 1.0};
	static inline timeHacker<ULONGLONG> speedHackULL{GetTickCount64(), 1.0};
	static inline timeHacker<LONGLONG>  speedHackLL{0, 1.0};


	static BOOL __stdcall queryPerformanceCounterHookFunction(LARGE_INTEGER* lpPerformanceCount)
	{

		//std::scoped_lock<std::mutex> lock(hookMutex);

		LARGE_INTEGER performanceCounter;
		BOOL result = queryPerformanceCounterHook.stdcall<BOOL, _LARGE_INTEGER*>(&performanceCounter);
		lpPerformanceCount->QuadPart = speedHackLL.getCurrentTime(performanceCounter.QuadPart);
		return result;
	}

	static DWORD __stdcall getTickCountHookFunction()
	{

		//std::scoped_lock<std::mutex> lock(hookMutex);
		return speedHack.getCurrentTime(getTickCountHook.stdcall<DWORD>());
	}

	static ULONGLONG __stdcall getTickCount64HookFunction()
	{

		//std::scoped_lock<std::mutex> lock(hookMutex);
		return speedHackULL.getCurrentTime(getTickCount64Hook.stdcall<ULONGLONG>());
	}

	static void setAllTimeHackerSpeeds(double speedInput) {
		//std::scoped_lock<std::mutex> lock(hookMutex);
		speedHack.setSpeed(getTickCountHook.stdcall<DWORD>(), speedInput);

		speedHackULL.setSpeed(getTickCount64Hook.stdcall<ULONGLONG>(), speedInput);

		LARGE_INTEGER performanceCounter;
		queryPerformanceCounterHook.stdcall<BOOL, _LARGE_INTEGER*>(&performanceCounter);

		speedHackLL.setSpeed(performanceCounter.QuadPart, speedInput);
	}

public:


	SpeedhackImpl()
	{

		// init speedHackLL timehacker 
		LARGE_INTEGER performanceCounter;
		QueryPerformanceCounter((_LARGE_INTEGER*)&performanceCounter);
		speedHackLL.reset(performanceCounter.QuadPart, 1.0);

		// create hooks
		queryPerformanceCounterHook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Kernel32.dll"), "QueryPerformanceCounter"), queryPerformanceCounterHookFunction);
		getTickCountHook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Kernel32.dll"), "GetTickCount"), getTickCountHookFunction);
		getTickCount64Hook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Kernel32.dll"), "GetTickCount64"), getTickCount64HookFunction);
		timeGetTimeHook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Winmm.dll"), "timeGetTime"), getTickCountHookFunction);

	}

	~SpeedhackImpl() = default;

	virtual void setSpeed(double in) override
	{
		setAllTimeHackerSpeeds(in);
	}
};

Speedhack::Speedhack() : impl(std::make_unique< SpeedhackImpl>()) 
{
	instance = this;
}

Speedhack::~Speedhack() = default;

