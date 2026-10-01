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
// Layer 4 (this change, committed separately): traces the remaining synchronization chain so ONE
// device run can answer "exactly what is the producer blocked on, and who satisfies it". It (a)
// subdivides the former catch-all "Other" caller bucket into the specific guest primitives (mutex,
// event, semaphore, cond, message-queue recv/send, sleep-ticks, sleep-thread, join) by tagging each
// queueAndWait call site; (b) splits the producer (critical-path) share per primitive; (c) records
// the producer's single longest wait in the frame together with its primitive, duration and guest
// thread id+name; and (d) captures the "other side" - the thread that wakes the producer - inside the
// three common wake funnels (wakeupEntireWaitQueue / wakeupSingleThreadWaitQueue / cancelWait), but
// ONLY when the woken thread is the producer, recording the waker's id, name, core and a wake count.
// Items (a)-(c) stay on the same main-core gate as Layer 2/3 (so the per-primitive sums still
// reconstruct the Layer-2 total); the waker capture in (d) is deliberately NOT core-gated because the
// waker may run on any core and that identity is the point. All of it is relaxed-atomic accumulation,
// pointer compares and per-char name snapshots under locks already held - no new locks, no scheduling,
// queue-ordering, timing, sleep, wake or synchronization changes. The Layer-2/3 counters are kept
// intact and still displayed; FastMutex contention remains invisible here (it bypasses queueAndWait
// via queueOnly + a direct switch) and GPU-retire still shows at its own Layer-1 funnel, not here.
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
	// Layer 4: extended with the specific guest synchronization primitives so the former catch-all
	// "Other" bucket is subdivided. Indices 0-3 are unchanged so the Layer-1/2/3 overlay stays comparable.
	enum class GuestInternalCaller : uint32
	{
		Other        = 0,  // genuinely untagged queueAndWait (after Layer 4 all known primitives are tagged -> ~0)
		Flip         = 1,  // GX2WaitForFlip
		Vsync        = 2,  // GX2WaitForVsync
		GpuRetire    = 3,  // reserved; GX2WaitTimeStamp uses TCLWaitTimestamp (NOT queueAndWait) -> stays ~0
		// Layer 4: specific guest sync primitives, tagged at each queueAndWait call site.
		Mutex        = 4,  // OSLockMutexInternal (contended OSMutex)
		Event        = 5,  // OSWaitEventInternal / OSWaitEventWithTimeout
		Semaphore    = 6,  // OSWaitSemaphoreInternal
		Cond         = 7,  // OSWaitCond / OSFastCond_Wait
		MsgQueueRecv = 8,  // OSReceiveMessage (queue empty)
		MsgQueueSend = 9,  // OSSendMessage (queue full)
		SleepTicks   = 10, // OSSleepTicks (timed sleep)
		SleepThread  = 11, // OSSleepThread (generic thread-queue wait)
		Join         = 12, // OSJoinThread
		Count        = 13,
	};

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
		static constexpr uint32 kGINameLen = 32; // Layer 4: guest thread-name snapshot length (per-char atomic)
		std::atomic<uint64> curGICallerTsc[kGICallerCount]{};
		std::atomic<uint32> curGICallerCount[kGICallerCount]{};
		// Layer 3: producer (critical-path) share of the SAME total; non-producer = total - producer.
		std::atomic<uint64> curGIProducerTsc{0};
		std::atomic<uint32> curGIProducerCount{0};
		// Layer 4: per-primitive producer (critical-path) split of the same main-core total.
		std::atomic<uint64> curGICallerProducerTsc[kGICallerCount]{};
		std::atomic<uint32> curGICallerProducerCount[kGICallerCount]{};
		// Layer 4: the producer's single longest queueAndWait this frame (what it is actually blocked on).
		std::atomic<uint64> curProducerMaxWaitTsc{0};
		std::atomic<uint32> curProducerMaxWaitCaller{0};
		std::atomic<uint32> curProducerThreadId{0};
		std::atomic<char>   curProducerThreadName[kGINameLen]{};
		// Layer 4: the "other side" - which thread woke the producer (captured in the wake funnels; any core).
		std::atomic<uint32> curProducerWakerId{0};
		std::atomic<char>   curProducerWakerName[kGINameLen]{};
		std::atomic<uint32> curProducerWakerCore{0};
		std::atomic<uint32> curProducerWakeCount{0};

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
		std::atomic<uint32> prevProducerThreadId{0};
		std::atomic<char>   prevProducerThreadName[kGINameLen]{};
		std::atomic<uint32> prevProducerWakerId{0};
		std::atomic<char>   prevProducerWakerName[kGINameLen]{};
		std::atomic<uint32> prevProducerWakerCore{0};
		std::atomic<uint32> prevProducerWakeCount{0};
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

	// Layer 4: lightweight per-char relaxed copies of a guest thread name into/out of the atomic
	// snapshot. Single-writer per destination (main submit core for producer name; publish on same
	// core; wake-funnel writer under the scheduler lock), so the per-char relaxed stores are
	// data-race-free and behavior-neutral. char atomics are always lock-free.
	inline void GICopyNameFromHost(std::atomic<char>* dst, const char* src)
	{
		uint32 i = 0;
		if (src)
			for (; i < GuestFrameTimingState::kGINameLen - 1 && src[i]; i++)
				dst[i].store(src[i], std::memory_order_relaxed);
		dst[i].store('\0', std::memory_order_relaxed);
	}

	inline void GIPublishName(std::atomic<char>* dst, const std::atomic<char>* src)
	{
		for (uint32 i = 0; i < GuestFrameTimingState::kGINameLen; i++)
		{
			const char c = src[i].load(std::memory_order_relaxed);
			dst[i].store(c, std::memory_order_relaxed);
			if (c == '\0')
				return;
		}
	}

	inline void GIReadName(const std::atomic<char>* src, char* dst, uint32 dstLen)
	{
		uint32 i = 0;
		for (; i + 1 < dstLen && i < GuestFrameTimingState::kGINameLen; i++)
		{
			const char c = src[i].load(std::memory_order_relaxed);
			dst[i] = c;
			if (c == '\0')
				return;
		}
		dst[i] = '\0';
	}

	inline const char* GuestInternalCallerName(uint32 c)
	{
		switch ((GuestInternalCaller)c)
		{
		case GuestInternalCaller::Other:        return "Other";
		case GuestInternalCaller::Flip:         return "Flip";
		case GuestInternalCaller::Vsync:        return "Vsync";
		case GuestInternalCaller::GpuRetire:    return "GpuRetire";
		case GuestInternalCaller::Mutex:        return "Mutex";
		case GuestInternalCaller::Event:        return "Event";
		case GuestInternalCaller::Semaphore:    return "Semaphore";
		case GuestInternalCaller::Cond:         return "Cond";
		case GuestInternalCaller::MsgQueueRecv: return "MsgQ-recv";
		case GuestInternalCaller::MsgQueueSend: return "MsgQ-send";
		case GuestInternalCaller::SleepTicks:   return "SleepTicks";
		case GuestInternalCaller::SleepThread:  return "SleepThread";
		case GuestInternalCaller::Join:         return "Join";
		default:                                return "?";
		}
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

	// Layer 4: is this the producer (critical-path) thread? Cheap pointer compare; no name deref.
	inline bool IsProducerThread(void* threadHandle)
	{
		return threadHandle != nullptr && threadHandle == GetGuestFrameTiming().producerThread.load(std::memory_order_relaxed);
	}

	// Layer 4: "other side" capture - record who woke the producer. Called from the wake funnels
	// (wakeupEntireWaitQueue / wakeupSingleThreadWaitQueue / cancelWait) ONLY when the woken thread
	// is the producer, under the existing scheduler lock. NOT main-core gated: the waker can run on
	// any core and that is exactly what we want to learn. Relaxed stores only; no behavior change.
	inline void RecordProducerWake(uint32 wakerId, const char* wakerName, uint32 wakerCore)
	{
		GuestFrameTimingState& s = GetGuestFrameTiming();
		s.curProducerWakerId.store(wakerId, std::memory_order_relaxed);
		GICopyNameFromHost(s.curProducerWakerName, wakerName);
		s.curProducerWakerCore.store(wakerCore, std::memory_order_relaxed);
		s.curProducerWakeCount.fetch_add(1, std::memory_order_relaxed);
	}

	// Layer 3: decompose the Layer-2 guest-internal total by caller tag and producer/other. Same
	// main-core gate as AddGuestWaitTsc so the per-caller sum reconstructs the Layer-2 total. Does
	// NOT touch curGuestInternalWaitTsc (the Layer-2 total is accumulated independently and kept intact).
	// Layer 4: additionally splits the producer share per-primitive and records the producer's single
	// longest wait this frame (its primitive, duration, and guest thread id+name). The producer thread
	// is the only writer of the producer-detail fields and this runs on the (single) main submit core,
	// so the per-char name copy is a lightweight single-writer relaxed store. Pure accounting.
	inline void AddGuestInternalBreakdown(uint32 caller, uint64 startTsc, void* blockingThread, uint32 blockingThreadId, const char* blockingThreadName)
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
			// Layer 4: per-primitive producer split
			s.curGICallerProducerTsc[caller].fetch_add(dt, std::memory_order_relaxed);
			s.curGICallerProducerCount[caller].fetch_add(1, std::memory_order_relaxed);
			// Layer 4: track the producer's single longest wait + its primitive and identity
			if (dt > s.curProducerMaxWaitTsc.load(std::memory_order_relaxed))
			{
				s.curProducerMaxWaitTsc.store(dt, std::memory_order_relaxed);
				s.curProducerMaxWaitCaller.store(caller, std::memory_order_relaxed);
				s.curProducerThreadId.store(blockingThreadId, std::memory_order_relaxed);
				GICopyNameFromHost(s.curProducerThreadName, blockingThreadName);
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
				// Layer 4: per-primitive producer split
				s.prevGICallerProducerTsc[i].store(s.curGICallerProducerTsc[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
				s.prevGICallerProducerCount[i].store(s.curGICallerProducerCount[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
			s.prevGIProducerTsc.store(s.curGIProducerTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevGIProducerCount.store(s.curGIProducerCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
			// Layer 4: producer longest-wait detail + "other side" waker detail
			s.prevProducerMaxWaitTsc.store(s.curProducerMaxWaitTsc.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevProducerMaxWaitCaller.store(s.curProducerMaxWaitCaller.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevProducerThreadId.store(s.curProducerThreadId.load(std::memory_order_relaxed), std::memory_order_relaxed);
			GIPublishName(s.prevProducerThreadName, s.curProducerThreadName);
			s.prevProducerWakerId.store(s.curProducerWakerId.load(std::memory_order_relaxed), std::memory_order_relaxed);
			GIPublishName(s.prevProducerWakerName, s.curProducerWakerName);
			s.prevProducerWakerCore.store(s.curProducerWakerCore.load(std::memory_order_relaxed), std::memory_order_relaxed);
			s.prevProducerWakeCount.store(s.curProducerWakeCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
			s.curGICallerProducerTsc[i].store(0, std::memory_order_relaxed);   // Layer 4
			s.curGICallerProducerCount[i].store(0, std::memory_order_relaxed); // Layer 4
		}
		s.curGIProducerTsc.store(0, std::memory_order_relaxed);
		s.curGIProducerCount.store(0, std::memory_order_relaxed);
		// Layer 4: reset producer-detail + waker-detail (name treated as empty via NUL at [0])
		s.curProducerMaxWaitTsc.store(0, std::memory_order_relaxed);
		s.curProducerMaxWaitCaller.store(0, std::memory_order_relaxed);
		s.curProducerThreadId.store(0, std::memory_order_relaxed);
		s.curProducerThreadName[0].store('\0', std::memory_order_relaxed);
		s.curProducerWakerId.store(0, std::memory_order_relaxed);
		s.curProducerWakerName[0].store('\0', std::memory_order_relaxed);
		s.curProducerWakerCore.store(0, std::memory_order_relaxed);
		s.curProducerWakeCount.store(0, std::memory_order_relaxed);
		s.curFrameStartTsc.store(now, std::memory_order_relaxed);
	}
}
