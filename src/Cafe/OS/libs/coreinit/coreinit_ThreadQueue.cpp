#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/gx2/GX2GuestFrameTiming.h" // Layer 2: TEMP diagnostic, guest-internal queueAndWait timing

namespace coreinit
{

	// Layer 4 TEMP diagnostic (behavior-neutral): if the thread being woken is the GX2 producer
	// (critical-path) thread, record who performs the wake - the current thread's guest id + name +
	// core - plus a per-frame wake count. This is the "other side" of the producer's queueAndWait:
	// it names the thread/path that satisfies the wait. All callers already hold the scheduler lock,
	// so OSGetCurrentThread()/OSGetCoreId() are valid here; relaxed-atomic stores + a pointer compare
	// only. NOT core-gated (the waker may run on any core - that is the point). No behavior change.
	static void _diagRecordWakeIfProducer(OSThread_t* wokenThread)
	{
		if (!GX2::IsProducerThread((void*)wokenThread))
			return;
		OSThread_t* waker = coreinit::OSGetCurrentThread();
		GX2::RecordProducerWake(waker ? (uint32)(uint16)waker->id : 0u,
			waker ? waker->threadName.GetPtr() : nullptr,
			(uint32)coreinit::OSGetCoreId());
	}

	// puts the thread on the waiting queue and changes state to WAITING
	// relinquishes timeslice
	// always uses thread->waitQueueLink
	void OSThreadQueueInternal::queueAndWait(OSThread_t* thread, uint32 diagCaller)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(thread->waitQueueLink.next == nullptr && thread->waitQueueLink.prev == nullptr);
		thread->currentWaitQueue = this;
		this->addThreadByPriority(thread, &thread->waitQueueLink);
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING);
		thread->state = OSThread_t::THREAD_STATE::STATE_WAITING;
		// Layer 2 TEMP diagnostic (behavior-neutral): bracket ONLY the actual block. On Cemu's fiber-based
		// scheduler the host C++ stack frame (and _diagT0) is preserved across the switch, so t1-t0 is the
		// wall-clock the current guest thread was descheduled. Main-core gated inside AddGuestWaitTsc, so this
		// is a no-op accumulate for every non-main-core thread. See GX2GuestFrameTiming.h for the overlap note.
		const uint64 _diagT0 = PPCTimer_getRawTsc();
		PPCCore_switchToSchedulerWithLock();
		GX2::AddGuestWaitTsc(GX2::GuestWaitCategory::GuestInternal, _diagT0);
		// Layer 3 TEMP diagnostic (behavior-neutral): decompose the SAME block by caller tag and by
		// producer/other thread. Keeps the Layer-2 total above intact; main-core gated inside the helper.
		// Layer 4 TEMP diagnostic: also pass the blocking thread's guest id + name so the producer's
		// single longest wait can be attributed to an exact primitive + thread (helper is main-core gated).
		GX2::AddGuestInternalBreakdown(diagCaller, _diagT0, (void*)thread,
			thread ? (uint32)(uint16)thread->id : 0u,
			thread ? thread->threadName.GetPtr() : nullptr);
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING);
	}

	void OSThreadQueueInternal::queueOnly(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(thread->waitQueueLink.next == nullptr && thread->waitQueueLink.prev == nullptr);
		thread->currentWaitQueue = this;
		this->addThreadByPriority(thread, &thread->waitQueueLink);
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING);
		thread->state = OSThread_t::THREAD_STATE::STATE_WAITING;
	}

	// remove thread from wait queue and wake it up
	void OSThreadQueueInternal::cancelWait(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		this->removeThread(thread, &thread->waitQueueLink);
		thread->state = OSThread_t::THREAD_STATE::STATE_READY;
		thread->currentWaitQueue = nullptr;
		coreinit::__OSAddReadyThreadToRunQueue(thread);
		_diagRecordWakeIfProducer(thread); // Layer 4 TEMP diagnostic (behavior-neutral)
		// todo - if waking up a thread on the same core with higher priority, reschedule
	}

	void OSThreadQueueInternal::addThread(OSThread_t* thread, OSThreadLink* threadLink)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		size_t linkOffset = getLinkOffset(thread, threadLink);
		// insert after tail
		if (tail.IsNull())
		{
			threadLink->next = nullptr;
			threadLink->prev = nullptr;
			head = thread;
			tail = thread;
		}
		else
		{
			threadLink->next = nullptr;
			threadLink->prev = tail;
			_getThreadLink(tail.GetPtr(), linkOffset)->next = thread;
			tail = thread;
		}
		_debugCheckChain(thread, threadLink);
	}

	void OSThreadQueueInternal::addThreadByPriority(OSThread_t* thread, OSThreadLink* threadLink)
	{
		cemu_assert_debug(tail.IsNull() == head.IsNull()); // either must be set or none at all
		cemu_assert_debug(__OSHasSchedulerLock());
		size_t linkOffset = getLinkOffset(thread, threadLink);
		if (tail.IsNull())
		{
			threadLink->next = nullptr;
			threadLink->prev = nullptr;
			head = thread;
			tail = thread;
		}
		else
		{
			// insert towards tail based on priority
			OSThread_t* threadItr = tail.GetPtr();
			while (threadItr && threadItr->effectivePriority > thread->effectivePriority)
				threadItr = _getThreadLink(threadItr, linkOffset)->prev.GetPtr();
			if (threadItr == nullptr)
			{
				// insert in front
				threadLink->next = head;
				threadLink->prev = nullptr;
				_getThreadLink(head.GetPtr(), linkOffset)->prev = thread;
				head = thread;
			}
			else
			{
				threadLink->prev = threadItr;
				threadLink->next = _getThreadLink(threadItr, linkOffset)->next;
				if (_getThreadLink(threadItr, linkOffset)->next)
				{
					OSThread_t* threadAfterItr = _getThreadLink(threadItr, linkOffset)->next.GetPtr();
					_getThreadLink(threadAfterItr, linkOffset)->prev = thread;
					_getThreadLink(threadItr, linkOffset)->next = thread;
				}
				else
				{
					tail = thread;
					_getThreadLink(threadItr, linkOffset)->next = thread;
				}
			}
		}
		_debugCheckChain(thread, threadLink);
	}

	void OSThreadQueueInternal::removeThread(OSThread_t* thread, OSThreadLink* threadLink)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		size_t linkOffset = getLinkOffset(thread, threadLink);
		_debugCheckChain(thread, threadLink);
		if (threadLink->prev)
			_getThreadLink(threadLink->prev.GetPtr(), linkOffset)->next = threadLink->next;
		else
			head = threadLink->next;
		if (threadLink->next)
			_getThreadLink(threadLink->next.GetPtr(), linkOffset)->prev = threadLink->prev;
		else
			tail = threadLink->prev;

		threadLink->next = nullptr;
		threadLink->prev = nullptr;
	}

	// counterpart for queueAndWait
	// if reschedule is true then scheduler will switch to woken up thread (if it is runnable on the same core)
	// sharedPriorityAndAffinityWorkaround is currently a hack/placeholder for some special cases. A proper fix likely involves handling all the nuances of thread effective priority
	void OSThreadQueueInternal::wakeupEntireWaitQueue(bool reschedule, bool sharedPriorityAndAffinityWorkaround)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		bool shouldReschedule = false;
		while (OSThread_t* thread = takeFirstFromQueue(offsetof(OSThread_t, waitQueueLink)))
		{
			cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_WAITING);
			//cemu_assert_debug(thread->suspendCounter == 0);
			thread->state = OSThread_t::THREAD_STATE::STATE_READY;
			thread->currentWaitQueue = nullptr;
			coreinit::__OSAddReadyThreadToRunQueue(thread);
			_diagRecordWakeIfProducer(thread); // Layer 4 TEMP diagnostic (behavior-neutral)
			if (reschedule && thread->suspendCounter == 0 && PPCInterpreter_getCurrentInstance() && __OSCoreShouldSwitchToThread(coreinit::OSGetCurrentThread(), thread, sharedPriorityAndAffinityWorkaround))
				shouldReschedule = true;
		}
		if (shouldReschedule)
			PPCCore_switchToSchedulerWithLock();
	}

	// counterpart for queueAndWait
	// if reschedule is true then scheduler will switch to woken up thread (if it is runnable on the same core)
	void OSThreadQueueInternal::wakeupSingleThreadWaitQueue(bool reschedule, bool sharedPriorityAndAffinityWorkaround)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		OSThread_t* thread = takeFirstFromQueue(offsetof(OSThread_t, waitQueueLink));
		cemu_assert_debug(thread);
		bool shouldReschedule = false;
		if (thread)
		{
			thread->state = OSThread_t::THREAD_STATE::STATE_READY;
			thread->currentWaitQueue = nullptr;
			coreinit::__OSAddReadyThreadToRunQueue(thread);
			_diagRecordWakeIfProducer(thread); // Layer 4 TEMP diagnostic (behavior-neutral)
			if (reschedule && thread->suspendCounter == 0 && PPCInterpreter_getCurrentInstance() && __OSCoreShouldSwitchToThread(coreinit::OSGetCurrentThread(), thread, sharedPriorityAndAffinityWorkaround))
				shouldReschedule = true;
		}
		if (shouldReschedule)
			PPCCore_switchToSchedulerWithLock();
	}

}
