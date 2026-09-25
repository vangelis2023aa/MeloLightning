#pragma once

#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCompiler.h"
#include "util/helpers/ConcurrentQueue.h"
#include "util/helpers/fspinlock.h"
#include "util/math/vector2.h"

class MetalPipelineCache
{
public:
	static MetalPipelineCache& GetInstance();

    MetalPipelineCache(class MetalRenderer* metalRenderer);
    ~MetalPipelineCache();

    PipelineObject* GetRenderPipelineState(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const class MetalAttachmentsInfo& lastUsedAttachmentsInfo, const class MetalAttachmentsInfo& activeAttachmentsInfo, Vector2i extend, uint32 indexCount, const LatteContextRegister& lcr);

    // Cache loading
	uint32 BeginLoading(uint64 cacheTitleId); // returns count of pipelines stored in cache
	bool UpdateLoading(uint32& pipelinesLoadedTotal, uint32& pipelinesMissingShaders);
	void EndLoading();
	void LoadPipelineFromCache(std::span<uint8> fileData);
       void Close(); // called on title exit

    // Debug
    size_t GetPipelineCacheSize() const { return m_pipelineCache.size(); }

private:
    class MetalRenderer* m_mtlr;

    std::unordered_map<uint64, PipelineObject*> m_pipelineCache;
    FSpinlock m_pipelineCacheLock;

	// Experimental per-draw-pass fast path (experimental_pipeline_cache_fast_path). Remembers the
	// pipeline resolved for the last draw so a following draw in the SAME draw pass can be served
	// without recomputing CalculatePipelineHash or probing m_pipelineCache. Reused only when the
	// draw-pass generation still matches and every hash input that can vary within a pass (the four
	// shader pointers, a vertex-stride signature and the primitive type) is unchanged; any mismatch
	// falls through to the normal hash+probe path, so a stale entry can never resolve a wrong
	// pipeline. GPU-thread only (same thread as GetRenderPipelineState); untouched when the toggle
	// is OFF (m_fpPipelineObj stays null → the fast path is never entered).
	uint32 m_fpGeneration = 0;
	const LatteFetchShader* m_fpFetchShader = nullptr;
	const LatteDecompilerShader* m_fpVertexShader = nullptr;
	const LatteDecompilerShader* m_fpGeometryShader = nullptr;
	const LatteDecompilerShader* m_fpPixelShader = nullptr;
	uint64 m_fpStrideSig = 0;
	uint32 m_fpPrimitiveType = 0;
	PipelineObject* m_fpPipelineObj = nullptr;

	std::thread* m_pipelineCacheStoreThread;

	class FileCache* s_cache;

	std::atomic_uint32_t m_numCompilationThreads{ 0 };
	ConcurrentQueue<std::vector<uint8>> m_compilationQueue;
	std::atomic_uint32_t m_compilationCount;

    static uint64 CalculatePipelineHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const class MetalAttachmentsInfo& lastUsedAttachmentsInfo, const class MetalAttachmentsInfo& activeAttachmentsInfo, const LatteContextRegister& lcr);

    void AddCurrentStateToCache(uint64 pipelineStateHash, const class MetalAttachmentsInfo& lastUsedAttachmentsInfo);

	// pipeline serialization for file
	bool SerializePipeline(class MemStreamWriter& memWriter, struct CachedPipeline& cachedPipeline);
	bool DeserializePipeline(class MemStreamReader& memReader, struct CachedPipeline& cachedPipeline);

    int CompilerThread();
	void WorkerThread();
};
