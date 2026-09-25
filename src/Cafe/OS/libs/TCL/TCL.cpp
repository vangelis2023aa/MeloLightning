#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "config/ActiveSettings.h"

#include "HW/Latte/Core/LattePM4.h"

namespace TCL
{
	SysAllocator<coreinit::OSEvent> s_updateRetirementEvent;
	uint64 s_currentRetireMarker = 0;

	struct TCLStatePPC // mapped into PPC space
	{
		uint64be gpuRetireMarker; // written by GPU
	};

	SysAllocator<TCLStatePPC> s_tclStatePPC;

	// called from GPU for timestamp EOP event
	void TCLGPUNotifyNewRetirementTimestamp()
	{
		// gpuRetireMarker is updated via event eop command
		__OSLockScheduler();
		coreinit::OSSignalEventAllInternal(s_updateRetirementEvent.GetPtr());
		__OSUnlockScheduler();
	}

	int TCLTimestamp(TCLTimestampId id, uint64be* timestampOut)
	{
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			MEMPTR<uint32> b;
			// this is the timestamp of the last buffer that was retired by the GPU
			stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
			*timestampOut = retireTimestamp.load();
			return 0;
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLTimestamp(): Unsupported timestamp ID {}", (uint32)id);
			*timestampOut = 0;
			return 0;
		}
	}

	int TCLWaitTimestamp(TCLTimestampId id, uint64 waitTs, uint64 timeout)
	{
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			while ( true )
			{
				stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
				uint64 currentTimestamp = retireTimestamp.load();
				if (currentTimestamp >= waitTs)
					return 0;
				coreinit::OSWaitEvent(s_updateRetirementEvent.GetPtr());
			}
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLWaitTimestamp(): Unsupported timestamp ID {}", (uint32)id);
		}
		return 0;
	}

	static constexpr uint32 TCL_RING_BUFFER_SIZE = 4096; // in U32s

	std::atomic<uint32> tclRingBufferA[TCL_RING_BUFFER_SIZE];
	std::atomic<uint32> tclRingBufferA_readIndex{0};
	std::atomic<uint32> tclRingBufferA_writeIndex{0};

	// GPU code calls this to grab the next command word
	bool TCLGPUReadRBWord(uint32& cmdWord)
	{
		uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::relaxed);
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::acquire);
		if (readIndex == writeIndex)
			return false;
		cmdWord = tclRingBufferA[readIndex].load(std::memory_order::relaxed);
		tclRingBufferA_readIndex.store((readIndex + 1) % TCL_RING_BUFFER_SIZE, std::memory_order::release);
		return true;
	}

	void TCLWaitForRBSpace(uint32be numU32s)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);
		uint32 waitIterations = 0;
		// Backoff for the ring-buffer space wait below. This wait is pure CPU-ahead-of-GPU
		// backpressure: the producer (CPU/GX2) has filled the ring and must wait for the GPU
		// consumer to advance the read index before more commands can be written. On ARM/iOS
		// _mm_pause() is only a "yield" hint and does not idle the core, so the original pure
		// spin burned a full core for the entire stall. We keep a very short pause-spin so the
		// common case (the GPU frees a slot within microseconds) stays low-latency, then yield
		// briefly, then ramp to short, bounded sleeps so a longer stall stops spinning a core.
		// The cap is kept small so a command whose space frees up mid-sleep is never materially
		// delayed. Unlike the GPU-idle wait, this is not clamped to a vsync deadline because it
		// is not on the vsync/pacing path - the small cap alone bounds any added latency.
		//
		// The "Aggressive Frame-Pacing Backoff" experimental toggle (ExperimentalAggressiveFramePacing,
		// read once here at wait entry, not per iteration) shortens the spin/yield window and sleeps
		// sooner and a little longer. With the toggle OFF these constants equal the original values, so
		// this wait behaves the same as it did before the toggle existed. The aggressive values are
		// experimental starting points for on-device tuning, not proven-optimal settings.
		const bool aggressive = ActiveSettings::ExperimentalAggressiveFramePacing();
		const uint32 kTCLRBSpaceSpinIterations  = aggressive ?  16 :  64; // pause-spin iterations for microsecond-latency pickup
		const uint32 kTCLRBSpaceYieldIterations = aggressive ?   8 :  32; // yield-only iterations before we begin sleeping
		const uint32 kTCLRBSpaceBackoffStepUs   = aggressive ? 100 :  50; // additional sleep granted per further waiting iteration
		const uint32 kTCLRBSpaceBackoffMaxUs    = aggressive ? 500 : 250; // hard cap so a submission is never materially delayed
		while (true)
		{
			uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::acquire);
			uint32 distance = (readIndex + TCL_RING_BUFFER_SIZE - writeIndex) & (TCL_RING_BUFFER_SIZE - 1);
			if (writeIndex == readIndex) // buffer completely empty
				distance = TCL_RING_BUFFER_SIZE;
			if (distance >= numU32s + 1) // assume distance minus one, because we are never allowed to completely wrap around
				break;

			// Not enough space yet. Back off progressively; the loop breaks immediately
			// above once the GPU has freed enough space, so no sleep is inserted once the
			// command can actually be written.
			if (waitIterations < kTCLRBSpaceSpinIterations)
			{
				_mm_pause();
			}
			else if (waitIterations < kTCLRBSpaceSpinIterations + kTCLRBSpaceYieldIterations)
			{
				std::this_thread::yield();
			}
			else
			{
				uint32 sleepUs = (waitIterations - (kTCLRBSpaceSpinIterations + kTCLRBSpaceYieldIterations) + 1) * kTCLRBSpaceBackoffStepUs;
				if (sleepUs > kTCLRBSpaceBackoffMaxUs)
					sleepUs = kTCLRBSpaceBackoffMaxUs;
				std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
			}
			waitIterations++;
		}
	}

	// this function assumes that TCLWaitForRBSpace was called and that there is enough space
	void TCLWriteCmd(uint32be* cmd, uint32 cmdLen)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);

		while (cmdLen > 0)
		{
			tclRingBufferA[writeIndex].store(*cmd, std::memory_order::relaxed);
			writeIndex++;
			writeIndex &= (TCL_RING_BUFFER_SIZE - 1);
			cmd++;
			cmdLen--;
		}

		tclRingBufferA_writeIndex.store(writeIndex, std::memory_order::release);
	}

	#define EVENT_TYPE_TS		5

	void TCLSubmitRetireMarker(bool triggerEventInterrupt)
	{
		s_currentRetireMarker++;
		uint32be cmd[6];
		cmd[0] = pm4HeaderType3(IT_EVENT_WRITE_EOP, 5);
		cmd[1] = (4 | (EVENT_TYPE_TS << 8)); // event type (bits 8-15) and event index (bits 0-7).
		cmd[2] = MEMPTR<void>(&s_tclStatePPC->gpuRetireMarker).GetMPTR(); // address lower 32bits + data sel bits
		cmd[3] = 0x40000000; // select 64bit write, lower 16 bits are the upper bits of the address
		if (triggerEventInterrupt)
			cmd[3] |= 0x2000000; // trigger interrupt after value has been written
		cmd[4] = (uint32)s_currentRetireMarker; // data lower 32 bits
		cmd[5] = (uint32)(s_currentRetireMarker>>32); // data higher 32 bits
		TCLWriteCmd(cmd, 6);
	}

	int TCLSubmitToRing(uint32be* cmd, uint32 cmdLen, betype<TCLSubmissionFlag>* controlFlags, uint64be* timestampValueOut)
	{
		TCLSubmissionFlag flags = *controlFlags;
		cemu_assert_debug(timestampValueOut); // handle case where this is null

		// make sure there is enough space to submit all commands at one
		uint32 totalCommandLength = cmdLen;
		totalCommandLength += 6; // space needed for TCLSubmitRetireMarker

		TCLWaitForRBSpace(totalCommandLength);

		// submit command buffer
		TCLWriteCmd(cmd, cmdLen);

		// create new marker timestamp and tell GPU to write it to our variable after its done processing the command
		if ((HAS_FLAG(flags, TCLSubmissionFlag::USE_RETIRED_MARKER)))
		{
			TCLSubmitRetireMarker(!HAS_FLAG(flags, TCLSubmissionFlag::NO_MARKER_INTERRUPT));
			*timestampValueOut = s_currentRetireMarker; // incremented before each submit
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return 0;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "tcl";
		}

		void RPLMapped() override
		{
			cafeExportRegister("TCL", TCLSubmitToRing, LogType::Placeholder);
			cafeExportRegister("TCL", TCLTimestamp, LogType::Placeholder);
			cafeExportRegister("TCL", TCLWaitTimestamp, LogType::Placeholder);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
				coreinit::OSInitEvent(s_updateRetirementEvent.GetPtr(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
			}
		}
	}s_COStclModule;

	COSModule* GetModule()
	{
		return &s_COStclModule;
	}
}
