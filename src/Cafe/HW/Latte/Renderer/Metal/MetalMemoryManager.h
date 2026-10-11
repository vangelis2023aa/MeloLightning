#pragma once

#include "Cafe/HW/Latte/Renderer/Metal/MetalBufferAllocator.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalSharedBufferTracker.h"

#include "GameProfile/GameProfile.h"

#include <array>

// MetalArgumentBinding / MetalArgumentBindings are defined in MetalCommon.h (pulled in transitively here) so
// that both this manager and MetalRenderer can store a MetalArgumentBindings by value without a header cycle.

class MetalMemoryManager
{
public:
    MetalMemoryManager(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer}, m_stagingAllocator(m_mtlr, m_mtlr->GetOptimalBufferStorageMode(), 32u * 1024 * 1024), m_indexAllocator(m_mtlr, m_mtlr->GetOptimalBufferStorageMode(), 4u * 1024 * 1024), m_snapshotAllocator(m_mtlr, m_mtlr->GetOptimalBufferStorageMode(), 4u * 1024 * 1024) {}
    ~MetalMemoryManager();

    static constexpr uint32 VertexSnapshotBase = 0;
    static constexpr uint32 UniformSnapshotBase = VertexSnapshotBase + MAX_MTL_VERTEX_BUFFERS;
    static constexpr uint32 SupportSnapshotBase = UniformSnapshotBase + METAL_GENERAL_SHADER_TYPE_TOTAL * MAX_MTL_BUFFERS;
    static constexpr uint32 SnapshotCount = SupportSnapshotBase + METAL_SHADER_TYPE_TOTAL;
    
    
    MetalSynchronizedHeapAllocator::AllocatorReservation* GetCachedSnapshot(uint32 slot, const void* data, uint32 size, uint32 firstByte = 0);
    MetalSynchronizedHeapAllocator::AllocatorReservation* GetCachedArgumentBuffer(uint32 stage, MTL::ArgumentEncoder* encoder, const MetalArgumentBindings& bindings);
    void GetSnapshotStats(uint32& numBuffers, size_t& totalSize, size_t& freeSize) const
    {
        m_snapshotAllocator.GetStats(numBuffers, totalSize, freeSize);
    }

    MetalSynchronizedRingAllocator& GetStagingAllocator()
    {
        return m_stagingAllocator;
    }

    MetalSynchronizedHeapAllocator& GetIndexAllocator()
    {
        return m_indexAllocator;
    }

    MTL::Buffer* GetBufferCache()
    {
        return m_bufferCache;
    }

    MTL::Buffer* GetImportedMemoryBuffer()
    {
        return m_importedMemoryBuffer;
    }

    void CleanupBuffers(MTL::CommandBuffer* latestFinishedCommandBuffer)
    {
        m_stagingAllocator.CleanupBuffer(latestFinishedCommandBuffer);
        m_indexAllocator.CleanupBuffer(latestFinishedCommandBuffer);
        m_snapshotAllocator.CleanupBuffer(latestFinishedCommandBuffer);
        m_sharedTracker.Complete(latestFinishedCommandBuffer);
    }

    // Texture upload buffer
    void* AcquireTextureUploadBuffer(size_t size);
    void ReleaseTextureUploadBuffer(uint8* mem);

    // Buffer cache
    void InitBufferCache(size_t size);
    void UploadToBufferCache(const void* data, size_t offset, size_t size);
    void CopyBufferCache(size_t srcOffset, size_t dstOffset, size_t size);
    void TrackSharedCache(MTL::Buffer* buffer, size_t offset, size_t size, bool write = false);
    bool SharedCacheBusy(size_t offset, size_t size, bool writesOnly = false) const;
    void NotifyBufferCacheRangeModified(size_t offset, size_t size);
    void NotifyImportedMemoryRangeModified(size_t offset, size_t size);

    // Getters
    bool UseHostMemoryForCache() const
    {
        return (m_metalBufferCacheMode == MetalBufferCacheMode::Host);
    }

    bool NeedsReducedLatency() const
    {
        return (m_metalBufferCacheMode == MetalBufferCacheMode::DeviceShared || m_metalBufferCacheMode == MetalBufferCacheMode::Host);
    }

    MPTR GetImportedMemBaseAddress() const
    {
        return m_importedMemBaseAddress;
    }

    size_t GetHostAllocationSize() const
    {
        return m_hostAllocationSize;
    }

    bool IsRangeImported(MPTR address, size_t size) const
    {
        if (!UseHostMemoryForCache() || !m_importedMemoryBuffer)
            return false;
        if (address < m_importedMemBaseAddress)
            return false;

        uint64 offset = (uint64)address - m_importedMemBaseAddress;
        return offset <= m_hostAllocationSize && size <= (m_hostAllocationSize - offset);
    }

    size_t GetImportedMemoryOffset(MPTR address) const
    {
        cemu_assert_debug(address >= m_importedMemBaseAddress);
        return (size_t)((uint64)address - m_importedMemBaseAddress);
    }

private:
    void NotifyBufferRangeModified(MTL::Buffer* buffer, size_t offset, size_t size);

    class MetalRenderer* m_mtlr;

    std::vector<uint8> m_textureUploadBuffer;

    MetalSynchronizedRingAllocator m_stagingAllocator;
    MetalSynchronizedHeapAllocator m_indexAllocator;
    MetalSynchronizedHeapAllocator m_snapshotAllocator;

    struct BufferSnapshot
    {
        MetalSynchronizedHeapAllocator::AllocatorReservation* allocation = nullptr;
        uint32 firstByte = 0;
        uint32 endByte = 0;
    } m_snapshots[SnapshotCount]{};

    struct ArgumentSnapshot
    {
        MTL::ArgumentEncoder* encoder = nullptr; // retained to keep layout identity stable
        MetalArgumentBindings bindings{};
        MetalSynchronizedHeapAllocator::AllocatorReservation* allocation = nullptr;
    } m_argumentSnapshots[METAL_SHADER_TYPE_TOTAL]{};

    MTL::Buffer* m_bufferCache = nullptr;
    MTL::Buffer* m_importedMemoryBuffer = nullptr;
    MetalBufferCacheMode m_metalBufferCacheMode;
    MPTR m_importedMemBaseAddress;
    size_t m_hostAllocationSize = 0;
    MetalSharedBufferTracker m_sharedTracker;
};
