#pragma once

#include <chrono>

namespace nsyshid
{
	// Per-figure persistence throttle shared by the emulated toy portals
	// (Skylanders / Disney Infinity / LEGO Dimensions).
	//
	// The original portals call Save() on EVERY block the game writes, which
	// flushes the whole figure buffer to disk on each 16/4-byte poke. This
	// batches those writes: the in-memory buffer is still updated on every write
	// (so emulation reads always see current data), but the disk write is rate
	// limited to at most once per configured interval.
	//
	// Correctness contract (no progress may ever be lost by batching):
	//  * A disk write only happens when there is a REAL pending change. An
	//    unchanged (no-op) write never marks the figure dirty and never writes.
	//  * intervalMs == 0 means "Every Time": every real change writes immediately,
	//    which is byte-for-byte the original behavior.
	//  * A pending change is force-written on lifecycle boundaries (figure
	//    removed/moved) and on app teardown via FlushPending(), so a change that
	//    has not yet hit its interval is never stranded in memory.
	//
	// Each figure owns its own throttle instance; there is no shared/global timer.
	class PortalSaveThrottle
	{
	  public:
		// Call after mutating the in-memory buffer. `changed` is whether the write
		// actually altered the buffer. `intervalMs` is the configured throttle
		// (0 = save on every change). Returns true when the caller should write
		// the figure to disk now.
		bool ShouldSaveOnChange(bool changed, uint32 intervalMs)
		{
			if (!changed)
				return false;
			m_dirty = true;
			if (intervalMs == 0)
				return TakePending();
			const auto now = std::chrono::steady_clock::now();
			if (now - m_lastSave >= std::chrono::milliseconds(intervalMs))
				return TakePending();
			return false; // keep it pending until the interval elapses or a flush occurs
		}

		// Force any pending change out now (lifecycle event / teardown). Returns
		// true when there is a pending change the caller should write to disk.
		bool FlushPending()
		{
			if (!m_dirty)
				return false;
			return TakePending();
		}

		[[nodiscard]] bool HasPending() const { return m_dirty; }

		// Call when a figure is (re)loaded into a slot: no pending change yet and
		// the interval window starts now.
		void Reset()
		{
			m_dirty = false;
			m_lastSave = std::chrono::steady_clock::now();
		}

	  private:
		bool TakePending()
		{
			m_dirty = false;
			m_lastSave = std::chrono::steady_clock::now();
			return true;
		}

		bool m_dirty = false;
		std::chrono::steady_clock::time_point m_lastSave = std::chrono::steady_clock::now();
	};
} // namespace nsyshid
