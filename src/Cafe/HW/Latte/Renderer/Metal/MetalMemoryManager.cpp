#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h"

#include "CafeSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Common/precompiled.h"
#include "HW/MMU/MMU.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"

#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

#include <cstring>

MetalMemoryManager::~MetalMemoryManager()
{
    for (auto& snapshot : m_argumentSnapshots)
    {
        for (const auto& binding : snapshot.bindings)
            if (binding.resource)
                static_cast<NS::Object*>(binding.resource)->release();
        if (snapshot.encoder)
            snapshot.encoder->release();
    }
    if (m_bufferCache)
    {
        m_bufferCache->release();
    }
    if (m_importedMemoryBuffer)
    {
        m_importedMemoryBuffer->release();
    }
}

MetalSynchronizedHeapAllocator::AllocatorReservation* MetalMemoryManager::GetCachedSnapshot(uint32 slot, const void* data, uint32 size, uint32 firstByte)
{
    cemu_assert_debug(slot < SnapshotCount && firstByte <= size);
    m_mtlr->GetCommandBuffer();
    auto& snapshot = m_snapshots[slot];
    const auto* source = static_cast<const uint8*>(data);
    const uint32 copySize = size - firstByte;
    if (snapshot.allocation && snapshot.firstByte <= firstByte && snapshot.endByte >= size &&
        std::memcmp(snapshot.allocation->memPtr + firstByte, source + firstByte, copySize) == 0)
    {
        m_mtlr->GetPerformanceMonitor().m_snapshotReuses++;
        return snapshot.allocation;
    }
    
    
    if (snapshot.allocation)
        m_snapshotAllocator.FreeReservation(snapshot.allocation);
    snapshot.allocation = m_snapshotAllocator.AllocateBufferMemory(std::max(size, 1u), 256);
    snapshot.firstByte = firstByte;
    snapshot.endByte = size;
    std::memcpy(snapshot.allocation->memPtr + firstByte, source + firstByte, copySize);
    m_snapshotAllocator.FlushReservation(snapshot.allocation);
    m_mtlr->GetPerformanceMonitor().m_snapshotBytes += copySize;
    m_mtlr->GetPerformanceMonitor().m_snapshotMisses++;
    return snapshot.allocation;
}

static void EncodeArgumentBufferSlot(MTL::ArgumentEncoder* encoder, const MetalArgumentBinding& binding, uint32 index)
{
    switch (binding.type)
    {
        case MetalArgumentBinding::Type::Unused:
            break;
        case MetalArgumentBinding::Type::Buffer:
            encoder->setBuffer(static_cast<MTL::Buffer*>(binding.resource), binding.value, index);
            break;
        case MetalArgumentBinding::Type::Texture:
            encoder->setTexture(static_cast<MTL::Texture*>(binding.resource), index);
            break;
        case MetalArgumentBinding::Type::Sampler:
            encoder->setSamplerState(static_cast<MTL::SamplerState*>(binding.resource), index);
            break;
        case MetalArgumentBinding::Type::Constant:
            if (void* constant = encoder->constantData(index))
                *static_cast<uint32*>(constant) = static_cast<uint32>(binding.value);
            break;
    }
}

MetalSynchronizedHeapAllocator::AllocatorReservation* MetalMemoryManager::GetCachedArgumentBuffer(uint32 stage, MTL::ArgumentEncoder* encoder, const MetalArgumentBindings& bindings)
{
    cemu_assert_debug(stage < METAL_SHADER_TYPE_TOTAL);
    m_mtlr->GetCommandBuffer();
    auto& snapshot = m_argumentSnapshots[stage];
    if (snapshot.encoder == encoder && snapshot.bindings == bindings)
    {
        m_mtlr->GetPerformanceMonitor().m_argumentBufferReuses++;
        return snapshot.allocation;
    }
    
    const bool sameEncoder = (snapshot.encoder == encoder);
    auto* oldAllocation = snapshot.allocation; // freed below, after any partial-encode memcpy has read it
    if (!sameEncoder)
    {
        if (snapshot.encoder)
            snapshot.encoder->release();
        snapshot.encoder = encoder->retain();
    }
    
    // Experimental (default OFF): adjust arg-buffer resource refcounts only for slots whose resource
    // pointer actually changed, instead of retaining every new resource and releasing every old one.
    // A resource present in both the old and new binding sets keeps its single snapshot reference across
    // the transition (it is never transiently dropped), which is strictly safer than release-then-retain.
    // Slots are index-aligned (fixed-size std::array, one [[id(i)]] each), so the comparison is valid
    // regardless of whether the encoder changed -- it is purely about which resources the snapshot
    // references before vs after. In busy scenes the VS arg buffer re-encodes ~every draw with only the
    // support-buffer slot changed, so this turns ~160 atomic refcount ops per re-encode into ~2.
    if (ActiveSettings::ExperimentalArgBufferIncrementalRefcount())
    {
        for (uint32 index = 0; index < bindings.size(); ++index)
        {
            void* newResource = bindings[index].resource;
            void* oldResource = snapshot.bindings[index].resource;
            if (newResource == oldResource)
                continue;
            if (newResource)
                static_cast<NS::Object*>(newResource)->retain();
            if (oldResource)
                static_cast<NS::Object*>(oldResource)->release();
        }
    }
    else
    {
        for (const auto& binding : bindings)
            if (binding.resource)
                static_cast<NS::Object*>(binding.resource)->retain();
        for (const auto& binding : snapshot.bindings)
            if (binding.resource)
                static_cast<NS::Object*>(binding.resource)->release();
    }
    const uint32 alignment = std::max<uint32>(256, static_cast<uint32>(encoder->alignment()));
    const uint32 encodedLength = static_cast<uint32>(encoder->encodedLength());

    // Experimental (default OFF): partial argument-buffer re-encode. When the encoder (i.e. the arg-buffer
    // layout) is unchanged and the previously-encoded allocation still exists, allocate a FRESH region
    // (arg buffers are read by the GPU at draw-execution time, so mutating the live one is unsafe), copy
    // the previously-encoded bytes into it, then re-encode ONLY the slots whose binding changed -- skipping
    // the whole-buffer memset and the ~40/88 per-slot setX calls that dominate the busy-scene VS re-encode
    // storm (where typically only the support-buffer slot changes per draw). ASSUMPTION (holds on Apple
    // Silicon in practice but is undocumented as a guarantee): encoded arg-buffer bytes are position-
    // independent, so an unchanged slot's bytes stay valid after being memcpy'd into a different
    // allocation/offset. This cannot be validated without on-device testing -> the Settings entry is OFF by
    // default and carries a compatibility warning.
    bool didPartialEncode = false;
    if (ActiveSettings::ExperimentalArgBufferPartialEncode() && sameEncoder && oldAllocation)
    {
        // Same encoder => identical layout => per-slot binding TYPES are invariant (only the resource/value
        // within a slot changes). Guard defensively: any type mismatch falls back to the full path.
        bool layoutStable = true;
        for (uint32 index = 0; index < bindings.size(); ++index)
            if (bindings[index].type != snapshot.bindings[index].type) { layoutStable = false; break; }
        if (layoutStable)
        {
            // Allocate the new region BEFORE freeing the old one so the two can never alias.
            auto* newAllocation = m_snapshotAllocator.AllocateBufferMemory(encodedLength, alignment);
            std::memcpy(newAllocation->memPtr, oldAllocation->memPtr, encodedLength);
            encoder->setArgumentBuffer(newAllocation->mtlBuffer, newAllocation->bufferOffset);
            for (uint32 index = 0; index < bindings.size(); ++index)
            {
                if (bindings[index] == snapshot.bindings[index])
                    continue;
                EncodeArgumentBufferSlot(encoder, bindings[index], index);
            }
            m_snapshotAllocator.FreeReservation(oldAllocation);
            snapshot.allocation = newAllocation;
            didPartialEncode = true;
        }
    }

    if (!didPartialEncode)
    {
        if (oldAllocation)
            m_snapshotAllocator.FreeReservation(oldAllocation);
        snapshot.allocation = m_snapshotAllocator.AllocateBufferMemory(encodedLength, alignment);
        std::memset(snapshot.allocation->memPtr, 0, snapshot.allocation->size);
        encoder->setArgumentBuffer(snapshot.allocation->mtlBuffer, snapshot.allocation->bufferOffset);
        for (uint32 index = 0; index < bindings.size(); ++index)
            EncodeArgumentBufferSlot(encoder, bindings[index], index);
    }
    snapshot.bindings = bindings;
    m_snapshotAllocator.FlushReservation(snapshot.allocation);
    m_mtlr->GetPerformanceMonitor().m_argumentBufferEncodes++;
    return snapshot.allocation;
}

void* MetalMemoryManager::AcquireTextureUploadBuffer(size_t size)
{
    if (m_textureUploadBuffer.size() < size)
    {
        m_textureUploadBuffer.resize(size);
    }

    return m_textureUploadBuffer.data();
}

void MetalMemoryManager::ReleaseTextureUploadBuffer(uint8* mem)
{
    cemu_assert_debug(m_textureUploadBuffer.data() == mem);
    m_textureUploadBuffer.clear();
}

void MetalMemoryManager::InitBufferCache(size_t size)
{
    cemu_assert_debug(!m_bufferCache);

    m_metalBufferCacheMode = g_current_game_profile->GetBufferCacheMode();

    if (m_metalBufferCacheMode == MetalBufferCacheMode::Auto)
    {
        // TODO: do this for all unified memory systems?
        if (m_mtlr->IsAppleGPU())
        {
            switch (CafeSystem::GetForegroundTitleId())
            {
            // The Legend of Zelda: Wind Waker HD
            case 0x0005000010143600: // EUR
            case 0x0005000010143500: // USA
            case 0x0005000010143400: // JPN
                // TODO: use host instead?
                m_metalBufferCacheMode = MetalBufferCacheMode::Host;
                break;
            default:
                m_metalBufferCacheMode = MetalBufferCacheMode::Host;
                break;
            }
        }
        else
        {
            m_metalBufferCacheMode = MetalBufferCacheMode::DevicePrivate;
        }
    }

    // First, try to import the host memory as a buffer
    if (m_metalBufferCacheMode == MetalBufferCacheMode::Host)
    {
        if (m_mtlr->HasUnifiedMemory())
        {
            m_importedMemBaseAddress = mmuRange_MEM2.getBase();
               m_hostAllocationSize = mmuRange_MEM2.getSize();
            m_importedMemoryBuffer = m_mtlr->GetDevice()->newBuffer(memory_getPointerFromVirtualOffset(m_importedMemBaseAddress), m_hostAllocationSize, MTL::ResourceStorageModeShared, nullptr);
            if (!m_importedMemoryBuffer)
            {
                cemuLog_log(LogType::Force, "Failed to import host memory as a buffer, using device shared mode instead");
                m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
                m_importedMemBaseAddress = 0;
                m_hostAllocationSize = 0;
            }
        }
        else
        {
            cemuLog_log(LogType::Force, "Host buffer cache mode is only available on unified memory systems, using device shared mode instead");
            m_metalBufferCacheMode = MetalBufferCacheMode::DeviceShared;
        }
    }

    if (!m_bufferCache)
        m_bufferCache = m_mtlr->GetDevice()->newBuffer(size, (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate ? MTL::ResourceStorageModePrivate : MTL::ResourceStorageModeShared));

    if (m_metalBufferCacheMode == MetalBufferCacheMode::DeviceShared)
        m_sharedTracker.Initialize(size);
    
    LatteBufferCache_hostSetVolatilityTracking(m_metalBufferCacheMode == MetalBufferCacheMode::Host);

#ifdef CEMU_DEBUG_ASSERT
    m_bufferCache->setLabel(GetLabel("Buffer cache", m_bufferCache));
    if (m_importedMemoryBuffer)
        m_importedMemoryBuffer->setLabel(GetLabel("Imported memory buffer", m_importedMemoryBuffer));
#endif
}

void MetalMemoryManager::UploadToBufferCache(const void* data, size_t offset, size_t size)
{
    if (size == 0)
        return;
    cemu_assert_debug(m_bufferCache);
    cemu_assert_debug((offset + size) <= m_bufferCache->length());

    if (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate || SharedCacheBusy(offset, size))
    {
        auto blitCommandEncoder = m_mtlr->GetBlitCommandEncoder();

        auto allocation = m_stagingAllocator.AllocateBufferMemory(size, 1);
        memcpy(allocation.memPtr, data, size);
        m_stagingAllocator.FlushReservation(allocation);

        blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, m_bufferCache, offset, size);
        TrackSharedCache(m_bufferCache, offset, size, true);

        //m_mtlr->CopyBufferToBuffer(allocation.mtlBuffer, allocation.bufferOffset, m_bufferCache, offset, size, ALL_MTL_RENDER_STAGES, ALL_MTL_RENDER_STAGES);
    }
    else
    {
        memcpy((uint8*)m_bufferCache->contents() + offset, data, size);
        NotifyBufferCacheRangeModified(offset, size);
    }

}

void MetalMemoryManager::CopyBufferCache(size_t srcOffset, size_t dstOffset, size_t size)
{
    if (size == 0 || srcOffset == dstOffset)
        return;
    cemu_assert_debug(m_bufferCache);
    if (m_metalBufferCacheMode == MetalBufferCacheMode::DevicePrivate ||
        SharedCacheBusy(srcOffset, size, true) || SharedCacheBusy(dstOffset, size))
    {
        m_mtlr->CopyBufferToBuffer(m_bufferCache, srcOffset, m_bufferCache, dstOffset, size, ALL_MTL_RENDER_STAGES, ALL_MTL_RENDER_STAGES);
        TrackSharedCache(m_bufferCache, srcOffset, size);
        TrackSharedCache(m_bufferCache, dstOffset, size, true);
    }
    else
    {
        memcpy((uint8*)m_bufferCache->contents() + dstOffset, (uint8*)m_bufferCache->contents() + srcOffset, size);
        NotifyBufferCacheRangeModified(dstOffset, size);
    }

}

bool MetalMemoryManager::SharedCacheBusy(size_t offset, size_t size, bool writesOnly) const
{
    return m_sharedTracker.Busy(offset, size, writesOnly);
}

void MetalMemoryManager::TrackSharedCache(MTL::Buffer* buffer, size_t offset, size_t size, bool write)
{
    if (m_metalBufferCacheMode == MetalBufferCacheMode::DeviceShared && buffer == m_bufferCache && size)
        m_sharedTracker.Mark(m_mtlr->GetCommandBuffer(), offset, size, write);
}

void MetalMemoryManager::NotifyBufferCacheRangeModified(size_t offset, size_t size)
{
    NotifyBufferRangeModified(m_bufferCache, offset, size);
}

void MetalMemoryManager::NotifyImportedMemoryRangeModified(size_t offset, size_t size)
{
    NotifyBufferRangeModified(m_importedMemoryBuffer, offset, size);
}

void MetalMemoryManager::NotifyBufferRangeModified(MTL::Buffer* buffer, size_t offset, size_t size)
{
    if (!buffer || size == 0 || buffer->storageMode() != MTL::StorageModeManaged)
        return;

    size_t bufferLength = buffer->length();
    if (offset >= bufferLength)
        return;

    size = std::min(size, bufferLength - offset);
    buffer->didModifyRange(NS::Range(offset, size));
}
