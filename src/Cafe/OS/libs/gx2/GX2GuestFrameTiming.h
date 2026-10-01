#pragma once
// ===========================================================================================
// TEMPORARY diagnostic instrumentation (behavior-neutral, NOT an optimization).
//
// Layer 1 (committed af6b64f): guest frame span via GX2SwapScanBuffers + GPU-retire / flip /
// vsync waits at their funnels. That layer showed 0 ms for all three funnels on 960x540 busy,
// leaving the entire ~24 ms in the derived "active/unaccounted" remainder.
//
// Layer 2 (this file, committed separately): adds guest-internal wait via queueAndWait, the
// single common funnel for all thread-blocking (thread queues, mutexes, events, semaphores,
// message queues, sleep/join). After this layer, "truly unaccounted" = span - queueAndWait,
// which is the closest we can get to "active guest execution" from host-wall-clock measurement.
//
// Anchoring: same as Layer 1 — a guest frame is delimited by successive GX2SwapScanBuffers on
// the GX2 main submit core. queueAndWait is gated to that same core so its accumulators bracket
// the same host-thread interval as the frame span.
//
// Double-count note: GX2WaitForFlip and GX2WaitForVsync both call queueAndWait internally.
// Their Layer-1 timers bracket the entire __OSLockScheduler region (including the queueAndWait
// call), so flip/vsync times overlap with the new queueAndWait accumulator. The overlay shows
// both; interpret as: flip/vsync = GX2-specific blocking; guest-internal = superset of ALL
// queueAndWait on the main core; "truly unaccounted" subtracts only the superset.
//
// Clock/atomics/threading: identical to Layer 1 (PPCTimer_getRawTsc, relaxed atomics, single-
// writer main core, cross-thread overlay read).
//
// Remove this file together with its call sites when the measurement is done.
// ===========================================================================================
#include <atomic>
#include "Cafe/HW/Espresso/PPCState.h" // PPCTimer_getRawTsc / PPCTimer_tscToMicroseconds (+ base int typedefs)

// Forward declarations so this header compiles standalone from any TU that reads prev*.
namespace coreinit { sint32 OSGetCoreId(); }

namespace GX2
{
	extern uint32 sGX2MainCoreIndex; // matches GX2_Misc.h

	enum class GuestWaitCategory
	{
		GpuRetire,   // GX2WaitTimeStamp (GPU buffer-retire funnel)
		Flip,        // GX2WaitForFlip (real waits only; no-flip early-return excluded)
		Vsync,       // GX2WaitForVsync
		GuestInternal, // queueAndWait — the common funnel for all guest-internal thread blocking
	};

	struct GuestFrameTimingState
	{
		// --- current (in-progress) guest frame; written only on the main submit core ---
		std::atomic<uint64> curFrameStartTsc{0};
		std::atomic<uint64> curGpuRetireWaitTsc{0};
		std::atomic<uint64> curFlipWaitTsc{0};
		std::atomic<uint64> curVsyncWaitTsc{0};
		std::atomic<uint64> curGuestInternalWaitTsc{0}; // Layer 2: queueAndWait accumulator

		// --- published snapshot of the last COMPLETED guest frame; read by the overlay (Latte thread) ---
		std::atomic<uint64> prevFrameSpanTsc{0};
		std::atomic<uint64> prevGpuRetireWaitTsc{0};
		std::atomic<uint64> prevFlipWaitTsc{0};
		std::atomic<uint64> prevVsyncWaitTsc{0};
		std::atomic<uint64> prevGuestInternalWaitTsc{0}; // Layer 2
		std::atomic<uint32> prevValid{0};
	};

	inline GuestFrameTimingState& GetGuestFrameTiming()
	{
		static GuestFrameTimingState s_state;
		return s_state;
	}

	inline void AddGuestWaitTsc(GuestWaitCategory cat, uint64 startTsc)
	{
		if (coreinit::OSGetCoreId() != (sint32)sGX2MainCoreIndex)
			return;
		const uint64 dt = PPCTimer_getRawTsc() - startTsc;
		GuestFrameTimingState& s = GetGuestFrameTiming();
		switch (cat)
		{
		case GuestWaitCategory::GpuRetire:   s.curGpuRetireWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		case GuestWaitCategory::Flip:        s.curFlipWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		case GuestWaitCategory::Vsync:       s.curVsyncWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		case GuestWaitCategory::GuestInternal: s.curGuestInternalWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		}
	}

	inline void PublishGuestFrameAtMainCoreSwap()
	{
		GuestFrameTimingState& s = GetGuestFrameTiming();
		const uint64 now = PPCTimer_getRawTsc();
		const uint64 start = s.curFrameStartTsc.load(std::memory_order_relaxed);
		if (start != 0)
		{
			s.prevFrameSpanTsc.store(now - start, std::memory_order_relaxed);
			s.prevGpuRetireWaitTsc.store(s.curGpuRetireWaitTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevFlipWaitTsc.store(s.curFlipWaitTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevVsyncWaitTsc.store(s.curVsyncWaitTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevGuestInternalWaitTsc.store(s.curGuestInternalWaitTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevValid.store(1, std::memory_order_relaxed);
		}
		s.curGpuRetireWaitTsc.store(0, std::memory_order_relaxed);
		s.curFlipWaitTsc.store(0, std::memory_order_relaxed);
		s.curVsyncWaitTsc.store(0, std::memory_order_relaxed);
		s.curGuestInternalWaitTsc.store(0, std::memory_order_relaxed);
		s.curFrameStartTsc.store(now, std::memory_order_relaxed);
	}
}
