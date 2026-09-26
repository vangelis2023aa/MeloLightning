#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"
#include "util/helpers/Semaphore.h"
#include <thread>
#include <atomic>

//#define BENCHMARK_TEXTURE_DECODING		// if defined, time it takes to decode textures will be measured and logged to log.txt

#ifdef BENCHMARK_TEXTURE_DECODING
uint64 textureDecodeBenchmark_perFormatSum[0x40] = { 0 }; // duration sum per texture format (hw format) - in microseconds
uint64 textureDecodeBenchmark_totalSum = 0;
#endif

namespace
{
	// Experimental Decode Cache (default OFF; gated by ActiveSettings::ExperimentalDecodeCache()).
	// The CPU texture decode (untile + format convert) is a pure function of (source bytes, decode
	// parameters), so when the exact same source bytes and parameters recur we can memcpy a previously
	// decoded result instead of running the untile/convert again. This helps content that alternates
	// between a few images (double-buffered sampler targets, animated UI) and textures that are evicted
	// then re-faulted with identical bytes. Touched only from the single Latte thread (inside
	// LatteTextureLoader_UpdateTextureSliceData), so it needs no locking. Bounded by a byte budget with
	// LRU eviction. All 18 fields are uint32 (no padding) so the struct is memcmp/hash safe.
	struct DecodeCacheParams
	{
		uint32 format, dim, width, height, depth, pitch, tileMode, pipeSwizzle, bankSwizzle;
		uint32 bpp, sliceIndex, mipIndex, surfaceInfoHeight, surfaceInfoDepth, imageSize;
		uint32 decodedTexelCountX, decodedTexelCountY, isDepth;
		bool operator==(const DecodeCacheParams& o) const { return memcmp(this, &o, sizeof(DecodeCacheParams)) == 0; }
	};
	class DecodeCache
	{
		struct Entry
		{
			uint64 verify;
			DecodeCacheParams params;
			std::vector<uint8> data;
			uint64 lastUse;
		};
		std::unordered_map<uint64, Entry> m_entries; // keyed by primary hash h0; verify(h1)+params guard against collisions
		uint64 m_useCounter = 0;
		size_t m_totalBytes = 0;
		static constexpr size_t kMaxBytes = 64 * 1024 * 1024; // 64 MiB budget
	public:
		bool tryGet(uint64 h0, uint64 h1, const DecodeCacheParams& p, uint8* out, uint32 size)
		{
			auto it = m_entries.find(h0);
			if (it == m_entries.end())
				return false;
			Entry& e = it->second;
			if (e.verify != h1 || e.data.size() != size || !(e.params == p))
				return false; // h0 collision on different content/params -> treat as miss (never returns wrong bytes)
			memcpy(out, e.data.data(), size);
			e.lastUse = ++m_useCounter;
			return true;
		}
		void put(uint64 h0, uint64 h1, const DecodeCacheParams& p, const uint8* data, uint32 size)
		{
			if (size == 0 || size > kMaxBytes)
				return;
			auto it = m_entries.find(h0);
			if (it != m_entries.end())
				m_totalBytes -= it->second.data.size();
			Entry& e = m_entries[h0];
			e.verify = h1;
			e.params = p;
			e.data.assign(data, data + size);
			e.lastUse = ++m_useCounter;
			m_totalBytes += size;
			while (m_totalBytes > kMaxBytes && m_entries.size() > 1)
				evictOldest();
		}
	private:
		void evictOldest()
		{
			auto oldest = m_entries.begin();
			for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
				if (it->second.lastUse < oldest->second.lastUse)
					oldest = it;
			m_totalBytes -= oldest->second.data.size();
			m_entries.erase(oldest);
		}
	};
	DecodeCache g_textureDecodeCache;
	// Two independent 64-bit hashes over the slice source bytes (one pass), giving a 128-bit content
	// key; the decode parameters are then folded in so identical bytes with different decode params
	// never collide. h1 (with params) is stored as the verify hash and re-checked on every hit.
	inline void DecodeCache_HashSource(const uint8* data, size_t len, uint64& h0, uint64& h1)
	{
		h0 = 1469598103934665603ULL;
		h1 = 0x9E3779B97F4A7C15ULL;
		size_t i = 0;
		const size_t len8 = len & ~size_t(7);
		for (; i < len8; i += 8)
		{
			uint64 v;
			memcpy(&v, data + i, sizeof(uint64));
			h0 = (h0 ^ v) * 1099511628211ULL;
			h1 += v;
			h1 = ((h1 << 31) | (h1 >> 33)) * 0xFF51AFD7ED558CCDULL;
		}
		for (; i < len; ++i)
		{
			h0 = (h0 ^ data[i]) * 1099511628211ULL;
			h1 = (h1 ^ data[i]) * 0x100000001B3ULL;
		}
	}

	inline void DecodeCache_FoldParams(const DecodeCacheParams& p, uint64& h0, uint64& h1)
	{
		const uint32* w = reinterpret_cast<const uint32*>(&p);
		for (size_t i = 0; i < sizeof(DecodeCacheParams) / sizeof(uint32); ++i)
		{
			h0 = (h0 ^ w[i]) * 1099511628211ULL;
			h1 = (h1 + w[i] + 0x9E3779B9u);
			h1 = ((h1 << 27) | (h1 >> 37)) * 0xC2B2AE3D27D4EB4FULL;
		}
	}

	// ---- Experimental Worker-Thread Texture Decode (default OFF; ExperimentalWorkerTextureDecode) ----
	// A tiny fixed-size, process-lifetime fork-join pool used ONLY to run the pure, reentrant CPU decode
	// (untile + format convert) of independent (slice,mip) units in parallel. Each unit owns its own
	// LatteTextureLoaderCtx and its own output buffer; guest memory is read-only during decode; the
	// decoders and the tiling-address math (ComputeSurfaceAddrFromCoordMacroTiledCached* + the per-ctx
	// CachedSurfaceAddrInfo, incl. its microTilePixelIndexTable) hold no shared mutable state. The pool
	// joins fully before the caller uploads anything, so a decoded slice is never visible to the renderer
	// before its decode completed. The calling (Latte) thread participates as a worker, so forward
	// progress holds even if the helper threads are descheduled. Helper count is clamped small to bound
	// how many cores are lit at once (thermal budget). Driven only from the single Latte thread.
	class TexDecodeForkJoin
	{
	public:
		using JobFn = void(*)(void* ctx, size_t index);

		// Runs fn(ctx, i) for i in [0,count), blocking until all indices have completed.
		void run(size_t count, JobFn fn, void* ctx)
		{
			if (count == 0)
				return;
			ensureStarted();
			m_fn = fn;
			m_ctx = ctx;
			m_total = count;
			m_nextIndex.store(0, std::memory_order_relaxed);
			// Publish the job, then release one work token per helper. Semaphore notify/wait synchronize
			// on the same mutex, so the stores above happen-before a helper observes its token.
			for (uint32 i = 0; i < m_helperCount; i++)
				m_workSem.notify();
			drain(); // the Latte thread pulls indices too (guaranteed progress if helpers are asleep)
			// Full join barrier: every helper must report done before we touch any output buffer.
			for (uint32 i = 0; i < m_helperCount; i++)
				m_doneSem.wait();
			m_fn = nullptr;
			m_ctx = nullptr;
		}

	private:
		void drain()
		{
			for (;;)
			{
				size_t i = m_nextIndex.fetch_add(1, std::memory_order_relaxed);
				if (i >= m_total)
					break;
				m_fn(m_ctx, i);
			}
		}

		void helperLoop()
		{
			for (;;)
			{
				m_workSem.wait();
				drain();
				m_doneSem.notify();
			}
		}
		void ensureStarted()
		{
			if (m_started)
				return;
			uint32 hw = std::thread::hardware_concurrency();
			uint32 helpers = (hw > 1) ? (hw - 1) : 1;
			if (helpers > 3)
				helpers = 3; // bound cores lit at once; with the Latte thread this is helpers+1 wide
			m_helperCount = helpers;
			for (uint32 i = 0; i < m_helperCount; i++)
				std::thread(&TexDecodeForkJoin::helperLoop, this).detach();
			m_started = true;
		}

		bool m_started = false;
		uint32 m_helperCount = 0;
		Semaphore m_workSem;
		Semaphore m_doneSem;
		std::atomic<size_t> m_nextIndex{0};
		size_t m_total = 0;
		JobFn m_fn = nullptr;
		void* m_ctx = nullptr;
	};

	// Intentionally leaked (process lifetime): the detached helper threads block on this pool's
	// semaphores forever, so destroying its sync primitives at static teardown would be UB. Only the
	// Latte thread ever calls this, so the local-static init is not contended.
	TexDecodeForkJoin& GetTexDecodeForkJoin()
	{
		static TexDecodeForkJoin* s_pool = new TexDecodeForkJoin();
		return *s_pool;
	}

	// One independent decode unit (a single slice of a single mip).
	struct SliceDecodeJob
	{
		LatteTextureLoaderCtx ctx{}; // zero-init to match the serial `LatteTextureLoaderCtx = {0}`
		TextureDecoder* decoder = nullptr;
		uint32 sliceIndex = 0;
		uint32 mipIndex = 0;
		sint32 imageSize = 0;
		std::vector<uint8> buffer;
		bool skipDecode = false;   // decode-cache (M1) hit: buffer already holds the decoded bytes
		bool needCachePut = false; // decode-cache (M1) miss: store buffer after the join
		uint64 cacheH0 = 0, cacheH1 = 0;
		DecodeCacheParams cacheParams{};
	};

	// Worker body: pure decode into the job's own buffer. Touches no shared state (no cache, no upload
	// buffer, no renderer) — safe to run on any thread concurrently.
	void TexDecodeJobFn(void* ctxPtr, size_t index)
	{
		auto* jobs = reinterpret_cast<std::vector<SliceDecodeJob>*>(ctxPtr);
		SliceDecodeJob& j = (*jobs)[index];
		if (!j.skipDecode)
			j.decoder->decode(&j.ctx, j.buffer.data());
	}
}



void LatteTextureLoader_begin(LatteTextureLoaderCtx* textureLoader, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle)
{
	textureLoader->physAddress = physImagePtr;
	textureLoader->physMipAddress = physMipPtr;
	textureLoader->sliceIndex = sliceIndex;
	cemu_assert_debug(mipLevels != 0);
	textureLoader->mipLevels = std::max<uint32>(1, mipLevels);
	textureLoader->tileMode = tileMode;
	textureLoader->bpp = Latte::GetFormatBits(format);
	textureLoader->stepX = 1;
	textureLoader->stepY = 1;
	if (Latte::IsCompressedFormat(format))
	{
		textureLoader->stepX = 4;
		textureLoader->stepY = 4;
	}

	textureLoader->pipeSwizzle = (swizzle >> 8) & 1;
	textureLoader->bankSwizzle = ((swizzle >> 9) & 3);

	uint32 surfaceAA = 0; // todo

	if (mipIndex > 0 && Latte::TM_IsMacroTiled(tileMode))
	{
		// separate swizzle from mip pointer if mip chain is not macro-tiled (and thus not swizzled)
		LatteAddrLib::AddrSurfaceInfo_OUT surfaceInfo;
		LatteAddrLib::GX2CalculateSurfaceInfo(format, width, height, depth, dim, Latte::MakeGX2TileMode(tileMode), surfaceAA, 1, &surfaceInfo);
		if (Latte::TM_IsMacroTiled(surfaceInfo.hwTileMode))
		{
			uint32 mipSwizzle = physMipPtr&0x700;
			physMipPtr &= ~0x700;
			textureLoader->physMipAddress = physMipPtr;
			textureLoader->pipeSwizzle = (mipSwizzle >> 8) & 1;
			textureLoader->bankSwizzle = ((mipSwizzle >> 9) & 3);
		}
	}

	// calculate surface info
	uint32 level = mipIndex;
	LatteAddrLib::AddrSurfaceInfo_OUT surfaceInfo;
	LatteAddrLib::GX2CalculateSurfaceInfo(format, width, height, depth, dim, Latte::MakeGX2TileMode(tileMode), surfaceAA, level, &surfaceInfo);
	textureLoader->levelOffset = LatteAddrLib::CalculateMipOffset(format, width, height, depth, dim, (Latte::E_HWTILEMODE)tileMode, swizzle, surfaceAA, level);
	textureLoader->tileMode = surfaceInfo.hwTileMode;

	textureLoader->minOffsetOutdated = 0;
	textureLoader->maxOffsetOutdated = (sint32)surfaceInfo.surfSize;

	textureLoader->surfaceInfoHeight = surfaceInfo.height;
	textureLoader->surfaceInfoDepth = surfaceInfo.depth;

	// correct handling for LINEAR_ALIGNED pitch alignment is still not fully understood:
	//seems like sometimes there is a conditional pitch alignment to 0x40 OR there is no pitch alignment at all and we have a bug somewhere else

	uint64 titleId = CafeSystem::GetForegroundTitleId();
	titleId &= ~0x300ULL;

	if (tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED && titleId == (0x000500301001200aULL))
	{
		// examples of titles that use linear textures:
		// Minecraft - Uses sprite atlases with mips and linear tilemode. Expects padding of pitch for smaller mips to be 0x40
		// Browser - Linear pitch must be used as-is, padding/alignment will break textures (uses a weird way to calculate pitch by using GX2CalcSurface on a texture with tileMode 0/4)
		// BotW - uses linear textures as render targets. With the smallest resolution being 3x3 with no pitch alignment expected at all (pitch = 3)? -> Not possible because both textures and rendertargets require a minimum alignment of 8 for pitch?
		surfaceInfo.pitch = std::max<uint32>(1, pitch >> mipIndex);
	}


	textureLoader->width = width >> (mipIndex);
	textureLoader->width = std::max(textureLoader->width, 1);
	textureLoader->height = height >> (mipIndex);
	textureLoader->height = std::max(textureLoader->height, 1);

	textureLoader->pitch = surfaceInfo.pitch;
	// calculate start address
	if (level == 0)
		textureLoader->inputData = (uint8*)memory_getPointerFromPhysicalOffset(physImagePtr);
	else
		textureLoader->inputData = (uint8*)memory_getPointerFromPhysicalOffset(physMipPtr) + textureLoader->levelOffset;

	SetupCachedSurfaceAddrInfo(&textureLoader->computeAddrInfo, textureLoader->sliceIndex, 0, textureLoader->bpp, textureLoader->pitch, surfaceInfo.height, depth, 1 * 1, textureLoader->tileMode, false, textureLoader->pipeSwizzle, textureLoader->bankSwizzle);
}

uint8* LatteTextureLoader_GetInput(LatteTextureLoaderCtx* textureLoader, sint32 x, sint32 y)
{
	// calculate address of input tile
	uint32 offset = 0;
	if (textureLoader->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || textureLoader->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
		offset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x / textureLoader->stepX, y / textureLoader->stepY, textureLoader->sliceIndex, 0, textureLoader->bpp, textureLoader->pitch, textureLoader->surfaceInfoHeight, textureLoader->surfaceInfoDepth);
	else if (textureLoader->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THIN1 || textureLoader->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THICK)
		offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x / textureLoader->stepX, y / textureLoader->stepY, textureLoader->sliceIndex, textureLoader->bpp, textureLoader->pitch, textureLoader->surfaceInfoHeight, (Latte::E_HWTILEMODE)textureLoader->tileMode, false);
	else
		offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x / textureLoader->stepX, y / textureLoader->stepY, &textureLoader->computeAddrInfo);
	uint8* blockData = textureLoader->inputData + offset;
	return blockData;
}

/*
 * Optimized version which assumes tileMode == 1
 * Also does not do any min/max offset tracking
 */
uint8* LatteTextureLoader_getInputLinearOptimized(LatteTextureLoaderCtx* textureLoader, sint32 x, sint32 y)
{
	// calculate address of input tile
	uint32 bitPos = 0;
	uint32 offset = 0;
	offset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x / textureLoader->stepX, y / textureLoader->stepY, textureLoader->sliceIndex, 0, textureLoader->bpp, textureLoader->pitch, textureLoader->surfaceInfoHeight, textureLoader->surfaceInfoDepth);
	return textureLoader->inputData + offset;
}

#define LatteTextureLoader_getInputLinearOptimized_(__textureLoader,__x,__y,__stepX,__stepY,__bpp,__sliceIndex,__numSlices,__sample,__pitch,__height) (textureLoader->inputData+((__x/__stepX) + __pitch * (__y/__stepY) + (__sliceIndex + __numSlices * __sample) * __height * __pitch)*(__bpp/8))

void decodeBC1Block(uint8* inputData, float* output4x4RGBA)
{
	// read colors
	uint16 c0 = *(uint16*)(inputData + 0);
	uint16 c1 = *(uint16*)(inputData + 2);
	// decode colors (RGB565 -> RGB888)
	float r[4];
	float g[4];
	float b[4];
	float a[4];
	b[0] = (float)((c0 >> 0) & 0x1F) / 31.0f;
	b[1] = (float)((c1 >> 0) & 0x1F) / 31.0f;
	g[0] = (float)((c0 >> 5) & 0x3F) / 63.0f;
	g[1] = (float)((c1 >> 5) & 0x3F) / 63.0f;
	r[0] = (float)((c0 >> 11) & 0x1F) / 31.0f;
	r[1] = (float)((c1 >> 11) & 0x1F) / 31.0f;
	a[0] = 1.0f;
	a[1] = 1.0f;
	a[2] = 1.0f;

	if (c0 > c1)
	{
		r[2] = (r[0] * 2.0f + r[1]) / 3.0f;
		r[3] = (r[0] * 1.0f + r[1] * 2.0f) / 3.0f;
		g[2] = (g[0] * 2.0f + g[1]) / 3.0f;
		g[3] = (g[0] * 1.0f + g[1] * 2.0f) / 3.0f;
		b[2] = (b[0] * 2.0f + b[1]) / 3.0f;
		b[3] = (b[0] * 1.0f + b[1] * 2.0f) / 3.0f;
		a[3] = 1.0f;
	}
	else
	{
		r[2] = (r[0] + r[1]) / 2.0f;
		r[3] = 0.0f;
		g[2] = (g[0] + g[1]) / 2.0f;
		g[3] = 0.0f;
		b[2] = (b[0] + b[1]) / 2.0f;
		b[3] = 0.0f;
		a[3] = 0.0f;
	}

	uint8* indexData = inputData + 4;
	float* colorOutputRGBA = output4x4RGBA;
	for (sint32 row = 0; row < 4; row++)
	{
		uint8 i0 = ((*indexData) >> 0) & 3;
		uint8 i1 = ((*indexData) >> 2) & 3;
		uint8 i2 = ((*indexData) >> 4) & 3;
		uint8 i3 = ((*indexData) >> 6) & 3;
		colorOutputRGBA[0] = r[i0];
		colorOutputRGBA[1] = g[i0];
		colorOutputRGBA[2] = b[i0];
		colorOutputRGBA[3] = a[i0];
		colorOutputRGBA += 4;
		colorOutputRGBA[0] = r[i1];
		colorOutputRGBA[1] = g[i1];
		colorOutputRGBA[2] = b[i1];
		colorOutputRGBA[3] = a[i1];
		colorOutputRGBA += 4;
		colorOutputRGBA[0] = r[i2];
		colorOutputRGBA[1] = g[i2];
		colorOutputRGBA[2] = b[i2];
		colorOutputRGBA[3] = a[i2];
		colorOutputRGBA += 4;
		colorOutputRGBA[0] = r[i3];
		colorOutputRGBA[1] = g[i3];
		colorOutputRGBA[2] = b[i3];
		colorOutputRGBA[3] = a[i3];
		colorOutputRGBA += 4;
		indexData++;
	}
}

void decodeBC2Block_UNORM(uint8* inputData, float* imageRGBA)
{
	uint32 color0 = *(uint16*)(inputData + 8);
	uint32 color1 = *(uint16*)(inputData + 10);
	uint32 colorIndices = *(uint32*)(inputData + 12);

	uint8 r0 = (color0 >> 11) & 0x1F;
	uint8 g0 = (color0 >> 5) & 0x3F;
	uint8 b0 = (color0 >> 0) & 0x1F;

	uint8 r1 = (color1 >> 11) & 0x1F;
	uint8 g1 = (color1 >> 5) & 0x3F;
	uint8 b1 = (color1 >> 0) & 0x1F;

	float r[4];
	float g[4];
	float b[4];
	r[0] = (float)r0 / 31.0f;
	r[1] = (float)r1 / 31.0f;
	r[2] = (r[0] * 2.0f + r[1]) / 3.0f;
	r[3] = (r[0] + r[1] * 2.0f) / 3.0f;
	g[0] = (float)g0 / 63.0f;
	g[1] = (float)g1 / 63.0f;
	g[2] = (g[0] * 2.0f + g[1]) / 3.0f;
	g[3] = (g[0] + g[1] * 2.0f) / 3.0f;
	b[0] = (float)b0 / 31.0f;
	b[1] = (float)b1 / 31.0f;
	b[2] = (b[0] * 2.0f + b[1]) / 3.0f;
	b[3] = (b[0] + b[1] * 2.0f) / 3.0f;

	for (sint32 py = 0; py < 4; py++)
	{
		for (sint32 px = 0; px < 4; px++)
		{
			uint8 colorIndex = (colorIndices >> (2 * (px + 4 * py))) & 0x03;
			sint32 pixelOffset = (px + py * 4) * 4;
			imageRGBA[pixelOffset + 0] = r[colorIndex];
			imageRGBA[pixelOffset + 1] = g[colorIndex];
			imageRGBA[pixelOffset + 2] = b[colorIndex];
		}
	}

	// decode alpha
	uint8* alphaData = (uint8*)(inputData + 0);
	for (sint32 py = 0; py < 4; py++)
	{
		for (sint32 px = 0; px < 4; px++)
		{
			uint32 alphaIndex = (px + py * 4);
			uint8 alphaCode = (alphaData[alphaIndex / 2] >> ((alphaIndex & 1) * 4)) & 0xF;
			alphaCode |= (alphaCode << 4);
			sint32 pixelOffset = (px + py * 4) * 4;
			imageRGBA[pixelOffset + 3] = (float)alphaCode / 255.0f; // alpha
		}
	}
}

void decodeBC3Block_UNORM(uint8* inputData, float* imageRGBA)
{
	uint32 color0 = *(uint16*)(inputData + 8);
	uint32 color1 = *(uint16*)(inputData + 10);
	uint32 colorIndices = *(uint32*)(inputData + 12);

	uint8 r0 = (color0 >> 11) & 0x1F;
	uint8 g0 = (color0 >> 5) & 0x3F;
	uint8 b0 = (color0 >> 0) & 0x1F;

	uint8 r1 = (color1 >> 11) & 0x1F;
	uint8 g1 = (color1 >> 5) & 0x3F;
	uint8 b1 = (color1 >> 0) & 0x1F;

	float r[4];
	float g[4];
	float b[4];
	r[0] = (float)r0 / 31.0f;
	r[1] = (float)r1 / 31.0f;
	r[2] = (r[0] * 2.0f + r[1]) / 3.0f;
	r[3] = (r[0] + r[1] * 2.0f) / 3.0f;
	g[0] = (float)g0 / 63.0f;
	g[1] = (float)g1 / 63.0f;
	g[2] = (g[0] * 2.0f + g[1]) / 3.0f;
	g[3] = (g[0] + g[1] * 2.0f) / 3.0f;
	b[0] = (float)b0 / 31.0f;
	b[1] = (float)b1 / 31.0f;
	b[2] = (b[0] * 2.0f + b[1]) / 3.0f;
	b[3] = (b[0] + b[1] * 2.0f) / 3.0f;

	for (sint32 py = 0; py < 4; py++)
	{
		for (sint32 px = 0; px < 4; px++)
		{
			uint8 colorIndex = (colorIndices >> (2 * (px + 4 * py))) & 0x03;
			sint32 pixelOffset = (px + py * 4) * 4;
			imageRGBA[pixelOffset + 0] = r[colorIndex];
			imageRGBA[pixelOffset + 1] = g[colorIndex];
			imageRGBA[pixelOffset + 2] = b[colorIndex];
			//imageRGBA[pixelOffset+3] = 1.0f; // alpha
		}
	}

	// decode alpha
	uint8 alpha0 = *(uint8*)(inputData + 0);
	uint8 alpha1 = *(uint8*)(inputData + 1);
	uint32 alphaCodeRow[2] = { 0 };
	alphaCodeRow[0] |= ((*(uint8*)(inputData + 2)) << 0);
	alphaCodeRow[0] |= ((*(uint8*)(inputData + 3)) << 8);
	alphaCodeRow[0] |= ((*(uint8*)(inputData + 4)) << 16);
	alphaCodeRow[1] |= ((*(uint8*)(inputData + 5)) << 0);
	alphaCodeRow[1] |= ((*(uint8*)(inputData + 6)) << 8);
	alphaCodeRow[1] |= ((*(uint8*)(inputData + 7)) << 16);

	float a[8];
	a[0] = (float)alpha0 / 255.0f;
	a[1] = (float)alpha1 / 255.0f;

	if (alpha0 > alpha1)
	{
		// 6 interpolated alpha values.
		a[2] = (a[0] * 6.0f + a[1] * 1.0f) / 7.0f;
		a[3] = (a[0] * 5.0f + a[1] * 2.0f) / 7.0f;
		a[4] = (a[0] * 4.0f + a[1] * 3.0f) / 7.0f;
		a[5] = (a[0] * 3.0f + a[1] * 4.0f) / 7.0f;
		a[6] = (a[0] * 2.0f + a[1] * 5.0f) / 7.0f;
		a[7] = (a[0] * 1.0f + a[1] * 6.0f) / 7.0f;
	}
	else
	{
		// 4 interpolated alpha values.
		a[2] = (a[0] * 4.0f + a[1] * 1.0f) / 5.0f;
		a[3] = (a[0] * 3.0f + a[1] * 2.0f) / 5.0f;
		a[4] = (a[0] * 2.0f + a[1] * 3.0f) / 5.0f;
		a[5] = (a[0] * 1.0f + a[1] * 4.0f) / 5.0f;
		a[6] = 0.0f;
		a[7] = 1.0f;
	}

	for (sint32 py = 0; py < 4; py++)
	{
		for (sint32 px = 0; px < 4; px++)
		{
			uint8 alphaCode = (alphaCodeRow[py / 2] >> 3 * (px + 4 * (py & 1))) & 0x07;
			sint32 pixelOffset = (px + py * 4) * 4;
			imageRGBA[pixelOffset + 3] = a[alphaCode]; // alpha
		}
	}
}

void decodeBC4Block_UNORM(uint8* blockStorage, float* rOutput)
{
	uint8* blockInput = (uint8*)blockStorage;
	float red[8];

	red[0] = ((float)(*(uint8*)(blockInput + 0))) / 255.0f;
	red[1] = ((float)(*(uint8*)(blockInput + 1))) / 255.0f;

	if (blockInput[0] > blockInput[1])
	{
		// 6 interpolated color values
		red[2] = (6 * red[0] + 1 * red[1]) / 7.0f; // bit code 010
		red[3] = (5 * red[0] + 2 * red[1]) / 7.0f; // bit code 011
		red[4] = (4 * red[0] + 3 * red[1]) / 7.0f; // bit code 100
		red[5] = (3 * red[0] + 4 * red[1]) / 7.0f; // bit code 101
		red[6] = (2 * red[0] + 5 * red[1]) / 7.0f; // bit code 110
		red[7] = (1 * red[0] + 6 * red[1]) / 7.0f; // bit code 111
	}
	else
	{
		// 4 interpolated color values
		red[2] = (4 * red[0] + 1 * red[1]) / 5.0f; // bit code 010
		red[3] = (3 * red[0] + 2 * red[1]) / 5.0f; // bit code 011
		red[4] = (2 * red[0] + 3 * red[1]) / 5.0f; // bit code 100
		red[5] = (1 * red[0] + 4 * red[1]) / 5.0f; // bit code 101
		red[6] = 0.0f;                       // bit code 110
		red[7] = 1.0f;                       // bit code 111
	}

	uint8* bitIndices = blockInput + 2;
	uint32 redRow0 = (((uint32)bitIndices[2]) << 16) | (((uint32)bitIndices[1]) << 8) | (((uint32)bitIndices[0]) << 0);
	uint32 redRow1 = (((uint32)bitIndices[5]) << 16) | (((uint32)bitIndices[4]) << 8) | (((uint32)bitIndices[3]) << 0);

	uint8 pRed[16];
	for (sint32 i = 0; i < 8; i++)
	{
		pRed[i] = (redRow0 >> (i * 3)) & 7;
		pRed[i + 8] = (redRow1 >> (i * 3)) & 7;
	}

	float* pixelOutput = rOutput;
	for (sint32 py = 0; py < 4; py++)
	{
		for (sint32 px = 0; px < 4; px++)
		{
			float c = red[pRed[px + py * 4]];
			*pixelOutput = c;
			pixelOutput++;
		}
	}
}

void decodeBC4Block_SNORM(uint8* blockStorage, float* rOutput)
{
    uint8* blockInput = (uint8*)blockStorage;
    float red[8];

    int8_t r0raw = (int8_t)blockInput[0];
    int8_t r1raw = (int8_t)blockInput[1];
    red[0] = (r0raw == -128) ? -1.0f : (float)r0raw / 127.0f;
    red[1] = (r1raw == -128) ? -1.0f : (float)r1raw / 127.0f;

    if (r0raw > r1raw)
    {
        // 6 interpolated color values
        red[2] = (6 * red[0] + 1 * red[1]) / 7.0f;
        red[3] = (5 * red[0] + 2 * red[1]) / 7.0f;
        red[4] = (4 * red[0] + 3 * red[1]) / 7.0f;
        red[5] = (3 * red[0] + 4 * red[1]) / 7.0f;
        red[6] = (2 * red[0] + 5 * red[1]) / 7.0f;
        red[7] = (1 * red[0] + 6 * red[1]) / 7.0f;
    }
    else
    {
        // 4 interpolated color values
        red[2] = (4 * red[0] + 1 * red[1]) / 5.0f;
        red[3] = (3 * red[0] + 2 * red[1]) / 5.0f;
        red[4] = (2 * red[0] + 3 * red[1]) / 5.0f;
        red[5] = (1 * red[0] + 4 * red[1]) / 5.0f;
        red[6] = -1.0f;   // bit code 110
        red[7] =  1.0f;   // bit code 111
    }

    uint8* bitIndices = blockInput + 2;
    uint32 redRow0 = (((uint32)bitIndices[2]) << 16) | (((uint32)bitIndices[1]) << 8) | (((uint32)bitIndices[0]) << 0);
    uint32 redRow1 = (((uint32)bitIndices[5]) << 16) | (((uint32)bitIndices[4]) << 8) | (((uint32)bitIndices[3]) << 0);

    uint8 pRed[16];
    for (sint32 i = 0; i < 8; i++)
    {
        pRed[i]     = (redRow0 >> (i * 3)) & 7;
        pRed[i + 8] = (redRow1 >> (i * 3)) & 7;
    }

    float* pixelOutput = rOutput;
    for (sint32 py = 0; py < 4; py++)
    {
        for (sint32 px = 0; px < 4; px++)
        {
            *pixelOutput++ = red[pRed[px + py * 4]];
        }
    }
}

void decodeBC5Block_UNORM(uint8* blockStorage, float* rgOutput)
{
	uint8* blockInput = (uint8*)blockStorage;
	float red[8];
	float green[8];

	red[0] = ((float)(*(uint8*)(blockInput + 0))) / 255.0f;
	red[1] = ((float)(*(uint8*)(blockInput + 1))) / 255.0f;

	if (red[0] > red[1])
	{
		// 6 interpolated color values
		red[2] = (6 * red[0] + 1 * red[1]) / 7.0f; // bit code 010
		red[3] = (5 * red[0] + 2 * red[1]) / 7.0f; // bit code 011
		red[4] = (4 * red[0] + 3 * red[1]) / 7.0f; // bit code 100
		red[5] = (3 * red[0] + 4 * red[1]) / 7.0f; // bit code 101
		red[6] = (2 * red[0] + 5 * red[1]) / 7.0f; // bit code 110
		red[7] = (1 * red[0] + 6 * red[1]) / 7.0f; // bit code 111
	}
	else
	{
		// 4 interpolated color values
		red[2] = (4 * red[0] + 1 * red[1]) / 5.0f; // bit code 010
		red[3] = (3 * red[0] + 2 * red[1]) / 5.0f; // bit code 011
		red[4] = (2 * red[0] + 3 * red[1]) / 5.0f; // bit code 100
		red[5] = (1 * red[0] + 4 * red[1]) / 5.0f; // bit code 101
		red[6] = 0.0f;                       // bit code 110
		red[7] = 1.0f;                       // bit code 111
	}

	green[0] = ((float)(*(uint8*)(blockInput + 8))) / 255.0f;
	green[1] = ((float)(*(uint8*)(blockInput + 9))) / 255.0f;

	if (green[0] > green[1])
	{
		// 6 interpolated color values
		green[2] = (6 * green[0] + 1 * green[1]) / 7.0f; // bit code 010
		green[3] = (5 * green[0] + 2 * green[1]) / 7.0f; // bit code 011
		green[4] = (4 * green[0] + 3 * green[1]) / 7.0f; // bit code 100
		green[5] = (3 * green[0] + 4 * green[1]) / 7.0f; // bit code 101
		green[6] = (2 * green[0] + 5 * green[1]) / 7.0f; // bit code 110
		green[7] = (1 * green[0] + 6 * green[1]) / 7.0f; // bit code 111
	}
	else
	{
		// 4 interpolated color values
		green[2] = (4 * green[0] + 1 * green[1]) / 5.0f; // bit code 010
		green[3] = (3 * green[0] + 2 * green[1]) / 5.0f; // bit code 011
		green[4] = (2 * green[0] + 3 * green[1]) / 5.0f; // bit code 100
		green[5] = (1 * green[0] + 4 * green[1]) / 5.0f; // bit code 101
		green[6] = 0.0f;						   // bit code 110
		green[7] = 1.0f;                           // bit code 111
	}


	uint8* bitIndices = blockInput + 2;
	uint32 redRow0 = (((uint32)bitIndices[2]) << 16) | (((uint32)bitIndices[1]) << 8) | (((uint32)bitIndices[0]) << 0);
	uint32 redRow1 = (((uint32)bitIndices[5]) << 16) | (((uint32)bitIndices[4]) << 8) | (((uint32)bitIndices[3]) << 0);
	bitIndices = blockInput + 8 + 2;
	uint32 greenRow0 = (((uint32)bitIndices[2]) << 16) | (((uint32)bitIndices[1]) << 8) | (((uint32)bitIndices[0]) << 0);
	uint32 greenRow1 = (((uint32)bitIndices[5]) << 16) | (((uint32)bitIndices[4]) << 8) | (((uint32)bitIndices[3]) << 0);

	uint8 pRed[16];
	uint8 pGreen[16];
	for (sint32 i = 0; i < 8; i++)
	{
		pRed[i] = (redRow0 >> (i * 3)) & 7;
		pRed[i + 8] = (redRow1 >> (i * 3)) & 7;
		pGreen[i] = (greenRow0 >> (i * 3)) & 7;
		pGreen[i + 8] = (greenRow1 >> (i * 3)) & 7;
	}

	float* pixelOutput = rgOutput;
	for (sint32 py = 0; py < 4; py++)
	{
		for (sint32 px = 0; px < 4; px++)
		{
			float c = red[pRed[px + py * 4]];
			*pixelOutput = c;
			pixelOutput++;
			c = green[pGreen[px + py * 4]];
			*pixelOutput = c;
			pixelOutput++;
		}
	}
}

void decodeBC5Block_SNORM(uint8* blockStorage, float* rgOutput) // todo - can merge this with the UNORM implementation by using a template?
{
	uint8* blockInput = (uint8*)blockStorage;
	float red[8];
	float green[8];

	red[0] = ((float)(*(sint8*)(blockInput + 0)) + 128.0f) / 255.0f;
	red[1] = ((float)(*(sint8*)(blockInput + 1)) + 128.0f) / 255.0f;
	red[0] = (red[0] * 2.0f - 1.0f);
	red[1] = (red[1] * 2.0f - 1.0f);

	if (red[0] > red[1])
	{
		// 6 interpolated color values
		red[2] = (6 * red[0] + 1 * red[1]) / 7.0f; // bit code 010
		red[3] = (5 * red[0] + 2 * red[1]) / 7.0f; // bit code 011
		red[4] = (4 * red[0] + 3 * red[1]) / 7.0f; // bit code 100
		red[5] = (3 * red[0] + 4 * red[1]) / 7.0f; // bit code 101
		red[6] = (2 * red[0] + 5 * red[1]) / 7.0f; // bit code 110
		red[7] = (1 * red[0] + 6 * red[1]) / 7.0f; // bit code 111
	}
	else
	{
		// 4 interpolated color values
		red[2] = (4 * red[0] + 1 * red[1]) / 5.0f; // bit code 010
		red[3] = (3 * red[0] + 2 * red[1]) / 5.0f; // bit code 011
		red[4] = (2 * red[0] + 3 * red[1]) / 5.0f; // bit code 100
		red[5] = (1 * red[0] + 4 * red[1]) / 5.0f; // bit code 101
		red[6] = -1.0f;                       // bit code 110
		red[7] = 1.0f;                       // bit code 111
	}

	green[0] = ((float)(*(sint8*)(blockInput + 8)) + 128.0f) / 255.0f;
	green[1] = ((float)(*(sint8*)(blockInput + 9)) + 128.0f) / 255.0f;
	green[0] = (green[0] * 2.0f - 1.0f);
	green[1] = (green[1] * 2.0f - 1.0f);

	if (green[0] > green[1])
	{
		// 6 interpolated color values
		green[2] = (6 * green[0] + 1 * green[1]) / 7.0f; // bit code 010
		green[3] = (5 * green[0] + 2 * green[1]) / 7.0f; // bit code 011
		green[4] = (4 * green[0] + 3 * green[1]) / 7.0f; // bit code 100
		green[5] = (3 * green[0] + 4 * green[1]) / 7.0f; // bit code 101
		green[6] = (2 * green[0] + 5 * green[1]) / 7.0f; // bit code 110
		green[7] = (1 * green[0] + 6 * green[1]) / 7.0f; // bit code 111
	}
	else
	{
		// 4 interpolated color values
		green[2] = (4 * green[0] + 1 * green[1]) / 5.0f; // bit code 010
		green[3] = (3 * green[0] + 2 * green[1]) / 5.0f; // bit code 011
		green[4] = (2 * green[0] + 3 * green[1]) / 5.0f; // bit code 100
		green[5] = (1 * green[0] + 4 * green[1]) / 5.0f; // bit code 101
		green[6] = -1.0f;                       // bit code 110
		green[7] = 1.0f;                       // bit code 111
	}


	uint8* bitIndices = blockInput + 2;
	uint32 redRow0 = (((uint32)bitIndices[2]) << 16) | (((uint32)bitIndices[1]) << 8) | (((uint32)bitIndices[0]) << 0);
	uint32 redRow1 = (((uint32)bitIndices[5]) << 16) | (((uint32)bitIndices[4]) << 8) | (((uint32)bitIndices[3]) << 0);
	bitIndices = blockInput + 8 + 2;
	uint32 greenRow0 = (((uint32)bitIndices[2]) << 16) | (((uint32)bitIndices[1]) << 8) | (((uint32)bitIndices[0]) << 0);
	uint32 greenRow1 = (((uint32)bitIndices[5]) << 16) | (((uint32)bitIndices[4]) << 8) | (((uint32)bitIndices[3]) << 0);

	uint8 pRed[16];
	uint8 pGreen[16];
	for (sint32 i = 0; i < 8; i++)
	{
		pRed[i] = (redRow0 >> (i * 3)) & 7;
		pRed[i + 8] = (redRow1 >> (i * 3)) & 7;
		pGreen[i] = (greenRow0 >> (i * 3)) & 7;
		pGreen[i + 8] = (greenRow1 >> (i * 3)) & 7;
	}

	for (sint32 py = 0; py < 4; py++)
	{
		float* pixelOutput = rgOutput + (py * 4) * 2;
		for (sint32 px = 0; px < 4; px++)
		{
			float c = red[pRed[px + py * 4]];
			pixelOutput[0] = c;
			c = green[pGreen[px + py * 4]];
			pixelOutput[1] = c;
			pixelOutput += 2;
		}
	}
}

void LatteTextureLoader_loadTextureDataIntoSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, sint32 mipLevels, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	if (mipIndex == 0)
	{
		cemu_assert_debug(width == hostTexture->width);
		cemu_assert_debug(height == hostTexture->height);
		cemu_assert_debug(depth == hostTexture->depth);
	}
	cemu_assert_debug(mipLevels == hostTexture->mipLevels);
	if (hostTexture->overwriteInfo.hasResolutionOverwrite || hostTexture->overwriteInfo.hasFormatOverwrite)
	{
		// todo - ideally, we should scale/convert the data to the new format and resolution
		g_renderer->texture_clearSlice(hostTexture, sliceIndex, mipIndex);
	}
	else
	{
		g_renderer->texture_loadSlice(hostTexture, width, height, depth, pixelData, sliceIndex, mipIndex, compressedImageSize);
	}
}

// Experimental Worker-Thread Texture Decode (default OFF). Returns true when it fully (re)loaded the
// texture through the parallel fork-join path; false tells the caller to run the original serial loop.
//
// Concurrency model / ownership boundaries:
//   Phase 1 (Latte thread only): enumerate every (slice,mip) unit in the SAME order as the serial loop
//     and do all thread-affine setup — LatteTextureLoader_begin, decoder texel counts, image size, the
//     one-time host allocation, each unit's own output buffer, and the single-threaded M1 decode-cache
//     probe. Nothing here is touched by workers.
//   Phase 2 (fork-join pool; the Latte thread participates): run ONLY the pure decoders. Each worker
//     writes to its own job's buffer, reading read-only guest memory through its own ctx copy. No shared
//     mutable state (no cache, no upload buffer, no renderer). Full join barrier before Phase 3.
//   Phase 3 (Latte thread only, original order): store M1 cache entries, replay the serial change-tracker
//     update, and upload each slice via the renderer.
// No slice is uploaded before the join, so no decoded texture is visible to the renderer before its
// decode completed (explicit ownership + join, not timing).
bool LatteTextureLoader_ReloadDataParallel(LatteTexture* tex)
{
	// Overwrite/dump variants take separate paths in the serial loader; keep them serial.
	if (ActiveSettings::DumpTexturesEnabled())
		return false;
	if (tex->overwriteInfo.hasFormatOverwrite || tex->overwriteInfo.hasResolutionOverwrite)
		return false;

	const Latte::E_GX2SURFFMT format = tex->format;
	const Latte::E_DIM dim = tex->dim;
	// One decoder per texture (format-driven). If none, the serial path performs the no-decoder clear.
	TextureDecoder* decoder = g_renderer->texture_chooseDecodedFormat(format, tex->isDepth, dim, tex->width, tex->height);
	if (!decoder)
		return false;

	// Enumerate units in the exact serial order (mip-major; slice order per dim).
	std::vector<SliceDecodeJob> jobs;
	for (sint32 mip = 0; mip < tex->mipLevels; mip++)
	{
		sint32 numSlices;
		if (dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA)
			numSlices = std::max(tex->depth, 1);
		else if (dim == Latte::E_DIM::DIM_CUBEMAP)
			numSlices = (tex->depth / 6) * 6; // matches serial numFullCubeMaps*6
		else if (dim == Latte::E_DIM::DIM_3D)
			numSlices = std::max(tex->depth >> mip, 1);
		else
			numSlices = 1;
		for (sint32 s = 0; s < numSlices; s++)
		{
			jobs.emplace_back();
			SliceDecodeJob& j = jobs.back();
			j.sliceIndex = (uint32)s;
			j.mipIndex = (uint32)mip;
			j.decoder = decoder;
		}
	}
	// Nothing to gain without at least two independent units; let the serial path handle it.
	if (jobs.size() < 2)
		return false;

	// Ensure the host texture exists before any upload (mirrors the serial lazy-allocate). Safe even if
	// we fall back below: the serial loop then sees isDataDefined already true and skips its own alloc.
	if (tex->isDataDefined == false)
	{
		tex->AllocateOnHost();
		tex->isDataDefined = true;
	}

	// Phase 1 (Latte thread): per-unit setup + M1 decode-cache probe. Bounded by a transient-byte budget
	// so a huge mip chain can't balloon memory; on overflow fall back to serial (correct, and frees what
	// we allocated). Cache probe/store stay single-threaded here — workers never touch M1.
	const size_t kMaxTransientBytes = 96ull * 1024 * 1024;
	size_t totalBytes = 0;
	const bool useDecodeCache = ActiveSettings::ExperimentalDecodeCache();
	for (SliceDecodeJob& j : jobs)
	{
		LatteTextureLoader_begin(&j.ctx, j.sliceIndex, j.mipIndex, tex->physAddress, tex->physMipAddress, format, dim, tex->width, tex->height, tex->depth, tex->mipLevels, tex->pitch, tex->tileMode, tex->swizzle);
		j.ctx.dump = false;
		j.ctx.decodedTexelCountX = decoder->getTexelCountX(&j.ctx);
		j.ctx.decodedTexelCountY = decoder->getTexelCountY(&j.ctx);
		j.imageSize = (sint32)decoder->calculateImageSize(&j.ctx);
		if (j.imageSize <= 0)
			return false; // unexpected sizing; let the serial path handle it
		totalBytes += (size_t)j.imageSize;
		if (totalBytes > kMaxTransientBytes)
			return false;
		j.buffer.resize((size_t)j.imageSize);

		if (useDecodeCache && j.ctx.inputData && j.ctx.maxOffsetOutdated > 0)
		{
			DecodeCacheParams dcp{};
			dcp.format = (uint32)format;
			dcp.dim = (uint32)dim;
			dcp.width = (uint32)j.ctx.width;
			dcp.height = (uint32)j.ctx.height;
			dcp.depth = (uint32)tex->depth;
			dcp.pitch = (uint32)j.ctx.pitch;
			dcp.tileMode = (uint32)j.ctx.tileMode;
			dcp.pipeSwizzle = j.ctx.pipeSwizzle;
			dcp.bankSwizzle = j.ctx.bankSwizzle;
			dcp.bpp = j.ctx.bpp;
			dcp.sliceIndex = j.sliceIndex;
			dcp.mipIndex = j.mipIndex;
			dcp.surfaceInfoHeight = j.ctx.surfaceInfoHeight;
			dcp.surfaceInfoDepth = j.ctx.surfaceInfoDepth;
			dcp.imageSize = (uint32)j.imageSize;
			dcp.decodedTexelCountX = (uint32)j.ctx.decodedTexelCountX;
			dcp.decodedTexelCountY = (uint32)j.ctx.decodedTexelCountY;
			dcp.isDepth = tex->isDepth ? 1u : 0u;
			const uint32 srcLen = (uint32)j.ctx.maxOffsetOutdated;
			DecodeCache_HashSource(j.ctx.inputData, srcLen, j.cacheH0, j.cacheH1);
			DecodeCache_FoldParams(dcp, j.cacheH0, j.cacheH1);
			if (g_textureDecodeCache.tryGet(j.cacheH0, j.cacheH1, dcp, j.buffer.data(), (uint32)j.imageSize))
				j.skipDecode = true;
			else
			{
				j.needCachePut = true;
				j.cacheParams = dcp;
			}
		}
	}

	// Phase 2: pure parallel decode into per-job buffers (Latte thread participates; full join inside).
	GetTexDecodeForkJoin().run(jobs.size(), &TexDecodeJobFn, &jobs);

	// Phase 3 (Latte thread, original order): store new cache entries, replay the serial change-tracker
	// update, then upload each slice.
	for (SliceDecodeJob& j : jobs)
	{
		if (j.needCachePut)
			g_textureDecodeCache.put(j.cacheH0, j.cacheH1, j.cacheParams, j.buffer.data(), (uint32)j.imageSize);
		if (j.mipIndex == 0 || (tex->texDataPtrLow == 0 && tex->texDataPtrHigh == 0))
		{
			tex->texDataPtrLow = tex->physAddress + j.ctx.minOffsetOutdated;
			tex->texDataPtrHigh = tex->physAddress + j.ctx.maxOffsetOutdated;
			LatteTC_ResetTextureChangeTracker(tex, true);
		}
		LatteTextureLoader_loadTextureDataIntoSlice(tex, j.ctx.width, j.ctx.height, (sint32)tex->depth, (sint32)tex->mipLevels, j.buffer.data(), (sint32)j.sliceIndex, (sint32)j.mipIndex, (uint32)j.imageSize);
	}
	return true;
}

void LatteTextureLoader_UpdateTextureSliceData(LatteTexture* tex, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle, bool dumpTex)
{
	LatteTextureLoaderCtx textureLoader = { 0 };

	Latte::E_GX2SURFFMT format = tex->format;
	LatteTextureLoader_begin(&textureLoader, sliceIndex, mipIndex, physImagePtr, physMipPtr, format, dim, width, height, depth, mipLevels, pitch, tileMode, swizzle);

	// enable texture dumping
	textureLoader.dump = ActiveSettings::DumpTexturesEnabled();
	if (textureLoader.dump)
	{
		uint32 dumpSize = (((textureLoader.width + 4)&~4) * ((textureLoader.height + 4)&~4)) * 4;
		textureLoader.dumpRGBA = (uint8*)malloc(dumpSize);
		memset(textureLoader.dumpRGBA, 0x00, dumpSize);
	}

	// query texture decoder from renderer
	TextureDecoder* texDecoder = nullptr;
	texDecoder = g_renderer->texture_chooseDecodedFormat(format, tex->isDepth, dim, width, height);

	if (tex->isDataDefined == false)
	{
		tex->AllocateOnHost();
		tex->isDataDefined = true;
		// if decoder is not set then clear texture
		// on Vulkan this is used to make sure the texture is no longer in UNDEFINED layout
		if (!texDecoder)
		{
			if(tex->isDepth)
				g_renderer->texture_clearDepthSlice(tex, 0, 0, true, tex->hasStencil, 0.0f, 0);
			else
				g_renderer->texture_clearColorSlice(tex, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
		}
	}

	if (texDecoder == nullptr)
		return;

	textureLoader.decodedTexelCountX = texDecoder->getTexelCountX(&textureLoader);
	textureLoader.decodedTexelCountY = texDecoder->getTexelCountY(&textureLoader);

	// allocate memory for decoded texture
	uint32 imageSize = texDecoder->calculateImageSize(&textureLoader);

	uint8* pixelData = (uint8*)g_renderer->texture_acquireTextureUploadBuffer(imageSize);
	// decode texture (if data is required)
#ifdef BENCHMARK_TEXTURE_DECODING
	LARGE_INTEGER benchmark_begin;
	LARGE_INTEGER benchmark_end;
	LARGE_INTEGER benchmark_freq;
	QueryPerformanceCounter(&benchmark_begin);
#endif

	if (tex->overwriteInfo.hasFormatOverwrite == false && tex->overwriteInfo.hasResolutionOverwrite == false)
	{
		bool decoded = false;
		// Experimental Decode Cache: reuse a prior decode of identical source bytes + params (default OFF).
		// Not used while dumping (the dump path re-reads the source separately). Purely additive: on any
		// miss (or when disabled) the original decode below runs unchanged.
		if (ActiveSettings::ExperimentalDecodeCache() && !textureLoader.dump && textureLoader.inputData && textureLoader.maxOffsetOutdated > 0)
		{
			DecodeCacheParams dcp{};
			dcp.format = (uint32)format;
			dcp.dim = (uint32)dim;
			dcp.width = (uint32)textureLoader.width;
			dcp.height = (uint32)textureLoader.height;
			dcp.depth = depth;
			dcp.pitch = (uint32)textureLoader.pitch;
			dcp.tileMode = (uint32)textureLoader.tileMode;
			dcp.pipeSwizzle = textureLoader.pipeSwizzle;
			dcp.bankSwizzle = textureLoader.bankSwizzle;
			dcp.bpp = textureLoader.bpp;
			dcp.sliceIndex = sliceIndex;
			dcp.mipIndex = mipIndex;
			dcp.surfaceInfoHeight = textureLoader.surfaceInfoHeight;
			dcp.surfaceInfoDepth = textureLoader.surfaceInfoDepth;
			dcp.imageSize = imageSize;
			dcp.decodedTexelCountX = (uint32)textureLoader.decodedTexelCountX;
			dcp.decodedTexelCountY = (uint32)textureLoader.decodedTexelCountY;
			dcp.isDepth = tex->isDepth ? 1u : 0u;
			const uint32 srcLen = (uint32)textureLoader.maxOffsetOutdated;
			uint64 h0, h1;
			DecodeCache_HashSource(textureLoader.inputData, srcLen, h0, h1);
			DecodeCache_FoldParams(dcp, h0, h1);
			if (g_textureDecodeCache.tryGet(h0, h1, dcp, pixelData, imageSize))
			{
				decoded = true;
			}
			else
			{
				texDecoder->decode(&textureLoader, pixelData);
				g_textureDecodeCache.put(h0, h1, dcp, pixelData, imageSize);
				decoded = true;
			}
		}
		if (!decoded)
			texDecoder->decode(&textureLoader, pixelData);
	}

#ifdef BENCHMARK_TEXTURE_DECODING
	QueryPerformanceCounter(&benchmark_end);
	QueryPerformanceFrequency(&benchmark_freq);
	uint64 benchmarkResultMicroSeconds = (benchmark_end.QuadPart - benchmark_begin.QuadPart) * 1000000ULL / benchmark_freq.QuadPart;
	textureDecodeBenchmark_perFormatSum[(int)tex->format & 0x3F] += benchmarkResultMicroSeconds;
	textureDecodeBenchmark_totalSum += benchmarkResultMicroSeconds;
	cemuLog_log(LogType::Force, "TexDecode {:04}x{:04}x{:04} Fmt {:04x} Dim {} TileMode {:02x} Took {:03}.{:03}ms Sum(format) {:06}ms Sum(total) {:06}ms", textureLoader.width, textureLoader.height, textureLoader.surfaceInfoDepth, (int)tex->format, (int)tex->dim, textureLoader.tileMode, (uint32)(benchmarkResultMicroSeconds / 1000ULL), (uint32)(benchmarkResultMicroSeconds % 1000ULL), (uint32)(textureDecodeBenchmark_perFormatSum[tex->gx2Format & 0x3F] / 1000ULL), (uint32)(textureDecodeBenchmark_totalSum / 1000ULL));
#endif

	// convert texture to RGBA when dumping is enabled
	if (textureLoader.dump)
	{
		for (sint32 y = 0; y < textureLoader.height; y++)
		{
			sint32 pixelOffset = (y * textureLoader.width) * 4;
			uint8* pixelOutput = textureLoader.dumpRGBA + pixelOffset;
			for (sint32 x = 0; x < textureLoader.width; x++)
			{
				uint8* blockData = LatteTextureLoader_GetInput(&textureLoader, x, y);
				texDecoder->decodePixelToRGBA(blockData, pixelOutput, x % textureLoader.stepX, y % textureLoader.stepY);
				pixelOutput += 4;
			}
		}
	}

	// update texture data offsets and hashes
	// this has to be done before the texture data is decoded & uploaded to prevent a race condition where updates during upload are missed
	if (mipIndex == 0 || (tex->texDataPtrLow == 0 && tex->texDataPtrHigh == 0))
	{
		tex->texDataPtrLow = physImagePtr + textureLoader.minOffsetOutdated; // always zero
		tex->texDataPtrHigh = physImagePtr + textureLoader.maxOffsetOutdated; // currently set to surface size
		LatteTC_ResetTextureChangeTracker(tex, true);
	}
	// load slice
	//debug_printf("[Load Slice] Addr: %08x MIP: %02d Slice: %02d Res %04x/%04x Texel Res %04x/%04x Fmt %04x Tm %d\n", textureLoader.physAddress, mipIndex, sliceIndex, textureLoader.width, textureLoader.height, textureLoader.texelCountX, textureLoader.texelCountY, (int)format, tileMode);

	LatteTextureLoader_loadTextureDataIntoSlice(tex, textureLoader.width, textureLoader.height, depth, mipLevels, pixelData, sliceIndex, mipIndex, imageSize);
	// write texture dump
	if (textureLoader.dump)
	{
		fs::path path = ActiveSettings::GetUserDataPath("dump/textures");
		path /= fmt::format("{:08x}_fmt{:04x}_slice{:d}_mip{:02d}_{:d}x{:d}_tm{:02d}.tga", physImagePtr, (uint32)tex->format, sliceIndex, mipIndex, tex->width, tex->height, tileMode);
		tga_write_rgba(path, textureLoader.width, textureLoader.height, textureLoader.dumpRGBA);
		free(textureLoader.dumpRGBA);
	}
	// clean up
	g_renderer->texture_releaseTextureUploadBuffer(pixelData);
	catchOpenGLError();
}

template<typename copyType>
void optimizedLinearReadbackWriteLoop(LatteTextureLoaderCtx* textureLoader, uint8* linearPixelData)
{
	uint32 pitch = textureLoader->width;
	// optimized for linear
	for (sint32 y = 0; y < textureLoader->height; y++)
	{
		sint32 yc = y;
		sint32 pixelOffset = yc * pitch;
		copyType* rowPixelData = (copyType*)(linearPixelData + pixelOffset * sizeof(copyType));
		copyType* blockData = (copyType*)LatteTextureLoader_getInputLinearOptimized_(textureLoader, 0, y, 1, 1, sizeof(copyType) * 8, 0, 1, 0, textureLoader->pitch, textureLoader->height);
		if constexpr (sizeof(copyType) == 4)
		{
			memcpy_dwords(blockData, rowPixelData, textureLoader->width);
		}
		else
		{
			for (sint32 x = 0; x < textureLoader->width; x++)
			{
				*blockData = *rowPixelData;
				rowPixelData++;
				blockData++;
			}
		}
	}
}

void LatteTextureLoader_writeReadbackTextureToMemory(LatteTextureDefinition* textureData, uint32 sliceIndex, uint32 mipIndex, uint8* linearPixelData)
{
	LatteTextureLoaderCtx textureLoader = { 0 };
	LatteTextureLoader_begin(&textureLoader, sliceIndex, mipIndex, textureData->physAddress, textureData->physMipAddress, textureData->format, textureData->dim, textureData->width, textureData->height, textureData->depth, textureData->mipLevels, textureData->pitch, textureData->tileMode, textureData->swizzle);

#ifdef CEMU_DEBUG_ASSERT
	if (textureData->depth != 1)
		cemuLog_log(LogType::Force, "_writeReadbackTextureToMemory(): Texture has multiple slices (not supported)");
#endif
	if (textureLoader.physAddress == MPTR_NULL)
	{
		cemuLog_log(LogType::Force, "_writeReadbackTextureToMemory(): Texture has invalid address");
		return;
	}

	cemuLog_log(LogType::TextureReadback, "[TextureReadback-Write] PhysAddr {:08x} Res {}x{} Fmt {} Slice {} Mip {}", textureData->physAddress, textureData->width, textureData->height, textureData->format, sliceIndex, mipIndex);

	if (textureData->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
	{
		uint32 pitch = textureLoader.width;
		if (textureData->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM ||
			textureData->format == Latte::E_GX2SURFFMT::R8_G8_B8_A8_SRGB)
		{
			optimizedLinearReadbackWriteLoop<uint32>(&textureLoader, linearPixelData);
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM)
		{
			optimizedLinearReadbackWriteLoop<uint64>(&textureLoader, linearPixelData);
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R32_G32_B32_A32_FLOAT)
		{
			for (sint32 y = 0; y < textureLoader.height; y += textureLoader.stepY)
			{
				sint32 yc = y;
				sint32 pixelOffset = (0 + yc * pitch) * 16;
				for (sint32 x = 0; x < textureLoader.width; x += textureLoader.stepX)
				{
					uint8* blockData = LatteTextureLoader_getInputLinearOptimized(&textureLoader, x, y);
					(*(uint32*)(blockData + 0)) = *(uint32*)(linearPixelData + pixelOffset + 0);
					(*(uint32*)(blockData + 4)) = *(uint32*)(linearPixelData + pixelOffset + 4);
					(*(uint32*)(blockData + 8)) = *(uint32*)(linearPixelData + pixelOffset + 8);
					(*(uint32*)(blockData + 12)) = *(uint32*)(linearPixelData + pixelOffset + 12);
					pixelOffset += 16;
				}
			}
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R32_FLOAT)
		{
			for (sint32 y = 0; y < textureLoader.height; y += textureLoader.stepY)
			{
				sint32 yc = y;
				for (sint32 x = 0; x < textureLoader.width; x += textureLoader.stepX)
				{
					uint8* blockData = LatteTextureLoader_getInputLinearOptimized(&textureLoader, x, y);
					sint32 pixelOffset = (x + yc * pitch) * 4;
					(*(uint32*)(blockData + 0)) = *(uint32*)(linearPixelData + pixelOffset + 0);
				}
			}
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_FLOAT)
		{
			for (sint32 y = 0; y < textureLoader.height; y += textureLoader.stepY)
			{
				sint32 yc = y;
				for (sint32 x = 0; x < textureLoader.width; x += textureLoader.stepX)
				{
					uint8* blockData = LatteTextureLoader_getInputLinearOptimized(&textureLoader, x, y);
					sint32 pixelOffset = (x + yc * pitch) * 8;
					(*(uint32*)(blockData + 0)) = *(uint32*)(linearPixelData + pixelOffset + 0);
					(*(uint32*)(blockData + 4)) = *(uint32*)(linearPixelData + pixelOffset + 4);
				}
			}
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R8_G8_UNORM)
		{
			optimizedLinearReadbackWriteLoop<uint16>(&textureLoader, linearPixelData);
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R16_G16_B16_A16_UNORM)
		{
			cemu_assert_unimplemented();
		}
		else if (textureData->format == Latte::E_GX2SURFFMT::R16_UNORM)
		{
			optimizedLinearReadbackWriteLoop<uint16>(&textureLoader, linearPixelData);
		}
		else
		{
			cemuLog_logDebug(LogType::Force, "Linear texture readback unsupported for format 0x{:04x}", (uint32)textureData->format);
			debugBreakpoint();
		}
		return;
	}
	// generic and slow decode loops
	Latte::E_HWSURFFMT hwFormat = Latte::GetHWFormat(textureData->format);
	if (hwFormat == Latte::E_HWSURFFMT::HWFMT_8_8_8_8)
	{
		// used in Bayonetta 2
		for (sint32 y = 0; y < textureLoader.height; y++)
		{
			uint8* pixelInput = linearPixelData + (y * textureLoader.width) * 4;
			for (sint32 x = 0; x < textureLoader.width; x++)
			{
				uint8* outputData = LatteTextureLoader_GetInput(&textureLoader, x, y);
				*(uint32*)(outputData + 0) = *(uint32*)pixelInput;
				pixelInput += 4;
			}
		}
	}
	else if (hwFormat == Latte::E_HWSURFFMT::HWFMT_32_FLOAT)
	{
		// required by Wind Waker for direct access to depth buffer
		// Bayonetta 2 also uses this but it converts the depth buffer to a color texture first
		for (sint32 y = 0; y < textureLoader.height; y++)
		{
			uint8* pixelInput = linearPixelData + (y * textureLoader.width) * 4;
			for (sint32 x = 0; x < textureLoader.width; x++)
			{
				uint8* outputData = LatteTextureLoader_GetInput(&textureLoader, x, y);
				*(uint32*)(outputData + 0) = *(uint32*)pixelInput;
				pixelInput += 4;
			}
		}
	}
	else
	{
		cemuLog_logDebug(LogType::Force, "Texture readback unsupported format {:04x} for tileMode 0x{:02x}", (uint32)textureData->format, textureData->tileMode);
	}

}

void LatteTextureLoader_estimateAccessedDataRange(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex, uint32& addrStart, uint32& addrEnd)
{
	LatteTextureLoaderCtx textureLoader = { 0 };
	LatteTextureLoader_begin(&textureLoader, sliceIndex, mipIndex, texture->physAddress, texture->physMipAddress, texture->format, texture->dim, texture->width, texture->height, texture->depth, texture->mipLevels, texture->pitch, texture->tileMode, texture->swizzle);

	cemu_assert_debug(textureLoader.width > 0);
	cemu_assert_debug(textureLoader.height > 0);

	// estimate data range by checking addresses of corner pixels
	// this isn't very reliable, find a better solution
	uint32 estimatedMinAddr = 0xFFFFFFFF;
	uint32 estimatedMaxAddr = 0x00000000;
	uint32 tempAddr;
	tempAddr = memory_getVirtualOffsetFromPointer(LatteTextureLoader_GetInput(&textureLoader, 0, 0));
	estimatedMinAddr = std::min(estimatedMinAddr, tempAddr);
	estimatedMaxAddr = std::max(estimatedMaxAddr, tempAddr);
	tempAddr = memory_getVirtualOffsetFromPointer(LatteTextureLoader_GetInput(&textureLoader, textureLoader.width - 1, 0));
	estimatedMinAddr = std::min(estimatedMinAddr, tempAddr);
	estimatedMaxAddr = std::max(estimatedMaxAddr, tempAddr);
	tempAddr = memory_getVirtualOffsetFromPointer(LatteTextureLoader_GetInput(&textureLoader, 0, textureLoader.height - 1));
	estimatedMinAddr = std::min(estimatedMinAddr, tempAddr);
	estimatedMaxAddr = std::max(estimatedMaxAddr, tempAddr);
	tempAddr = memory_getVirtualOffsetFromPointer(LatteTextureLoader_GetInput(&textureLoader, textureLoader.width - 1, textureLoader.height - 1));
	estimatedMinAddr = std::min(estimatedMinAddr, tempAddr);
	estimatedMaxAddr = std::max(estimatedMaxAddr, tempAddr);

	addrStart = estimatedMinAddr;
	addrEnd = estimatedMaxAddr;
}
