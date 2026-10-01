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
// Layer 3 (this change, committed separately): decomposes the single Layer-2 guest-internal total
// by (a) CALLER category and (b) which thread blocked. queueAndWait takes a diagnostic-only caller
// tag (default 0 = "Other"); the two GX2 funnels pass Flip/Vsync. GX2WaitTimeStamp / GPU-retire is
// NOT in the breakdown because it uses TCLWaitTimestamp, not queueAndWait (its slot stays ~0 by
// construction; it is measured at its own Layer-1 funnel instead). Everything else — mutex, event,
// semaphore, cond, message-queue, sleep/join — defaults to "Other". Separately, the thread passed
// to queueAndWait is compared against the producer thread (the one that drives GX2SwapScanBuffers on
// the main core) to split the SAME main-core-gated total into producer (critical path) vs other
// (worker threads multiplexed on the main core). Per category we keep cumulative time AND a count.
// This only adds relaxed-atomic accumulation + a pointer compare; it changes no scheduling, queue
// ordering, locking, timing or synchronization. The Layer-2 total counter is kept intact and the
// per-category sum should match it (modulo a few-ns raw-TSC skew from a second clock read).
//
// Layer 4 (this change, committed separately — SAFE REIMPLEMENTATION): subdivides the former
// catch-all "Other" caller into the specific guest primitives (mutex, event, semaphore, cond,
// message-queue recv/send, sleep-ticks, sleep-thread, join) so ONE device run can name exactly
// which primitive the producer's long queueAndWait belongs to. It does this PURELY by tagging the
// existing queueAndWait CALL SITES (the OSLockMutex / OSWaitEvent / OSReceiveMessage / ... wait
// functions already pass the Layer-3 diagCaller argument; we just pass the specific value instead
// of defaulting to Other) plus three extra relaxed-atomic accumulators inside the SAME Layer-3
// breakdown helper that already runs on the main core: a per-primitive producer split and the
// producer's single longest wait this frame (its primitive + duration). An earlier Layer-4 attempt
// also instrumented the scheduler wake funnels (wakeupEntireWaitQueue / wakeupSingleThreadWaitQueue
// / cancelWait) to trace the waker thread; that broke launch and is DELIBERATELY NOT redone here —
// no scheduler/wake-funnel code is touched, no guest thread-name pointers are dereferenced, and the
// wake-side ("who satisfies the wait") trace is intentionally deferred. All additions stay on the
// existing main-core gate, so the per-primitive sums still reconstruct the Layer-2 total.
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

	// Layer 3: diagnostic-only caller tag passed into queueAndWait. Values are stable array indices.
	// Layer 4: indices 0-3 are unchanged (overlay compat); 4-12 subdivide the former "Other" bucket
	// by the specific guest primitive, tagged purely at each wait CALL SITE (no scheduler changes).
	enum class GuestInternalCaller : uint32
	{
		Other        = 0,  // untagged queueAndWait callers (should be ~empty once Layer 4 tagging is in)
		Flip         = 1,  // GX2WaitForFlip
		Vsync        = 2,  // GX2WaitForVsync
		GpuRetire    = 3,  // reserved; GX2WaitTimeStamp uses TCLWaitTimestamp (NOT queueAndWait) -> stays ~0
		Mutex        = 4,  // OSLockMutex contention (blocking path only; OSFastMutex direct-switch bypasses the funnel)
		Event        = 5,  // OSWaitEvent / OSWaitEventWithTimeout
		Semaphore    = 6,  // OSWaitSemaphore
		Cond         = 7,  // OSWaitCond / OSFastCond_Wait
		MsgQueueRecv = 8,  // OSReceiveMessage (blocking)
		MsgQueueSend = 9,  // OSSendMessage (blocking, queue full)
		SleepTicks   = 10, // OSSleepTicks
		SleepThread  = 11, // OSSleepThread
		Join         = 12, // OSJoinThread
		Count        = 13,
	};

	// Layer 4: pure name helper for the overlay. String literals only — no pointer dereference, no
	// guest thread-name access, nothing that can fault. Safe to call from the Latte overlay thread.
	inline const char* GuestInternalCallerName(uint32 caller)
	{
		switch ((GuestInternalCaller)caller)
		{
		case GuestInternalCaller::Other:        return "Other";
		case GuestInternalCaller::Flip:         return "Flip";
		case GuestInternalCaller::Vsync:        return "Vsync";
		case GuestInternalCaller::GpuRetire:    return "GpuRetire";
		case GuestInternalCaller::Mutex:        return "Mutex";
		case GuestInternalCaller::Event:        return "Event";
		case GuestInternalCaller::Semaphore:    return "Semaphore";
		case GuestInternalCaller::Cond:         return "Cond";
		case GuestInternalCaller::MsgQueueRecv: return "MsgQueue-recv";
		case GuestInternalCaller::MsgQueueSend: return "MsgQueue-send";
		case GuestInternalCaller::SleepTicks:   return "SleepTicks";
		case GuestInternalCaller::SleepThread:  return "SleepThread";
		case GuestInternalCaller::Join:         return "Join";
		default:                                return "?";
		}
	}

	struct GuestFrameTimingState
	{
		// --- current (in-progress) guest frame; written only on the main submit core ---
		std::atomic<uint64> curFrameStartTsc{0};
		std::atomic<uint64> curGpuRetireWaitTsc{0};
		std::atomic<uint64> curFlipWaitTsc{0};
		std::atomic<uint64> curVsyncWaitTsc{0};
		std::atomic<uint64> curGuestInternalWaitTsc{0}; // Layer 2: queueAndWait accumulator
		// Layer 3: per-caller decomposition of the Layer-2 total (same main-core gate), time + count.
		static constexpr uint32 kGICallerCount = (uint32)GuestInternalCaller::Count;
		std::atomic<uint64> curGICallerTsc[kGICallerCount]{};
		std::atomic<uint32> curGICallerCount[kGICallerCount]{};
		// Layer 3: producer (critical-path) share of the SAME total; non-producer = total - producer.
		std::atomic<uint64> curGIProducerTsc{0};
		std::atomic<uint32> curGIProducerCount{0};
		// Layer 4: per-primitive producer (critical-path) split — same gate, parallel to curGICaller*.
		std::atomic<uint64> curGICallerProducerTsc[kGICallerCount]{};
		std::atomic<uint32> curGICallerProducerCount[kGICallerCount]{};
		// Layer 4: producer's single longest queueAndWait this frame (primitive index + duration only;
		// NO thread id/name, NO pointer deref). Lets one run name the primitive behind the long wait.
		std::atomic<uint64> curProducerMaxWaitTsc{0};
		std::atomic<uint32> curProducerMaxWaitCaller{0};

		// --- published snapshot of the last COMPLETED guest frame; read by the overlay (Latte thread) ---
		std::atomic<uint64> prevFrameSpanTsc{0};
		std::atomic<uint64> prevGpuRetireWaitTsc{0};
		std::atomic<uint64> prevFlipWaitTsc{0};
		std::atomic<uint64> prevVsyncWaitTsc{0};
		std::atomic<uint64> prevGuestInternalWaitTsc{0}; // Layer 2
		// Layer 3 published snapshots
		std::atomic<uint64> prevGICallerTsc[kGICallerCount]{};
		std::atomic<uint32> prevGICallerCount[kGICallerCount]{};
		std::atomic<uint64> prevGIProducerTsc{0};
		std::atomic<uint32> prevGIProducerCount{0};
		// Layer 4 published snapshots
		std::atomic<uint64> prevGICallerProducerTsc[kGICallerCount]{};
		std::atomic<uint32> prevGICallerProducerCount[kGICallerCount]{};
		std::atomic<uint64> prevProducerMaxWaitTsc{0};
		std::atomic<uint32> prevProducerMaxWaitCaller{0};
		std::atomic<uint32> prevValid{0};

		// Layer 3: handle of the thread that drives GX2SwapScanBuffers on the main core (critical path).
		// Set each main-core swap, persistent across frames (NOT reset by the per-frame publish).
		std::atomic<void*> producerThread{nullptr};
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

	// Layer 3: record which thread currently drives presentation (critical path). Cheap, idempotent.
	inline void SetProducerThread(void* threadHandle)
	{
		GetGuestFrameTiming().producerThread.store(threadHandle, std::memory_order_relaxed);
	}

	// Layer 3: decompose the Layer-2 guest-internal total by caller tag and producer/other. Same
	// main-core gate as AddGuestWaitTsc so the per-caller sum reconstructs the Layer-2 total. Does
	// NOT touch curGuestInternalWaitTsc (the Layer-2 total is accumulated independently and kept intact).
	inline void AddGuestInternalBreakdown(uint32 caller, uint64 startTsc, void* blockingThread)
	{
		if (coreinit::OSGetCoreId() != (sint32)sGX2MainCoreIndex)
			return;
		const uint64 dt = PPCTimer_getRawTsc() - startTsc;
		GuestFrameTimingState& s = GetGuestFrameTiming();
		if (caller >= GuestFrameTimingState::kGICallerCount)
			caller = (uint32)GuestInternalCaller::Other;
		s.curGICallerTsc[caller].fetch_add(dt, std::memory_order_relaxed);
		s.curGICallerCount[caller].fetch_add(1, std::memory_order_relaxed);
		if (blockingThread != nullptr && blockingThread == s.producerThread.load(std::memory_order_relaxed))
		{
			s.curGIProducerTsc.fetch_add(dt, std::memory_order_relaxed);
			s.curGIProducerCount.fetch_add(1, std::memory_order_relaxed);
			// Layer 4: per-primitive producer split + running producer longest-wait (primitive + duration
			// only). Single-writer on the main core, so load/compare/store on the max needs no CAS.
			s.curGICallerProducerTsc[caller].fetch_add(dt, std::memory_order_relaxed);
			s.curGICallerProducerCount[caller].fetch_add(1, std::memory_order_relaxed);
			if (dt > s.curProducerMaxWaitTsc.load(std::memory_order_relaxed))
			{
				s.curProducerMaxWaitTsc.store(dt, std::memory_order_relaxed);
				s.curProducerMaxWaitCaller.store(caller, std::memory_order_relaxed);
			}
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
			for (uint32 i = 0; i < GuestFrameTimingState::kGICallerCount; i++)
			{
				s.prevGICallerTsc[i].store(s.curGICallerTsc[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
				s.prevGICallerCount[i].store(s.curGICallerCount[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
				s.prevGICallerProducerTsc[i].store(s.curGICallerProducerTsc[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
				s.prevGICallerProducerCount[i].store(s.curGICallerProducerCount[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
			s.prevGIProducerTsc.store(s.curGIProducerTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevGIProducerCount.store(s.curGIProducerCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevProducerMaxWaitTsc.store(s.curProducerMaxWaitTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevProducerMaxWaitCaller.store(s.curProducerMaxWaitCaller.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevValid.store(1, std::memory_order_relaxed);
		}
		s.curGpuRetireWaitTsc.store(0, std::memory_order_relaxed);
		s.curFlipWaitTsc.store(0, std::memory_order_relaxed);
		s.curVsyncWaitTsc.store(0, std::memory_order_relaxed);
		s.curGuestInternalWaitTsc.store(0, std::memory_order_relaxed);
		for (uint32 i = 0; i < GuestFrameTimingState::kGICallerCount; i++)
		{
			s.curGICallerTsc[i].store(0, std::memory_order_relaxed);
			s.curGICallerCount[i].store(0, std::memory_order_relaxed);
			s.curGICallerProducerTsc[i].store(0, std::memory_order_relaxed);
			s.curGICallerProducerCount[i].store(0, std::memory_order_relaxed);
		}
		s.curGIProducerTsc.store(0, std::memory_order_relaxed);
		s.curGIProducerCount.store(0, std::memory_order_relaxed);
		s.curProducerMaxWaitTsc.store(0, std::memory_order_relaxed);
		s.curProducerMaxWaitCaller.store(0, std::memory_order_relaxed);
		s.curFrameStartTsc.store(now, std::memory_order_relaxed);
	}
}
