#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

void LatteThread_Exit();

SlimRWLock swl_gpuAsyncCommands;

typedef struct  
{
	uint32 type;
	union
	{
		struct
		{
			MPTR physAddr;
			MPTR mipAddr;
			uint32 swizzle;
			sint32 format;
			sint32 width;
			sint32 height;
			sint32 depth;
			uint32 pitch;
			uint32 slice;
			sint32 dim;
			Latte::E_HWTILEMODE tilemode;
			sint32 aa;
			sint32 level;
		}forceTextureReadback;

		struct
		{
			uint64 shaderBaseHash; 
			uint64 shaderAuxHash; 
			LatteConst::ShaderType shaderType;
		}deleteShader;

		struct
		{
			LatteSurfaceCopyParam src;
			LatteSurfaceCopyParam dst;
			LatteSurfaceCopyRect rect;
		}textureCopy;
	};
}LatteAsyncCommand_t;

#define ASYNC_CMD_FORCE_TEXTURE_READBACK		1
#define ASYNC_CMD_DELETE_SHADER					2
#define ASYNC_CMD_TEXTURE_COPY					3

std::queue<LatteAsyncCommand_t> LatteAsyncCommandQueue;

void LatteAsyncCommands_queueForceTextureReadback(MPTR physAddr, MPTR mipAddr, uint32 swizzle, sint32 format, sint32 width, sint32 height, sint32 depth, uint32 pitch, uint32 slice, sint32 dim, Latte::E_HWTILEMODE tilemode, sint32 aa, sint32 level)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_FORCE_TEXTURE_READBACK;
	
	asyncCommand.forceTextureReadback.physAddr = physAddr;
	asyncCommand.forceTextureReadback.mipAddr = mipAddr;
	asyncCommand.forceTextureReadback.swizzle = swizzle;
	asyncCommand.forceTextureReadback.format = format;
	asyncCommand.forceTextureReadback.width = width;
	asyncCommand.forceTextureReadback.height = height;
	asyncCommand.forceTextureReadback.depth = depth;
	asyncCommand.forceTextureReadback.pitch = pitch;
	asyncCommand.forceTextureReadback.slice = slice;
	asyncCommand.forceTextureReadback.dim = dim;
	asyncCommand.forceTextureReadback.tilemode = tilemode;
	asyncCommand.forceTextureReadback.aa = aa;
	asyncCommand.forceTextureReadback.level = level;
	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_queueDeleteShader(uint64 shaderBaseHash, uint64 shaderAuxHash, LatteConst::ShaderType shaderType)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_DELETE_SHADER;

	asyncCommand.deleteShader.shaderBaseHash = shaderBaseHash;
	asyncCommand.deleteShader.shaderAuxHash = shaderAuxHash;
	asyncCommand.deleteShader.shaderType = shaderType;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommand_queueTextureCopy(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	LatteAsyncCommand_t asyncCommand = {};
	// setup command
	asyncCommand.type = ASYNC_CMD_TEXTURE_COPY;
	asyncCommand.textureCopy.src = src;
	asyncCommand.textureCopy.dst = dst;
	asyncCommand.textureCopy.rect = rect;

	swl_gpuAsyncCommands.LockWrite();
	LatteAsyncCommandQueue.push(asyncCommand);
	swl_gpuAsyncCommands.UnlockWrite();
}

void LatteAsyncCommands_waitUntilAllProcessed()
{
	// Blocks the calling (guest CPU) thread until the Latte GPU thread has drained the async
	// command queue. It does no work itself - it is a pure cross-thread stall - so on
	// ARM64/iOS a bare _mm_pause() spin burns a whole host core while merely waiting. Ramp
	// from a short pause-spin (keeps the common microsecond-latency drain fast) through a
	// brief yield window into small bounded sleeps, so a longer drain stops consuming a core.
	// The queue is re-checked every iteration and the loop exits the instant it is empty, so
	// no sleep is inserted once draining has completed. No lock is held while sleeping, so the
	// GPU thread keeps draining freely (no deadlock), and the cap is small enough that the
	// caller (a synchronous GX2 surface copy) is never materially delayed.
	static constexpr uint32 kAsyncDrainSpinIterations  = 64;  // pause-spin for microsecond-latency pickup
	static constexpr uint32 kAsyncDrainYieldIterations = 32;  // yield-only window before sleeping
	static constexpr uint32 kAsyncDrainBackoffStepUs   = 50;  // extra sleep granted per further waiting iteration
	static constexpr uint32 kAsyncDrainBackoffMaxUs    = 250; // hard cap so the copy is never materially delayed
	uint32 waitIterations = 0;
	while (LatteAsyncCommandQueue.empty() == false)
	{
		if (waitIterations < kAsyncDrainSpinIterations)
		{
			_mm_pause();
		}
		else if (waitIterations < kAsyncDrainSpinIterations + kAsyncDrainYieldIterations)
		{
			std::this_thread::yield();
		}
		else
		{
			uint32 sleepUs = (waitIterations - (kAsyncDrainSpinIterations + kAsyncDrainYieldIterations) + 1) * kAsyncDrainBackoffStepUs;
			if (sleepUs > kAsyncDrainBackoffMaxUs)
				sleepUs = kAsyncDrainBackoffMaxUs;
			std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
		}
		waitIterations++;
	}
}

/*
 * Called by the GPU command processor frequently
 */
void LatteAsyncCommands_checkAndExecute()
{
	// quick check if queue is empty (requires no lock)
	if (Latte_GetStopSignal())
		LatteThread_Exit();
	if (LatteAsyncCommandQueue.empty())
		return;
	swl_gpuAsyncCommands.LockWrite();
	while (LatteAsyncCommandQueue.empty() == false)
	{
		// get first command in queue
		LatteAsyncCommand_t asyncCommand = LatteAsyncCommandQueue.front();
		swl_gpuAsyncCommands.UnlockWrite();
		if (asyncCommand.type == ASYNC_CMD_FORCE_TEXTURE_READBACK)
		{
			cemu_assert_debug(asyncCommand.forceTextureReadback.level == 0); // implement mip swizzle and verify
			LatteTextureView* textureView = LatteTC_GetTextureSliceViewOrTryCreate(asyncCommand.forceTextureReadback.physAddr, asyncCommand.forceTextureReadback.mipAddr, (Latte::E_GX2SURFFMT)asyncCommand.forceTextureReadback.format, asyncCommand.forceTextureReadback.tilemode, asyncCommand.forceTextureReadback.width, asyncCommand.forceTextureReadback.height, asyncCommand.forceTextureReadback.depth, asyncCommand.forceTextureReadback.pitch, 0, asyncCommand.forceTextureReadback.slice, asyncCommand.forceTextureReadback.level);
			if (textureView != nullptr)
			{
				LatteTexture_UpdateDataToLatest(textureView->baseTexture);
				// start transfer
				LatteTextureReadback_StartTransfer(textureView);
				// wait until finished
				LatteTextureReadback_UpdateFinishedTransfers(true);
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "Texture not found for readback");
			}
		}
		else if (asyncCommand.type == ASYNC_CMD_DELETE_SHADER)
		{
			LatteSHRC_RemoveFromCacheByHash(asyncCommand.deleteShader.shaderBaseHash, asyncCommand.deleteShader.shaderAuxHash, asyncCommand.deleteShader.shaderType);
		}
		else if (asyncCommand.type == ASYNC_CMD_TEXTURE_COPY)
		{
			LatteSurfaceCopy_copySurfaceNew(asyncCommand.textureCopy.src, asyncCommand.textureCopy.dst, asyncCommand.textureCopy.rect);
		}
		else
		{
			cemu_assert_unimplemented();
		}
		swl_gpuAsyncCommands.LockWrite();
		LatteAsyncCommandQueue.pop();
	}
	swl_gpuAsyncCommands.UnlockWrite();
}
