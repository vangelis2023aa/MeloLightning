#pragma once
// ===========================================================================================
// TEMPORARY diagnostic instrumentation (behavior-neutral, NOT an optimization). DO NOT COMMIT.
//
// Purpose: measure how the guest producer's per-frame wall-clock interval splits between
// active guest execution and blocking on GPU-retire / flip / vsync. The existing overlay's
// "Ring idle" collapses every guest-side stall into one number on the Latte consumer thread;
// this measures the guest (producer) side directly at its own blocking chokepoints.
//
// Anchoring: a "guest frame" is delimited by successive GX2SwapScanBuffers calls ON THE GX2
// MAIN SUBMIT CORE (GX2::sGX2MainCoreIndex). Every wait delta is attributed only when observed
// on that same core, so the span and the accumulators bracket the same interval on one host
// thread. (In multi-core recompiler mode each guest core maps to a fixed host thread; in
// single-core mode one host thread runs all cores sequentially. Either way, only one host
// thread ever satisfies OSGetCoreId()==mainCore at a time, so the accumulators are effectively
// single-writer.)
//
// Straddle safety: the swap (frame boundary) and every wait-add run on the SAME main-core host
// thread. A wait fully returns and adds its delta before that thread can reach the next swap, so
// no wait can straddle the boundary; each wait counts entirely in the frame it completes in.
//
// Double-count safety: only the funnels are instrumented -- GX2WaitTimeStamp (through which both
// GX2DrawDone and GX2Command_WaitForNextBufferRetired pass), GX2WaitForFlip, GX2WaitForVsync.
// None of these three nest inside another, so a single blocking episode is counted once.
//
// Clock: PPCTimer_getRawTsc() deltas converted via PPCTimer_tscToMicroseconds() -- the SAME raw
// TSC clock the LattePerfStatTimer frame-budget overlay uses, so values are directly comparable.
//
// Atomics: the user asked for relaxed-atomic accumulation. The cur* accumulators are only written
// on the main core (single writer) and the prev* snapshot is read cross-thread by the overlay
// (Latte thread); all fields are std::atomic with relaxed ordering. A rare overlay read that lands
// between the four prev* stores yields one mixed-frame sample -- bounded display noise, no UB.
//
// Remove this file together with its call sites when the measurement is done.
// ===========================================================================================
#include <atomic>
#include "Cafe/HW/Espresso/PPCState.h" // PPCTimer_getRawTsc / PPCTimer_tscToMicroseconds (+ base int typedefs)

// Minimal forward declarations so this header compiles standalone (e.g. from the overlay TU,
// which reads prev* but never calls the gated add). Definitions live in coreinit / GX2_Misc.
namespace coreinit { sint32 OSGetCoreId(); }

namespace GX2
{
	extern uint32 sGX2MainCoreIndex; // matches the declaration in GX2_Misc.h

	enum class GuestWaitCategory
	{
		GpuRetire, // GX2WaitTimeStamp (GPU buffer-retire funnel)
		Flip,      // GX2WaitForFlip (real waits only; the no-flip early-return path is excluded)
		Vsync,     // GX2WaitForVsync
	};

	struct GuestFrameTimingState
	{
		// --- current (in-progress) guest frame; written only on the main submit core ---
		std::atomic<uint64> curFrameStartTsc{0}; // raw TSC at the last main-core swap (0 = not started yet)
		std::atomic<uint64> curGpuRetireWaitTsc{0};
		std::atomic<uint64> curFlipWaitTsc{0};
		std::atomic<uint64> curVsyncWaitTsc{0};

		// --- published snapshot of the last COMPLETED guest frame; read by the overlay (Latte thread) ---
		std::atomic<uint64> prevFrameSpanTsc{0};
		std::atomic<uint64> prevGpuRetireWaitTsc{0};
		std::atomic<uint64> prevFlipWaitTsc{0};
		std::atomic<uint64> prevVsyncWaitTsc{0};
		std::atomic<uint32> prevValid{0};
	};

	inline GuestFrameTimingState& GetGuestFrameTiming()
	{
		static GuestFrameTimingState s_state;
		return s_state;
	}

	// Attribute a blocking duration to the current guest frame, but ONLY when running on the GX2
	// main submit core (keeps everything anchored to the frame pacer and avoids cross-core double
	// counting). Safe to call from any guest thread; a non-main-core call is a no-op. startTsc is a
	// raw TSC captured immediately before the (unchanged) wait.
	inline void AddGuestWaitTsc(GuestWaitCategory cat, uint64 startTsc)
	{
		if (coreinit::OSGetCoreId() != (sint32)sGX2MainCoreIndex)
			return;
		const uint64 dt = PPCTimer_getRawTsc() - startTsc;
		GuestFrameTimingState& s = GetGuestFrameTiming();
		switch (cat)
		{
		case GuestWaitCategory::GpuRetire: s.curGpuRetireWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		case GuestWaitCategory::Flip:      s.curFlipWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		case GuestWaitCategory::Vsync:     s.curVsyncWaitTsc.fetch_add(dt, std::memory_order_relaxed); break;
		}
	}

	// Called at the top of GX2SwapScanBuffers, ON THE MAIN SUBMIT CORE ONLY. Publishes the just-
	// finished guest frame's span + per-category wait sums, then resets the accumulators and starts
	// timing the next guest frame. First call (curFrameStartTsc==0) only arms the clock.
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
			s.prevValid.store(1, std::memory_order_relaxed);
		}
		// reset accumulators for the new frame
		s.curGpuRetireWaitTsc.store(0, std::memory_order_relaxed);
		s.curFlipWaitTsc.store(0, std::memory_order_relaxed);
		s.curVsyncWaitTsc.store(0, std::memory_order_relaxed);
		s.curFrameStartTsc.store(now, std::memory_order_relaxed);
	}
}
