#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalVoidVertexPipeline.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureViewMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/RendererShaderMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/CachedFBOMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalOutputShaderCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalDepthStencilCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalSamplerCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalTextureBindCache.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalFXUpscaler.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureReadbackMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalQuery.h"
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/UtilityShaderSource.h"

#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "CafeSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/OS/libs/gx2/GX2GuestFrameTiming.h" // TEMP diagnostic instrumentation (behavior-neutral)
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "WindowSystem.h"

#include <cstring>

#define IMGUI_IMPL_METAL_CPP
#include "imgui/imgui_extension.h"
#include "imgui/imgui_impl_metal.h"

#define EVENT_VALUE_WRAP 4096

extern bool hasValidFramebufferAttached;

float supportBufferData[512 * 4];

// Diagnostic-only (behavior-neutral): early-return-safe RAII bracket for the per-category CPU-submit
// breakdown timers. When 'active' is false (debug overlay hidden) it does nothing, so normal play pays
// zero cost. The destructor closes the bracket on every code path, including the early returns in
// draw_execute / BindStageResources. Read out via getPreviousFrameValue() in AppendOverlayDebugInfo.
namespace
{
    struct ScopedStageTimer
    {
        LattePerfStatTimer* m_timer;
        ScopedStageTimer(LattePerfStatTimer& timer, bool active) : m_timer(active ? &timer : nullptr)
        {
            if (m_timer)
                m_timer->beginMeasuring();
        }
        ~ScopedStageTimer()
        {
            if (m_timer)
                m_timer->endMeasuring();
        }
    };
}

// Defined in the Common renderer
void LatteDraw_handleSpecialState8_clearAsDepth();

std::vector<MetalRenderer::DeviceInfo> MetalRenderer::GetDevices()
{
    NS_STACK_SCOPED auto devices = MTL::CopyAllDevices();
    std::vector<MetalRenderer::DeviceInfo> result;
    result.reserve(devices->count());
    for (uint32 i = 0; i < devices->count(); i++)
    {
        MTL::Device* device = static_cast<MTL::Device*>(devices->object(i));
        result.push_back({std::string(device->name()->utf8String()), device->registryID()});
    }

    return result;
}

MetalRenderer::MetalRenderer()
{
    // Options

    // Position invariance
    switch (g_current_game_profile->GetPositionInvariance())
    {
    case PositionInvariance::Auto:
        switch (CafeSystem::GetForegroundTitleId())
        {
        // Bayonetta
        case 0x0005000010157F00: // EUR
        case 0x0005000010157E00: // USA
        case 0x000500001014DB00: // JPN
        // Bayonetta 2
        case 0x0005000010172700: // EUR
        case 0x0005000010172600: // USA
        // Disney Planes
        case 0x0005000010136900: // EUR
        case 0x0005000010136A00: // EUR (TODO: check)
        case 0x0005000010136B00: // EUR (TODO: check)
        case 0x000500001011C500: // USA (TODO: check)
        // LEGO STAR WARS: The Force Awakens
        case 0x00050000101DAA00: // EUR
        case 0x00050000101DAB00: // USA
        // Mario Kart 8
        case 0x000500001010ED00: // EUR
        case 0x000500001010EC00: // USA
        case 0x000500001010EB00: // JPN
        case 0x0005000010183A00: // JPN (TODO: check)
        // Minecraft: Story Mode
        case 0x000500001020A300: // EUR
        case 0x00050000101E0100: // USA
        //case 0x000500001020a200: // USA
        // Ninja Gaiden 3: Razor's Edge
        case 0x0005000010110B00: // EUR
        case 0x0005000010139B00: // EUR (TODO: check)
        case 0x0005000010110A00: // USA
        case 0x0005000010110900: // JPN
        // Resident Evil: Revelations
        case 0x000500001012B400: // EUR
        case 0x000500001012CF00: // USA
        // Star Fox Zero
        case 0x00050000101B0500: // EUR
        case 0x0005000010201C00: // EUR (TODO: check)
        case 0x00050000101B0400: // USA
        case 0x0005000010201B00: // USA (TODO: check)
        // The Legend of Zelda: Breath of the Wild
        case 0x00050000101C9500: // EUR
        case 0x00050000101C9400: // USA
        case 0x00050000101C9300: // JPN
        // Wonderful 101
        case 0x0005000010135300: // EUR
        case 0x000500001012DC00: // USA
        case 0x0005000010116300: // JPN
        case 0x0005000010185600: // JPN (TODO: check)
            m_positionInvariance = true;
            break;
        default:
            m_positionInvariance = false;
            break;
        }
        break;
    case PositionInvariance::False:
        m_positionInvariance = false;
        break;
    case PositionInvariance::True:
        m_positionInvariance = true;
        break;
    }

    // Pick a device
    auto& config = GetConfig();
    const bool hasDeviceSet = config.mtl_graphic_device_uuid != 0;

    // If a device is set, try to find it
    if (hasDeviceSet)
    {
        NS_STACK_SCOPED auto devices = MTL::CopyAllDevices();
        for (uint32 i = 0; i < devices->count(); i++)
        {
            MTL::Device* device = static_cast<MTL::Device*>(devices->object(i));
            if (device->registryID() == config.mtl_graphic_device_uuid)
            {
                m_device = device;
                break;
            }
        }
    }

    if (!m_device)
    {
        if (hasDeviceSet)
        {
            cemuLog_log(LogType::Force, "The selected GPU ({}) could not be found. Using the system default device.", config.mtl_graphic_device_uuid);
            config.mtl_graphic_device_uuid = 0;
        }
        // Use the system default device
        m_device = MTL::CreateSystemDefaultDevice();
    }

    // Vendor
    const char* deviceName = m_device->name()->utf8String();
    if (memcmp(deviceName, "Apple", 5) == 0)
        m_vendor = GfxVendor::Apple;
    else if (memcmp(deviceName, "AMD", 3) == 0)
        m_vendor = GfxVendor::AMD;
    else if (memcmp(deviceName, "Intel", 5) == 0)
        m_vendor = GfxVendor::Intel;
    else if (memcmp(deviceName, "NVIDIA", 6) == 0)
        m_vendor = GfxVendor::Nvidia;
    else
        m_vendor = GfxVendor::Generic;

    // Feature support
    m_isAppleGPU = m_device->supportsFamily(MTL::GPUFamilyApple1);
    m_supportsFramebufferFetch = GetConfig().framebuffer_fetch.GetValue() ? m_device->supportsFamily(MTL::GPUFamilyApple2) : false;
    m_hasUnifiedMemory = m_device->hasUnifiedMemory();
    m_supportsMetal3 = m_device->supportsFamily(MTL::GPUFamilyMetal3);
    m_supportsMeshShaders = (m_supportsMetal3 && (m_vendor != GfxVendor::Intel || GetConfig().force_mesh_shaders.GetValue())); // Intel GPUs have issues with mesh shaders
    m_argumentBufferTier = m_device->argumentBuffersSupport();
    m_maxArgumentBufferSamplerCount = static_cast<uint32>(m_device->maxArgumentBufferSamplerCount());
    cemuLog_log(LogType::Force, "Metal argument buffers: Tier {}, {} samplers", m_argumentBufferTier == MTL::ArgumentBuffersTier2 ? 2 : 1, m_maxArgumentBufferSamplerCount);
    m_recommendedMaxVRAMUsage = m_device->recommendedMaxWorkingSetSize();
    m_pixelFormatSupport = MetalPixelFormatSupport(m_device);

    CheckForPixelFormatSupport(m_pixelFormatSupport);

    // Command queue
    m_commandQueue = m_device->newCommandQueue();

    // Synchronization resources
    m_event = m_device->newEvent();

    // Resources
    NS_STACK_SCOPED MTL::SamplerDescriptor* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
    samplerDescriptor->setSupportArgumentBuffers(true);
#ifdef CEMU_DEBUG_ASSERT
    samplerDescriptor->setLabel(GetLabel("Nearest sampler state", samplerDescriptor));
#endif
    m_nearestSampler = m_device->newSamplerState(samplerDescriptor);

    samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
    samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
#ifdef CEMU_DEBUG_ASSERT
    samplerDescriptor->setLabel(GetLabel("Linear sampler state", samplerDescriptor));
#endif
    m_linearSampler = m_device->newSamplerState(samplerDescriptor);

    // Null resources
    m_nullBuffer = m_device->newBuffer(64 * 1024, MTL::ResourceStorageModeShared);
    std::memset(m_nullBuffer->contents(), 0, m_nullBuffer->length());
#ifdef CEMU_DEBUG_ASSERT
    m_nullBuffer->setLabel(GetLabel("Null buffer", m_nullBuffer));
#endif

    NS_STACK_SCOPED MTL::TextureDescriptor* textureDescriptor = MTL::TextureDescriptor::alloc()->init();
    textureDescriptor->setTextureType(MTL::TextureType1D);
    textureDescriptor->setWidth(1);
    textureDescriptor->setUsage(MTL::TextureUsageShaderRead);
    m_nullTexture1D = m_device->newTexture(textureDescriptor);
#ifdef CEMU_DEBUG_ASSERT
    m_nullTexture1D->setLabel(GetLabel("Null texture 1D", m_nullTexture1D));
#endif

    textureDescriptor->setTextureType(MTL::TextureType2D);
    textureDescriptor->setHeight(1);
    textureDescriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
    m_nullTexture2D = m_device->newTexture(textureDescriptor);
#ifdef CEMU_DEBUG_ASSERT
    m_nullTexture2D->setLabel(GetLabel("Null texture 2D", m_nullTexture2D));
#endif

    m_memoryManager = new MetalMemoryManager(this);
    m_outputShaderCache = new MetalOutputShaderCache(this);
    m_pipelineCache = new MetalPipelineCache(this);
    m_depthStencilCache = new MetalDepthStencilCache(this);
    m_samplerCache = new MetalSamplerCache(this);
    m_textureBindCache = new MetalTextureBindCache();

    // Lower the commit treshold when buffer cache needs reduced latency
    if (m_memoryManager->NeedsReducedLatency())
        m_defaultCommitTreshlod = 64;
    else
        m_defaultCommitTreshlod = 196;

    // Adaptive Commit Cadence (experimental_extended_commit_threshold) starts at the static baseline;
    // UpdateAdaptiveCommitThreshold() moves it within [baseline, 2x baseline] once the toggle is on.
    m_adaptiveCommitThreshold = m_defaultCommitTreshlod;

    // Occlusion queries
    m_occlusionQuery.m_resultBuffer = m_device->newBuffer(OCCLUSION_QUERY_BUFFER_COUNT * OCCLUSION_QUERY_POOL_SIZE * sizeof(uint64), MTL::ResourceStorageModeShared);
#ifdef CEMU_DEBUG_ASSERT
    m_occlusionQuery.m_resultBuffer->setLabel(GetLabel("Occlusion query result buffer", m_occlusionQuery.m_resultBuffer));
#endif
    m_occlusionQuery.m_resultsPtr = (uint64*)m_occlusionQuery.m_resultBuffer->contents();
    std::fill_n(m_occlusionQuery.m_resultsPtr, OCCLUSION_QUERY_BUFFER_COUNT * OCCLUSION_QUERY_POOL_SIZE, uint64{0});

    // Reset vertex and uniform buffers
    for (uint32 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
    {
        m_state.m_vertexBuffers[i] = nullptr;
        m_state.m_vertexBufferOffsets[i] = INVALID_OFFSET;
        m_state.m_vertexBufferSizes[i] = 0;
    }

    for (uint32 i = 0; i < METAL_GENERAL_SHADER_TYPE_TOTAL; i++)
    {
        for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
            m_state.m_uniformBuffers[i][j] = nullptr;
        for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
            m_state.m_uniformBufferOffsets[i][j] = INVALID_OFFSET;
        for (uint32 j = 0; j < MAX_MTL_BUFFERS; j++)
            m_state.m_uniformBufferSizes[i][j] = 0;
    }

    // Utility shader library

    // Create the library
    NS::Error* error = nullptr;
    NS_STACK_SCOPED MTL::Library* utilityLibrary = m_device->newLibrary(ToNSString(utilityShaderSource), nullptr, &error);
    if (error)
    {
        cemuLog_log(LogType::Force, "failed to create utility library (error: {})", error->localizedDescription()->utf8String());
    }

    // Pipelines
    NS_STACK_SCOPED MTL::Function* vertexFullscreenFunction = utilityLibrary->newFunction(ToNSString("vertexFullscreen"));
    NS_STACK_SCOPED MTL::Function* fragmentCopyDepthToColorFunction = utilityLibrary->newFunction(ToNSString("fragmentCopyDepthToColor"));
    NS_STACK_SCOPED MTL::Function* fragmentCopyColorToDepthFunction = utilityLibrary->newFunction(ToNSString("fragmentCopyColorToDepth"));

    m_copyDepthToColorDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    m_copyDepthToColorDesc->setVertexFunction(vertexFullscreenFunction);
    m_copyDepthToColorDesc->setFragmentFunction(fragmentCopyDepthToColorFunction);
    m_copyDepthToColorDesc->colorAttachments()->object(0)->setWriteMask(MTL::ColorWriteMaskRed);

    m_copyColorToDepthDesc = MTL::RenderPipelineDescriptor::alloc()->init();
    m_copyColorToDepthDesc->setVertexFunction(vertexFullscreenFunction);
    m_copyColorToDepthDesc->setFragmentFunction(fragmentCopyColorToDepthFunction);

    NS_STACK_SCOPED MTL::DepthStencilDescriptor* copyColorToDepthStateDesc = MTL::DepthStencilDescriptor::alloc()->init();
    copyColorToDepthStateDesc->setDepthCompareFunction(MTL::CompareFunctionAlways);
    copyColorToDepthStateDesc->setDepthWriteEnabled(true);
    m_copyColorToDepthState = m_device->newDepthStencilState(copyColorToDepthStateDesc);

    // Void vertex pipelines
    if (m_isAppleGPU)
        m_copyBufferToBufferPipeline = new MetalVoidVertexPipeline(this, utilityLibrary, "vertexCopyBufferToBuffer");

    // HACK: for some reason, this variable ends up being initialized to some garbage data, even though its declared as bool m_captureFrame = false;
    m_occlusionQuery.m_lastCommandBuffer = nullptr;
    m_captureFrame = false;

    // Experimental MetalFX: latch the feature state ONCE, here at construction, from config. The
    // upscaler is allocated only when the user enabled it AND asked for a sub-native internal
    // resolution (< 100%) AND the device actually supports MetalFX. In every other case
    // (feature off, 100% = native, unsupported device) m_metalFXUpscaler stays nullptr and
    // m_metalFXActive stays false, so the present path is byte-identical to the pre-MetalFX
    // renderer. Nothing is re-read per frame; the present hot path is a single bool test.
    m_metalFXActive = false;
    m_metalFXRenderScale = 100;
    m_metalFXUpscaler = nullptr;

    if (ActiveSettings::ExperimentalMetalFXEnable())
    {
        sint32 scale = ActiveSettings::ExperimentalMetalFXRenderScale();
        if (scale < 25) scale = 25;
        else if (scale > 100) scale = 100;

        // Only Spatial (mode 0) is implemented. Temporal (mode 1) is intentionally not wired up: it
        // needs per-frame depth, motion vectors and a jitter sequence the color-only Wii U scanout
        // cannot provide, and the UI keeps it disabled. If the config somehow requests anything other
        // than Spatial (e.g. a hand-edited file), leave MetalFX off rather than fabricate those inputs.
        const bool modeIsSpatial = (ActiveSettings::ExperimentalMetalFXMode() == 0);

        // 100% means "native, no upscale" -> leave MetalFX entirely off so behavior is unchanged.
        if (modeIsSpatial && scale < 100)
        {
            if (MetalFXSpatialUpscaler::IsSupported(m_device))
            {
                m_metalFXRenderScale = scale;
                m_metalFXUpscaler = new MetalFXSpatialUpscaler(m_device);
                m_metalFXActive = true;
                cemuLog_log(LogType::Force, "MetalFX: experimental spatial upscaling enabled at {}% internal resolution", m_metalFXRenderScale);
            }
            else
            {
                cemuLog_log(LogType::Force, "MetalFX: requested but not supported on this device; using normal rendering");
            }
        }
        else if (!modeIsSpatial)
        {
            cemuLog_log(LogType::Force, "MetalFX: non-Spatial mode requested but only Spatial is implemented; using normal rendering");
        }
    }

    // Experimental MetalFX: publish the internal render-scale to the Latte texture layer so render
    // targets are created at the reduced resolution. 0 when MetalFX is inactive => native resolution,
    // no change. This is the only place the scale is armed; it is reset to 0 in the destructor.
    LatteTexture_setMetalFXRenderScalePercent(m_metalFXActive ? m_metalFXRenderScale : 0);

    // Experimental MetalFX "Selective Render Scaling": only meaningful while MetalFX scaling is active.
    // When active, scale only large render targets and keep small (UI/intermediate) ones native. Latched
    // here alongside the scale percent and reset to false in the destructor. When MetalFX is inactive the
    // scale percent is 0 so this flag has no effect regardless of its value.
    LatteTexture_setMetalFXSelectiveScaling(m_metalFXActive && ActiveSettings::ExperimentalMetalFXSelectiveScaling());

    // Experimental MetalFX "Direct Input": only meaningful while MetalFX scaling is active. Latched here
    // so it cannot flip mid-session; TryApplyMetalFX() reads only this member. When MetalFX is inactive
    // the scaler is never allocated so this flag has no effect regardless of its value.
    m_metalFXDirectInput = m_metalFXActive && ActiveSettings::ExperimentalMetalFXDirectInput();

    // Experimental MetalFX "Sharp Present": only meaningful while MetalFX scaling is active. Latched here
    // so it cannot flip mid-session; DrawBackbufferQuad() reads only this member and only after MetalFX
    // actually engaged, so it has no effect when MetalFX is inactive or passed the frame through.
    m_metalFXSharpPresent = m_metalFXActive && ActiveSettings::ExperimentalMetalFXSharpPresent();
}

MetalRenderer::~MetalRenderer()
{
    if (m_isAppleGPU)
        delete m_copyBufferToBufferPipeline;
    //delete m_copyTextureToTexturePipeline;
    //delete m_restrideBufferPipeline;

    m_copyDepthToColorDesc->release();
    for (const auto [pixelFormat, pipeline] : m_copyDepthToColorPipelines)
        pipeline->release();
    m_copyColorToDepthDesc->release();
    for (const auto [pixelFormat, pipeline] : m_copyColorToDepthPipelines)
        pipeline->release();
    m_copyColorToDepthState->release();

    delete m_outputShaderCache;
    delete m_pipelineCache;
    delete m_depthStencilCache;
    delete m_samplerCache;
    delete m_textureBindCache;
    delete m_memoryManager;

    // Experimental MetalFX: stop scaling newly-created render targets before teardown, then release
    // the scaler + owned intermediate textures. nullptr when the feature was never enabled, so this
    // is a no-op in the default configuration.
    LatteTexture_setMetalFXRenderScalePercent(0);
    LatteTexture_setMetalFXSelectiveScaling(false);
    delete m_metalFXUpscaler;

    m_nullBuffer->release();
    m_nullTexture1D->release();
    m_nullTexture2D->release();
    for (const auto& [key, texture] : m_nullSampledTextures)
    {
        if (texture)
            texture->release();
    }

    m_nearestSampler->release();
    m_linearSampler->release();

    if (m_readbackBuffer)
        m_readbackBuffer->release();

    if (m_xfbRingBuffer)
        m_xfbRingBuffer->release();
    for (MTL::Buffer* retiredBuffer : m_retiredXfbRingBuffers)
        retiredBuffer->release();

    m_occlusionQuery.m_resultBuffer->release();
    for (auto* completion : m_occlusionQuery.m_bufferCompletion)
        if (completion)
            completion->release();
    if (m_occlusionQuery.m_lastCommandBuffer)
        m_occlusionQuery.m_lastCommandBuffer->release();

    m_event->release();

    m_commandQueue->release();
    m_device->release();
}

void MetalRenderer::InitializeLayer(const Vector2i& size, bool mainWindow)
{
    auto& layer = GetLayer(mainWindow);
    layer = MetalLayerHandle(m_device, size, mainWindow);
    layer.GetLayer()->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
}

void MetalRenderer::ShutdownLayer(bool mainWindow)
{
    GetLayer(mainWindow) = MetalLayerHandle();
}

void MetalRenderer::ResizeLayer(const Vector2i& size, bool mainWindow)
{
    GetLayer(mainWindow).Resize(size);
}

void MetalRenderer::Initialize()
{
    Renderer::Initialize();
    RendererShaderMtl::Initialize();
}

void MetalRenderer::Shutdown()
{
    Flush(true);
    // TODO: should shutdown both layers
    ImGui_ImplMetal_Shutdown();
    Renderer::Shutdown();
    RendererShaderMtl::Shutdown();
}

bool MetalRenderer::IsPadWindowActive()
{
    return (GetLayer(false).GetLayer() != nullptr);
}

bool MetalRenderer::GetVRAMInfo(int& usageInMB, int& totalInMB) const
{
    // Subtract host memory from total VRAM, since it's shared with the CPU
    usageInMB = (m_device->currentAllocatedSize() - m_memoryManager->GetHostAllocationSize()) / 1024 / 1024;
    totalInMB = m_recommendedMaxVRAMUsage / 1024 / 1024;

    return true;
}

void MetalRenderer::ClearColorbuffer(bool padView)
{
    if (!AcquireDrawable(!padView))
        return;

    ClearColorTextureInternal(GetLayer(!padView).GetDrawable()->texture(), 0, 0, 0.0f, 0.0f, 0.0f, 1.0f);
}

void MetalRenderer::DrawEmptyFrame(bool mainWindow)
{
    if (!BeginFrame(mainWindow))
        return;
    SwapBuffers(mainWindow, !mainWindow);
}

void MetalRenderer::SwapBuffers(bool swapTV, bool swapDRC)
{
    // Diagnostic-only (behavior-neutral): wall-clock around drawable acquisition + PresentDrawable +
    // final command-buffer commit. Does NOT include the vsync pacing wait (that is IT_HLE_WAIT_FOR_FLIP,
    // measured separately as gpuTime_flipTime). Written once per frame; ResetPerFrameData does not clear
    // it, so it holds this value for the next frame's overlay draw (1-frame display lag).
    const HRTick presentStartTick = HighResolutionTimer::now().getTick();
    if (swapTV)
        SwapBuffer(true);
    if (swapDRC)
        SwapBuffer(false);

    // Reset the command buffers (they are released by TemporaryBufferAllocator)
    CommitCommandBuffer();
    const HRTick presentEndTick = HighResolutionTimer::now().getTick();
    m_performanceMonitor.m_presentTimeNs = (uint64)(HighResolutionTimer::getTimeDiff(presentStartTick, presentEndTick) * 1000000000.0);

    // Debug
    m_performanceMonitor.ResetPerFrameData();

    // Diagnostic-only: decide once per frame whether the per-category CPU-submit brackets run next frame.
    // Gated on the debug overlay being visible so normal play pays zero cost. 1-frame latency is fine.
    m_captureCpuStageTimings = GetConfig().overlay.debug;

    // GPU capture
    if (m_capturing)
    {
        EndCapture();
    }
    else if (m_captureFrame)
    {
        StartCapture();
        m_captureFrame = false;
    }
}

void MetalRenderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView) {
    if (!m_screenshot_requested && m_screenshot_state == ScreenshotState::None)
        return;

    if (m_mainLayer.GetDrawable())
    {
        // we already took a pad view screenshow and want a main window screenshot
        if (m_screenshot_state == ScreenshotState::Main && padView)
            return;

        if (m_screenshot_state == ScreenshotState::Pad && !padView)
            return;

        // remember which screenshot is left to take
        if (m_screenshot_state == ScreenshotState::None)
            m_screenshot_state = padView ? ScreenshotState::Main : ScreenshotState::Pad;
        else
            m_screenshot_state = ScreenshotState::None;
    }
    else
        m_screenshot_state = ScreenshotState::None;

    auto texMtl = static_cast<LatteTextureMtl*>(texView->baseTexture);

    int width, height;
    texMtl->GetEffectiveSize(width, height, 0);

    uint32 bytesPerRow = GetMtlTextureBytesPerRow(texMtl->format, texMtl->isDepth, width);
    uint32 size = GetMtlTextureBytesPerImage(texMtl->format, texMtl->isDepth, height, bytesPerRow);

    auto blitCommandEncoder = GetBlitCommandEncoder();

    auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
    auto buffer = bufferAllocator.AllocateBufferMemory(size, 1);

    blitCommandEncoder->copyFromTexture(texMtl->GetTexture(), 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1), buffer.mtlBuffer, buffer.bufferOffset, bytesPerRow, 0);

    bool formatValid = true;
    std::vector<uint8> rgb_data;
    rgb_data.reserve(3 * width * height);

    auto pixelFormat = texMtl->GetTexture()->pixelFormat();
    // TODO: implement more formats
    switch (pixelFormat)
    {
    case MTL::PixelFormatRGBA8Unorm:
        for (auto ptr = buffer.memPtr; ptr < buffer.memPtr + size; ptr += 4)
        {
            rgb_data.emplace_back(*ptr);
            rgb_data.emplace_back(*(ptr + 1));
            rgb_data.emplace_back(*(ptr + 2));
        }
        break;
    case MTL::PixelFormatRGBA8Unorm_sRGB:
        for (auto ptr = buffer.memPtr; ptr < buffer.memPtr + size; ptr += 4)
        {
            rgb_data.emplace_back(SRGBComponentToRGB(*ptr));
            rgb_data.emplace_back(SRGBComponentToRGB(*(ptr + 1)));
            rgb_data.emplace_back(SRGBComponentToRGB(*(ptr + 2)));
        }
        break;
    default:
        cemuLog_log(LogType::Force, "Unsupported screenshot texture pixel format {}", pixelFormat);
        formatValid = false;
        break;
    }

    if (formatValid)
        SaveScreenshot(rgb_data, width, height, !padView);
}

// True for the sRGB variants of the pixel formats a scanout present source can use. A texture with
// one of these formats is linearized by the hardware on sample, so any reader (including MetalFX)
// receives LINEAR values; a plain UNORM format returns the stored (gamma-encoded / perceptual)
// values unmodified. This distinction drives the MetalFX color-processing mode below.
static bool MetalFX_PixelFormatIsSRGB(MTL::PixelFormat format)
{
    switch (format)
    {
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatBGR10_XR_sRGB:
    case MTL::PixelFormatBGRA10_XR_sRGB:
        return true;
    default:
        return false;
    }
}

MTL::Texture* MetalRenderer::TryApplyMetalFX(MTL::Texture* sourceTexture, sint32 targetWidth, sint32 targetHeight)
{
    if (!sourceTexture || !m_metalFXUpscaler)
        return sourceTexture;

    const uint32 inputWidth = (uint32)sourceTexture->width();
    const uint32 inputHeight = (uint32)sourceTexture->height();
    const uint32 outputWidth = (uint32)std::max<sint32>(targetWidth, 1);
    const uint32 outputHeight = (uint32)std::max<sint32>(targetHeight, 1);

    // Spatial scaling only makes sense when upscaling. If the source already covers the target (e.g.
    // render scale left at 100% or a downscale case), pass the source through untouched so the present
    // path is unchanged.
    if (inputWidth >= outputWidth && inputHeight >= outputHeight)
        return sourceTexture;

    const MTL::PixelFormat colorFormat = sourceTexture->pixelFormat();

    // The scaler must be told the color space of the values it will READ from the color texture, i.e.
    // AFTER any automatic sRGB->linear conversion the pixel format implies. An sRGB format linearizes
    // on sample so the scaler sees LINEAR data (Linear mode); a plain UNORM format holding the game's
    // gamma-encoded scanout returns those values unmodified (Perceptual mode). Deriving the mode from
    // the format (rather than a user setting) is the fix for the "MetalFX is much darker" artifact:
    // an sRGB present source fed as Perceptual made MetalFX linearize a second time, darkening every
    // pixel. HDR is never applicable here (the Wii U scanout is LDR 8-bit).
    const sint32 colorProcessingMode = MetalFX_PixelFormatIsSRGB(colorFormat) ? 1 /*Linear*/ : 0 /*Perceptual*/;

    // (Re)configure lazily; cheap no-op when the key is unchanged. On any failure the upscaler releases
    // its partial state and returns false, and we fall back to the original source texture.
    if (!m_metalFXUpscaler->Configure(inputWidth, inputHeight, outputWidth, outputHeight, colorFormat, colorProcessingMode))
        return sourceTexture;

    MTL::Texture* inputTexture = m_metalFXUpscaler->GetInputTexture();
    MTL::Texture* outputTexture = m_metalFXUpscaler->GetOutputTexture();
    if (!inputTexture || !outputTexture)
        return sourceTexture;

    // Experimental "Direct Input": bind the reduced-resolution present source directly as the scaler's
    // color input and skip the per-frame copy into the owned input texture, saving that copy's
    // tile-memory bandwidth. Only taken when the source already carries every usage flag MetalFX
    // requires of its color texture (the scanout is created +sampled as a render target, so it normally
    // does). Everything is still recorded onto the SAME command buffer, so default hazard tracking keeps
    // produce -> scale -> sample ordered without an explicit fence.
    if (m_metalFXDirectInput)
    {
        const MTL::TextureUsage requiredUsage = m_metalFXUpscaler->GetRequiredColorTextureUsage();
        if (requiredUsage != MTL::TextureUsageUnknown && (sourceTexture->usage() & requiredUsage) == requiredUsage)
        {
            m_metalFXUpscaler->SetColorTexture(sourceTexture);
            EndEncoding(); // MetalFX must encode with no open encoder
            m_metalFXUpscaler->Encode(GetCommandBuffer());
            return outputTexture;
        }
        // Source cannot be bound directly: fall through to the copy path, first restoring the owned input
        // texture as the scaler's color input in case a previous frame bound an external one.
        m_metalFXUpscaler->SetColorTexture(inputTexture);
    }

    // Copy the reduced-resolution present source into the scaler's owned input texture, then let
    // MetalFX encode its own pass producing the full-resolution output. All three textures are tracked
    // (default hazard tracking) and everything is recorded onto the SAME command buffer, so Metal
    // orders copy-in -> scale -> downstream sample automatically without an explicit fence. MetalFX
    // must encode with no open encoder, so we end the blit encoder first.
    GetBlitCommandEncoder()->copyFromTexture(sourceTexture, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(inputWidth, inputHeight, 1),
                                             inputTexture, 0, 0, MTL::Origin(0, 0, 0));
    EndEncoding();

    m_metalFXUpscaler->Encode(GetCommandBuffer());

    return outputTexture;
}

void MetalRenderer::DrawBackbufferQuad(LatteTextureView* texView, RendererOutputShader* shader, bool useLinearTexFilter,
                                sint32 imageX, sint32 imageY, sint32 imageWidth, sint32 imageHeight,
                                bool padView, bool clearBackground)
{
    if (!AcquireDrawable(!padView))
        return;

    // M5: MetalFX (below) encodes directly on the command buffer, bypassing the encoder
    // funnels that would otherwise materialize deferred clears, and the present source may
    // itself be a pending-clear target. Flush here so nothing reads stale contents.
    FlushPendingClears();

    MTL::Texture* presentTexture = static_cast<LatteTextureViewMtl*>(texView)->GetRGBAView();

    // Experimental MetalFX spatial upscale (main window only). Dormant unless the feature is latched
    // ON and the upscaler was allocated; when engaged it upscales the (reduced-resolution) present
    // source into a full-resolution intermediate that the output-shader blit below samples exactly as
    // it would the original. Any failure returns presentTexture unchanged, so the present path is
    // never disturbed. Gated to !padView so the scaler is not recreated for the differently sized DRC.
    if (m_metalFXActive && m_metalFXUpscaler && !padView)
    {
        MTL::Texture* presentTextureBeforeMetalFX = presentTexture;
        presentTexture = TryApplyMetalFX(presentTexture, imageWidth, imageHeight);

        // Experimental "Sharp Present": MetalFX just upscaled to the exact present size, so the output
        // shader below would only resample it 1:1. If the user picked a multi-tap upscaling filter
        // (bicubic/Hermite) that pass is pure overhead and slightly re-softens MetalFX's result, so
        // collapse it to a single-tap nearest copy. Only when MetalFX actually engaged (texture changed);
        // preserves the upside-down shader variant. Default OFF => the chosen filter is used unchanged.
        if (m_metalFXSharpPresent && presentTexture != presentTextureBeforeMetalFX)
        {
            const bool upsideDown = (shader == RendererOutputShader::s_copy_shader_ud ||
                                     shader == RendererOutputShader::s_bicubic_shader_ud ||
                                     shader == RendererOutputShader::s_hermit_shader_ud);
            shader = upsideDown ? RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader;
            useLinearTexFilter = false;
        }
    }

    // Create render pass
    auto& layer = GetLayer(!padView);

    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(layer.GetDrawable()->texture());
    colorAttachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));
    // Experimental: when the output blit covers the entire drawable (clearBackground == false, i.e.
    // the image fills the screen), the full-screen triangle overwrites every pixel, so loading the
    // freshly-acquired drawable's undefined contents is pure wasted TBDR tile-load bandwidth. Use
    // DontCare in that case. Letterboxed output (clearBackground == true) still clears its borders.
    // Default OFF => unchanged LoadActionLoad, byte-identical present.
    MTL::LoadAction presentLoadAction;
    if (clearBackground)
        presentLoadAction = MTL::LoadActionClear;
    else if (ActiveSettings::ExperimentalPresentDontCare())
        presentLoadAction = MTL::LoadActionDontCare;
    else
        presentLoadAction = MTL::LoadActionLoad;
    colorAttachment->setLoadAction(presentLoadAction);
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    auto renderCommandEncoder = GetTemporaryRenderCommandEncoder(renderPassDescriptor);

    // Get a render pipeline

    // Find out which shader we are using
    uint8 shaderIndex = 255;
    if (shader == RendererOutputShader::s_copy_shader) shaderIndex = 0;
    else if (shader == RendererOutputShader::s_bicubic_shader) shaderIndex = 1;
    else if (shader == RendererOutputShader::s_hermit_shader) shaderIndex = 2;
    else if (shader == RendererOutputShader::s_copy_shader_ud) shaderIndex = 3;
    else if (shader == RendererOutputShader::s_bicubic_shader_ud) shaderIndex = 4;
    else if (shader == RendererOutputShader::s_hermit_shader_ud) shaderIndex = 5;

    uint8 shaderType = shaderIndex % 3;

    // Get the render pipeline state
    auto renderPipelineState = m_outputShaderCache->GetPipeline(shader, shaderIndex, m_state.m_usesSRGB);

    // Draw to Metal layer
    renderCommandEncoder->setRenderPipelineState(renderPipelineState);
    renderCommandEncoder->setFragmentTexture(presentTexture, 0);
    renderCommandEncoder->setFragmentSamplerState((useLinearTexFilter ? m_linearSampler : m_nearestSampler), 0);

    // Set uniforms
    float outputSize[2] = {(float)imageWidth, (float)imageHeight};
    switch (shaderType)
    {
    case 2:
        renderCommandEncoder->setFragmentBytes(outputSize, sizeof(outputSize), 0);
        break;
    default:
        break;
    }

    renderCommandEncoder->setViewport(MTL::Viewport{(double)imageX, (double)imageY, (double)imageWidth, (double)imageHeight, 0.0, 1.0});
    renderCommandEncoder->setScissorRect(MTL::ScissorRect{(uint32)imageX, (uint32)imageY, (uint32)imageWidth, (uint32)imageHeight});

    renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));

    EndEncoding();
}

bool MetalRenderer::BeginFrame(bool mainWindow)
{
    if (!AcquireDrawable(mainWindow))
        return false;
    
    ClearColorTextureInternal(GetLayer(mainWindow).GetDrawable()->texture(), 0, 0, 0.0f, 0.0f, 0.0f, 1.0f);
    return true;
}

void MetalRenderer::Flush(bool waitIdle)
{
    if (m_recordedDrawcalls > 0 || waitIdle)
        CommitCommandBuffer();

    if (waitIdle && m_executingCommandBuffers.size() != 0)
    {
        m_executingCommandBuffers.back()->waitUntilCompleted();
        ProcessFinishedCommandBuffers();
    }
}

void MetalRenderer::NotifyLatteCommandProcessorIdle()
{
    // Experimental: the Latte command processor calls this when its ring buffer has genuinely drained
    // (it has run out of guest commands to decode and is about to wait). When the frame is
    // CPU-throughput-bound the GPU is frequently starved, sitting idle until the commit threshold is
    // reached. Committing the pending (recorded but not yet submitted) draws at this idle point lets
    // the GPU begin that work immediately instead of waiting for more draws to accumulate, improving
    // CPU/GPU overlap. CommitCommandBuffer() ends the open encoder, no-ops without a command buffer,
    // and is guarded against double-commit, so this is safe to call here. It performs the same GPU
    // work, just sooner (one extra submission), so it does not spin harder or trade thermals for FPS.
    // Default OFF => unchanged no-op behavior.
    //
    // Guarded on an uncommitted command buffer that has recorded draws: m_recordedDrawcalls is only
    // reset when the *next* command buffer is created, so after a commit it stays > 0 until then. This
    // idle hook can fire on every iteration of a bounded-backoff stall (LatteCommandProcessor.cpp:549),
    // so the m_commited check keeps us from re-entering CommitCommandBuffer/ProcessFinishedCommandBuffers
    // repeatedly for the same already-submitted batch.
    if (ActiveSettings::ExperimentalCommitOnCpIdle() && m_recordedDrawcalls > 0
        && m_currentCommandBuffer.m_commandBuffer && !m_currentCommandBuffer.m_commited)
        CommitCommandBuffer();
}

bool MetalRenderer::ImguiBegin(bool mainWindow)
{
    if (!Renderer::ImguiBegin(mainWindow))
        return false;

    if (!AcquireDrawable(mainWindow))
        return false;

    EnsureImGuiBackend();

    // Check if the font texture needs to be built
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts->IsBuilt())
        ImGui_ImplMetal_CreateFontsTexture(m_device);

    auto& layer = GetLayer(mainWindow);

    // Render pass descriptor
    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(layer.GetDrawable()->texture());
    colorAttachment->setLoadAction(MTL::LoadActionLoad);
    colorAttachment->setStoreAction(MTL::StoreActionStore);

    // New frame
    ImGui_ImplMetal_NewFrame(renderPassDescriptor);
    ImGui_UpdateWindowInformation(mainWindow);
    ImGui::NewFrame();

    if (m_encoderType != MetalEncoderType::Render)
        GetTemporaryRenderCommandEncoder(renderPassDescriptor);

    return true;
}

void MetalRenderer::ImguiEnd()
{
    EnsureImGuiBackend();

    if (m_encoderType != MetalEncoderType::Render)
    {
        cemuLog_logOnce(LogType::Force, "no render command encoder, cannot draw ImGui");
        return;
    }

    ImGui::Render();
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), GetCurrentCommandBuffer(), (MTL::RenderCommandEncoder*)m_commandEncoder);
    //ImGui::EndFrame();

    EndEncoding();
}

ImTextureID MetalRenderer::GenerateTexture(const std::vector<uint8>& data, const Vector2i& size)
{
    try
    {
        std::vector <uint8> tmp(size.x * size.y * 4);
        for (size_t i = 0; i < data.size() / 3; ++i)
        {
            tmp[(i * 4) + 0] = data[(i * 3) + 0];
            tmp[(i * 4) + 1] = data[(i * 3) + 1];
            tmp[(i * 4) + 2] = data[(i * 3) + 2];
            tmp[(i * 4) + 3] = 0xFF;
        }

        NS_STACK_SCOPED MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
        desc->setTextureType(MTL::TextureType2D);
        desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        desc->setWidth(size.x);
        desc->setHeight(size.y);
        desc->setStorageMode(m_isAppleGPU ? MTL::StorageModeShared : MTL::StorageModeManaged);
        desc->setUsage(MTL::TextureUsageShaderRead);

        MTL::Texture* texture = m_device->newTexture(desc);

        // TODO: do a GPU copy?
        texture->replaceRegion(MTL::Region(0, 0, size.x, size.y), 0, 0, tmp.data(), size.x * 4, 0);

        return (ImTextureID)texture;
    }
    catch (const std::exception& ex)
    {
        cemuLog_log(LogType::Force, "can't generate imgui texture: {}", ex.what());
        return nullptr;
    }
}

void MetalRenderer::DeleteTexture(ImTextureID id)
{
    EnsureImGuiBackend();

    ((MTL::Texture*)id)->release();
}

void MetalRenderer::DeleteFontTextures()
{
    EnsureImGuiBackend();

    ImGui_ImplMetal_DestroyFontsTexture();
}

void MetalRenderer::AppendOverlayDebugInfo()
{
    ImGui::Text("--- GPU info ---");
    ImGui::Text("GPU                        %s", m_device->name()->utf8String());
    ImGui::Text("Is Apple GPU               %s", (m_isAppleGPU ? "yes" : "no"));
    ImGui::Text("Supports framebuffer fetch %s", (m_supportsFramebufferFetch ? "yes" : "no"));
    ImGui::Text("Has unified memory         %s", (m_hasUnifiedMemory ? "yes" : "no"));
    ImGui::Text("Supports Metal3            %s", (m_supportsMetal3 ? "yes" : "no"));

    ImGui::Text("--- Metal info ---");
    ImGui::Text("Render pipeline states     %zu", m_pipelineCache->GetPipelineCacheSize());

    ImGui::Text("--- Metal info (per frame) ---");
    ImGui::Text("Command buffers            %u", m_performanceMonitor.m_commandBuffers);
    ImGui::Text("Render passes              %u", m_performanceMonitor.m_renderPasses);
    ImGui::Text("Clears                     %u", m_performanceMonitor.m_clears);
    ImGui::Text("Manual vertex fetch draws  %u (mesh draws: %u)", m_performanceMonitor.m_manualVertexFetchDraws, m_performanceMonitor.m_meshDraws);
    ImGui::Text("Triangle fans              %u", m_performanceMonitor.m_triangleFans);
    ImGui::Text("Snapshot uploads           %llu KB (reuses: %u)", static_cast<unsigned long long>(m_performanceMonitor.m_snapshotBytes / 1024), m_performanceMonitor.m_snapshotReuses);
    ImGui::Text("Argument buffer encodes    %u (reuses: %u)", m_performanceMonitor.m_argumentBufferEncodes, m_performanceMonitor.m_argumentBufferReuses);
    ImGui::Text("Direct-binding draws       %u", m_performanceMonitor.m_directBindingDraws);
    ImGui::Text("Bind-loop skips            %u", m_performanceMonitor.m_bindLoopSkips);
    ImGui::Text("Support-buffer indirection %u", m_performanceMonitor.m_supportIndirectionDraws);

    ImGui::Text("--- Pass fragmentation (per frame) ---");
    ImGui::Text("Draw calls                 %u", m_performanceMonitor.m_drawCalls);
    ImGui::Text("Draw-pass begins           %u", m_performanceMonitor.m_drawPassBegins);
    {
        const float drawsPerPass = m_performanceMonitor.m_drawPassBegins ? ((float)m_performanceMonitor.m_drawCalls / (float)m_performanceMonitor.m_drawPassBegins) : 0.0f;
        ImGui::Text("Draws per pass             %.2f", drawsPerPass);
    }
    ImGui::Text("Snapshot misses            %u", m_performanceMonitor.m_snapshotMisses);
    {
        // ICB-batching premise: of m_drawCalls draws, how many continued a same-pipeline run in the same pass
        // (ICB-batchable candidates) and how long the single longest run was (best-case ICB batch size). Near-0
        // repeats / longest-run ~1 means "consecutive compatible draws" do not exist to batch -> ICB is moot.
        ImGui::Text("Same-pipeline draw repeats %u (longest run: %u)", m_performanceMonitor.m_drawPipelineRepeats, m_performanceMonitor.m_drawLongestPipelineRun);
    }

    ImGui::Text("--- Frame budget (per frame, prev) ---");
    {
        // All LattePerfStatTimer values are the PREVIOUS completed frame (getPreviousFrameValue), converted
        // from TSC ticks to microseconds. The per-frame COUNTS above are the CURRENT frame (reset in
        // ResetPerFrameData), so there is a 1-frame skew between the two groups - acceptable for a diagnostic.
        // Every line is a real measured span (timestamps around actual wait/sync/present points) or a real
        // GPU timestamp, NOT an estimate, except the single line explicitly marked "(derived)".
        const double frameSpanMs = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_frameTime.getPreviousFrameValue()) / 1000.0;
        const double idleMs      = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_idleTime.getPreviousFrameValue()) / 1000.0;
        const double fenceMs     = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_fenceTime.getPreviousFrameValue()) / 1000.0;
        const double readbackMs  = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_waitForAsync.getPreviousFrameValue()) / 1000.0;
        const double flipMs      = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_flipTime.getPreviousFrameValue()) / 1000.0;
        const double semaphoreMs = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_semaphoreTime.getPreviousFrameValue()) / 1000.0;
        const double occlusionMs = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_occlusionTime.getPreviousFrameValue()) / 1000.0;
        const double shaderMs    = PPCTimer_tscToMicroseconds(performanceMonitor.gpuTime_shaderCreate.getPreviousFrameValue()) / 1000.0;
        const double presentMs   = m_performanceMonitor.m_presentTimeNs / 1000000.0;
        const double gpuActiveMs = m_performanceMonitor.m_gpuActiveUs / 1000.0;
        const double waitSumMs   = idleMs + fenceMs + readbackMs + flipMs + semaphoreMs + occlusionMs + shaderMs;
        double submitMs = frameSpanMs - waitSumMs; // CPU work on the Latte thread not covered by a named wait
        if (submitMs < 0.0) submitMs = 0.0;
        ImGui::Text("Latte frame span (excl pres) %.2f ms", frameSpanMs);
        ImGui::Text("Present acq+present+commit   %.2f ms", presentMs);
        ImGui::Text("Frame total span+present     %.2f ms", frameSpanMs + presentMs);
        ImGui::Text("GPU active (reaped CBs)      %.2f ms (%u CB)", gpuActiveMs, m_performanceMonitor.m_gpuActiveCBs);
        ImGui::Text("Ring idle (guest starve)     %.2f ms", idleMs);
        ImGui::Text("Fence wait (WAIT_REG_MEM)    %.2f ms", fenceMs);
        ImGui::Text("Semaphore wait (MEM_SEM)     %.2f ms", semaphoreMs);
        ImGui::Text("Readback wait (async)        %.2f ms", readbackMs);
        ImGui::Text("Occlusion wait (CB done)     %.2f ms", occlusionMs);
        ImGui::Text("Flip wait (vsync pacing)     %.2f ms", flipMs);
        ImGui::Text("Shader decompile             %.2f ms", shaderMs);
        ImGui::Text("CPU submit (derived)         %.2f ms", submitMs);

        // CPU-submit per-category breakdown (Metal backend). Populated only while this overlay is shown
        // (m_captureCpuStageTimings). Each line is a real begin/end bracket around the named call site in
        // draw_execute / BindStageResources, summed over all draws in the previous frame. dcArgEncode and
        // dcResidency are subsets of dcBindStage; "support/uniform/tex" is the remainder of dcBindStage.
        const double beginSeqMs  = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcBeginSeq.getPreviousFrameValue()) / 1000.0;
        const double dcIndexMs   = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcIndex.getPreviousFrameValue()) / 1000.0;
        const double bufSyncMs   = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcBufferSync.getPreviousFrameValue()) / 1000.0;
        const double pipelineMs  = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcPipeline.getPreviousFrameValue()) / 1000.0;
        const double bindStageMs = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcBindStage.getPreviousFrameValue()) / 1000.0;
        const double argEncodeMs = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcArgEncode.getPreviousFrameValue()) / 1000.0;
        const double residencyMs = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcResidency.getPreviousFrameValue()) / 1000.0;
        const double drawEmitMs  = PPCTimer_tscToMicroseconds(performanceMonitor.cpuTime_dcDrawEmit.getPreviousFrameValue()) / 1000.0;
        double bindRemainderMs = bindStageMs - argEncodeMs - residencyMs; // support asm + snapshot + uniform loop + texture resolve
        if (bindRemainderMs < 0.0) bindRemainderMs = 0.0;
        const double dcAccountedMs = beginSeqMs + dcIndexMs + bufSyncMs + pipelineMs + bindStageMs + drawEmitMs;
        ImGui::Text("  [submit split - overlay on]");
        ImGui::Text("  beginSequence (per pass)   %.2f ms", beginSeqMs);
        ImGui::Text("  index decode               %.2f ms", dcIndexMs);
        ImGui::Text("  buffer/uniform sync        %.2f ms", bufSyncMs);
        ImGui::Text("  pipeline hash+lookup       %.2f ms", pipelineMs);
        ImGui::Text("  bindStage TOTAL            %.2f ms", bindStageMs);
        ImGui::Text("    - arg-buffer encode      %.2f ms", argEncodeMs);
        ImGui::Text("    - residency declare      %.2f ms", residencyMs);
        ImGui::Text("    - support/uniform/tex    %.2f ms", bindRemainderMs);
        ImGui::Text("  draw emit                  %.2f ms", drawEmitMs);
        ImGui::Text("  accounted sum              %.2f ms", dcAccountedMs);
    }

    ImGui::Text("--- Guest frame (producer, prev, TEMP diag) ---");
    {
        // TEMPORARY behavior-neutral instrumentation. The guest producer's per-frame wall clock,
        // delimited by successive GX2SwapScanBuffers on the GX2 main submit core, split into the
        // blocking categories timed at their funnels (GX2WaitTimeStamp / GX2WaitForFlip /
        // GX2WaitForVsync). Same raw-TSC clock as the Latte timers above, so directly comparable.
        // Layer 2 adds "Guest-internal (qAndW)" = time blocked inside queueAndWait, the common funnel
        // for ALL guest thread-blocking. It is a SUPERSET of flip/vsync (both call queueAndWait), while
        // GPU-retire is a separate GPU wait. "Truly unaccounted" = span - guest-internal: the closest
        // host-wall-clock estimate of non-blocking guest time, but NOT asserted to be CPU execution
        // (on a single host thread per core the descheduled interval can include other threads' work).
        // 1-frame skew vs the CURRENT counts, same as the block above.
        GX2::GuestFrameTimingState& gft = GX2::GetGuestFrameTiming();
        if (gft.prevValid.load(std::memory_order_relaxed) == 0)
        {
            ImGui::Text("Guest frame data             (waiting for main-core swap)");
        }
        else
        {
            const double gSpanMs     = PPCTimer_tscToMicroseconds(gft.prevFrameSpanTsc.load(std::memory_order_relaxed)) / 1000.0;
            const double gRetireMs   = PPCTimer_tscToMicroseconds(gft.prevGpuRetireWaitTsc.load(std::memory_order_relaxed)) / 1000.0;
            const double gFlipMs     = PPCTimer_tscToMicroseconds(gft.prevFlipWaitTsc.load(std::memory_order_relaxed)) / 1000.0;
            const double gVsyncMs    = PPCTimer_tscToMicroseconds(gft.prevVsyncWaitTsc.load(std::memory_order_relaxed)) / 1000.0;
            const double gInternalMs = PPCTimer_tscToMicroseconds(gft.prevGuestInternalWaitTsc.load(std::memory_order_relaxed)) / 1000.0;
            const double gWaitSum    = gRetireMs + gFlipMs + gVsyncMs;
            double gActiveMs = gSpanMs - gWaitSum; // Layer 1 remainder = span - GX2 waits (still includes guest-internal)
            if (gActiveMs < 0.0) gActiveMs = 0.0;
            double gTrulyUnaccMs = gSpanMs - gInternalMs; // Layer 2 remainder = span - guest-internal (flip/vsync are inside it)
            if (gTrulyUnaccMs < 0.0) gTrulyUnaccMs = 0.0;
            ImGui::Text("Guest frame span             %.2f ms", gSpanMs);
            ImGui::Text("GPU-retire wait (WaitTS)     %.2f ms", gRetireMs);
            ImGui::Text("Flip wait (GX2WaitForFlip)   %.2f ms", gFlipMs);
            ImGui::Text("Vsync wait (GX2WaitForVsync) %.2f ms", gVsyncMs);
            ImGui::Text("Guest wait sum               %.2f ms", gWaitSum);
            ImGui::Text("Guest active/unaccounted     %.2f ms (derived)", gActiveMs);
            ImGui::Text("Guest-internal (qAndW)       %.2f ms", gInternalMs);
            ImGui::Text("Truly unaccounted            %.2f ms (derived)", gTrulyUnaccMs);
            // Layer 3: decompose the SAME Guest-internal (qAndW) total by caller tag and by producer
            // (critical path) vs other main-core threads. Per-caller sum ~= Guest-internal above (modulo
            // a few-ns second-TSC-read skew). GpuRetire stays ~0 by construction (GX2WaitTimeStamp uses
            // TCLWaitTimestamp, not queueAndWait). Counts are per-frame wait entries on the main core.
            const double gGIOtherMs  = PPCTimer_tscToMicroseconds(gft.prevGICallerTsc[(uint32)GX2::GuestInternalCaller::Other].load(std::memory_order_relaxed)) / 1000.0;
            const double gGIFlipMs   = PPCTimer_tscToMicroseconds(gft.prevGICallerTsc[(uint32)GX2::GuestInternalCaller::Flip].load(std::memory_order_relaxed)) / 1000.0;
            const double gGIVsyncMs  = PPCTimer_tscToMicroseconds(gft.prevGICallerTsc[(uint32)GX2::GuestInternalCaller::Vsync].load(std::memory_order_relaxed)) / 1000.0;
            const double gGIRetireMs = PPCTimer_tscToMicroseconds(gft.prevGICallerTsc[(uint32)GX2::GuestInternalCaller::GpuRetire].load(std::memory_order_relaxed)) / 1000.0;
            const unsigned gGIOtherN  = (unsigned)gft.prevGICallerCount[(uint32)GX2::GuestInternalCaller::Other].load(std::memory_order_relaxed);
            const unsigned gGIFlipN   = (unsigned)gft.prevGICallerCount[(uint32)GX2::GuestInternalCaller::Flip].load(std::memory_order_relaxed);
            const unsigned gGIVsyncN  = (unsigned)gft.prevGICallerCount[(uint32)GX2::GuestInternalCaller::Vsync].load(std::memory_order_relaxed);
            const unsigned gGIRetireN = (unsigned)gft.prevGICallerCount[(uint32)GX2::GuestInternalCaller::GpuRetire].load(std::memory_order_relaxed);
            const double   gGIProducerMs = PPCTimer_tscToMicroseconds(gft.prevGIProducerTsc.load(std::memory_order_relaxed)) / 1000.0;
            const unsigned gGIProducerN  = (unsigned)gft.prevGIProducerCount.load(std::memory_order_relaxed);
            double gGIOtherThreadsMs = gInternalMs - gGIProducerMs; // non-producer (parallel worker) share
            if (gGIOtherThreadsMs < 0.0) gGIOtherThreadsMs = 0.0;
            ImGui::Text("  qAndW by caller (time / count):");
            ImGui::Text("    Other (mutex/evt/sem/..)   %.2f ms / %u", gGIOtherMs, gGIOtherN);
            ImGui::Text("    Flip                       %.2f ms / %u", gGIFlipMs, gGIFlipN);
            ImGui::Text("    Vsync                      %.2f ms / %u", gGIVsyncMs, gGIVsyncN);
            ImGui::Text("    GPU-retire (expect ~0)     %.2f ms / %u", gGIRetireMs, gGIRetireN);
            ImGui::Text("  qAndW producer/crit-path     %.2f ms / %u", gGIProducerMs, gGIProducerN);
            ImGui::Text("  qAndW other main-core thr.   %.2f ms (derived)", gGIOtherThreadsMs);
        }
    }

    ImGui::Text("--- Cache debug info ---");

    uint32 bufferCacheHeapSize = 0;
    uint32 bufferCacheAllocationSize = 0;
    uint32 bufferCacheNumAllocations = 0;

    LatteBufferCache_getStats(bufferCacheHeapSize, bufferCacheAllocationSize, bufferCacheNumAllocations);

    ImGui::Text("Buffer");
    ImGui::SameLine(60.0f);
    ImGui::Text("%06uKB / %06uKB Allocs: %u", (uint32)(bufferCacheAllocationSize + 1023) / 1024, ((uint32)bufferCacheHeapSize + 1023) / 1024, (uint32)bufferCacheNumAllocations);

    uint32 numBuffers;
    size_t totalSize, freeSize;

    m_memoryManager->GetStagingAllocator().GetStats(numBuffers, totalSize, freeSize);
    ImGui::Text("Staging");
    ImGui::SameLine(60.0f);
    ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, (uint32)numBuffers);

    m_memoryManager->GetIndexAllocator().GetStats(numBuffers, totalSize, freeSize);
    ImGui::Text("Index");
    ImGui::SameLine(60.0f);
    ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, (uint32)numBuffers);
    
    m_memoryManager->GetSnapshotStats(numBuffers, totalSize, freeSize);
    ImGui::Text("Snapshots");
    ImGui::SameLine(60.0f);
    ImGui::Text("%06uKB / %06uKB Buffers: %u", ((uint32)(totalSize - freeSize) + 1023) / 1024, ((uint32)totalSize + 1023) / 1024, numBuffers);
}

void MetalRenderer::renderTarget_setViewport(float x, float y, float width, float height, float nearZ, float farZ, bool halfZ)
{
    // halfZ is handled in the shader

    m_state.m_viewport = MTL::Viewport{x, y, width, height, nearZ, farZ};
}

void MetalRenderer::renderTarget_setScissor(sint32 scissorX, sint32 scissorY, sint32 scissorWidth, sint32 scissorHeight)
{
    m_state.m_scissor = MTL::ScissorRect{(uint32)scissorX, (uint32)scissorY, (uint32)scissorWidth, (uint32)scissorHeight};
}

LatteCachedFBO* MetalRenderer::rendertarget_createCachedFBO(uint64 key)
{
    return new CachedFBOMtl(this, key);
}

void MetalRenderer::rendertarget_deleteCachedFBO(LatteCachedFBO* cfbo)
{
    if (cfbo == (LatteCachedFBO*)m_state.m_activeFBO.m_fbo)
        m_state.m_activeFBO = {nullptr};
}

void MetalRenderer::rendertarget_bindFramebufferObject(LatteCachedFBO* cfbo)
{
    m_state.m_activeFBO = {(CachedFBOMtl*)cfbo, MetalAttachmentsInfo((CachedFBOMtl*)cfbo)};
    m_state.m_fboChanged = true;
}

void* MetalRenderer::texture_acquireTextureUploadBuffer(uint32 size)
{
    return m_memoryManager->AcquireTextureUploadBuffer(size);
}

void MetalRenderer::texture_releaseTextureUploadBuffer(uint8* mem)
{
    m_memoryManager->ReleaseTextureUploadBuffer(mem);
}

TextureDecoder* MetalRenderer::texture_chooseDecodedFormat(Latte::E_GX2SURFFMT format, bool isDepth, Latte::E_DIM dim, uint32 width, uint32 height)
{
    return GetMtlPixelFormatInfo(format, isDepth).textureDecoder;
}

void MetalRenderer::texture_clearSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex)
{
    if (hostTexture->isDepth)
    {
        texture_clearDepthSlice(hostTexture, sliceIndex, mipIndex, true, hostTexture->hasStencil, 0.0f, 0);
    }
    else
    {
        texture_clearColorSlice(hostTexture, sliceIndex, mipIndex, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

static MTL::BlitOption GetTextureUploadBlitOption(MTL::PixelFormat pixelFormat)
{
    switch (pixelFormat)
    {
    case MTL::PixelFormatDepth16Unorm:
    case MTL::PixelFormatDepth32Float:
        return MTL::BlitOptionDepthFromDepthStencil;
    case MTL::PixelFormatStencil8:
        return MTL::BlitOptionStencilFromDepthStencil;
    default:
        return MTL::BlitOptionNone;
    }
}

struct DepthStencilUploadLayout
{
    size_t sourceBytesPerTexel;
    size_t stencilOffset;
    bool maskDepthTo24Bit;
};

static DepthStencilUploadLayout GetDepthStencilUploadLayout(Latte::E_GX2SURFFMT format, const MetalPixelFormatInfo& formatInfo)
{
    switch (format)
    {
    case Latte::E_GX2SURFFMT::D24_S8_UNORM:
        if (formatInfo.pixelFormat == MTL::PixelFormatDepth24Unorm_Stencil8)
            return {4, 3, true};
        return {8, 4, false};
    case Latte::E_GX2SURFFMT::D24_S8_FLOAT:
    case Latte::E_GX2SURFFMT::D32_S8_FLOAT:
        return {8, 4, false};
    default:
        cemu_assert_suspicious();
        return {formatInfo.bytesPerBlock, formatInfo.bytesPerBlock == 4 ? (size_t)3 : (size_t)4, false};
    }
}

// Experimental (experimental_skip_redundant_upload, B3): 128-bit content fingerprint of an upload's raw
// bytes. Independent of the decode-cache hash (which is file-local to LatteTextureLoader and not exported);
// used only to decide whether the identical slice is already resident so the staging-copy + blit can be
// skipped. Reads every byte, folds the length into the seed, and avalanches at the end so a false match is
// not realistically reachable. Purely a decision input — never alters the bytes that get uploaded.
static inline void MtlHashUploadBytes(const void* data, uint32 size, uint64& outH0, uint64& outH1)
{
    const uint8* p = static_cast<const uint8*>(data);
    uint64 h0 = 1469598103934665603ull;                              // FNV-1a offset basis
    uint64 h1 = 0x9E3779B97F4A7C15ull ^ ((uint64)size * 0xFF51AFD7ED558CCDull);
    uint32 i = 0;
    for (; i + 8 <= size; i += 8)
    {
        uint64 block;
        memcpy(&block, p + i, sizeof(block));
        h0 = (h0 ^ block) * 1099511628211ull;                        // FNV-1a over 8-byte chunks
        uint64 k = block * 0xFF51AFD7ED558CCDull;
        k = (k << 31) | (k >> 33);                                   // rotl31
        h1 = (h1 ^ k) * 0x100000001B3ull + 0x9E3779B97F4A7C15ull;
    }
    if (i < size)
    {
        uint64 tail = 0;
        for (uint32 b = 0; i + b < size; ++b)
            tail |= (uint64)p[i + b] << (b * 8);
        h0 = (h0 ^ tail) * 1099511628211ull;
        h1 = (h1 ^ tail) * 0x100000001B3ull;
    }
    h0 ^= h0 >> 33; h0 *= 0xFF51AFD7ED558CCDull; h0 ^= h0 >> 29;     // final avalanche
    h1 ^= h1 >> 33; h1 *= 0xC4CEB9FE1A85EC53ull; h1 ^= h1 >> 32;
    outH0 = h0;
    outH1 = h1;
}

// TODO: do a cpu copy on Apple Silicon?
void MetalRenderer::texture_loadSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
    auto textureMtl = (LatteTextureMtl*)hostTexture;

    // Experimental "skip redundant texture upload" (B3): if this exact subresource already holds these
    // identical bytes, the staging-copy + GPU blit are pure waste. Only safe while the texture has never
    // been GPU-written (isUpdatedOnGPU is monotonic false->true): then the resident content came solely
    // from earlier texture_loadSlice calls, so a fingerprint match => the same bytes are already resident.
    // Depth is excluded (also covers the packed depth/stencil branch below). Key uses the ORIGINAL slice
    // index, captured here before the 3D fold rewrites it. OFF or any miss/uncertainty => normal upload.
    const uint64 reuseKey = ((uint64)(uint32)mipIndex << 32) | (uint32)sliceIndex;
    const bool reuseEligible = ActiveSettings::ExperimentalSkipRedundantUpload() && !textureMtl->isUpdatedOnGPU && !textureMtl->isDepth;
    uint64 reuseH0 = 0, reuseH1 = 0;
    if (reuseEligible)
    {
        MtlHashUploadBytes(pixelData, compressedImageSize, reuseH0, reuseH1);
        if (textureMtl->TryReuseUpload(reuseKey, reuseH0, reuseH1, compressedImageSize))
            return; // identical bytes already resident — skip staging copy + blit entirely
    }

    uint32 offsetZ = 0;
    if (textureMtl->Is3DTexture())
    {
        offsetZ = sliceIndex;
        sliceIndex = 0;
    }

    const auto& formatInfo = GetMtlPixelFormatInfo(textureMtl->format, textureMtl->isDepth);
    size_t bytesPerRow = GetMtlTextureBytesPerRow(textureMtl->format, textureMtl->isDepth, width);
    // No need to set bytesPerImage for 3D textures, since we always load just one slice
    //size_t bytesPerImage = GetMtlTextureBytesPerImage(textureMtl->GetFormat(), textureMtl->isDepth, height, bytesPerRow);
    //if (m_isAppleGPU)
    //{
    //    textureMtl->GetTexture()->replaceRegion(MTL::Region(0, 0, offsetZ, width, height, 1), mipIndex, sliceIndex, pixelData, bytesPerRow, 0);
    //}
    //else
    //{
    auto blitCommandEncoder = GetBlitCommandEncoder();

    if (textureMtl->isDepth && formatInfo.hasStencil)
    {
        const auto uploadLayout = GetDepthStencilUploadLayout(textureMtl->format, formatInfo);
        const size_t sourceBytesPerTexel = uploadLayout.sourceBytesPerTexel;
        const size_t pixelCount = (size_t)width * (size_t)height;
        const size_t expectedSourceSize = pixelCount * sourceBytesPerTexel;
        if ((sourceBytesPerTexel != 4 && sourceBytesPerTexel != 8) || uploadLayout.stencilOffset >= sourceBytesPerTexel || expectedSourceSize > compressedImageSize)
        {
            cemuLog_log(LogType::Force, "Invalid packed depth/stencil upload size for format {:04x}", (uint32)textureMtl->format);
            return;
        }
        
        constexpr size_t depthBytesPerTexel = sizeof(uint32);
        constexpr size_t stencilBytesPerTexel = sizeof(uint8);
        const size_t depthBytesPerRow = (size_t)width * depthBytesPerTexel;
        const size_t stencilBytesPerRow = (size_t)width * stencilBytesPerTexel;
        const size_t depthDataSize = depthBytesPerRow * (size_t)height;
        const size_t stencilDataSize = stencilBytesPerRow * (size_t)height;
        
        auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
        auto depthAllocation = bufferAllocator.AllocateBufferMemory(depthDataSize, depthBytesPerTexel);
        auto stencilAllocation = bufferAllocator.AllocateBufferMemory(stencilDataSize, stencilBytesPerTexel);
        
        const uint8* sourceData = static_cast<const uint8*>(pixelData);
        uint8* depthData = depthAllocation.memPtr;
        uint8* stencilData = stencilAllocation.memPtr;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8* sourceTexel = sourceData + i * sourceBytesPerTexel;
            uint32 depthValue;
            memcpy(&depthValue, sourceTexel, sizeof(depthValue));
            if (uploadLayout.maskDepthTo24Bit)
                depthValue &= 0x00FFFFFF;
            memcpy(depthData + i * depthBytesPerTexel, &depthValue, sizeof(depthValue));
            stencilData[i] = sourceTexel[uploadLayout.stencilOffset];
        }
        
        bufferAllocator.FlushReservation(depthAllocation);
        bufferAllocator.FlushReservation(stencilAllocation);
        
        const MTL::Size copySize(width, height, 1);
        const MTL::Origin destinationOrigin(0, 0, offsetZ);
        blitCommandEncoder->copyFromBuffer(depthAllocation.mtlBuffer, depthAllocation.bufferOffset, depthBytesPerRow, 0, copySize, textureMtl->GetTexture(), sliceIndex, mipIndex, destinationOrigin, MTL::BlitOptionDepthFromDepthStencil);
        blitCommandEncoder->copyFromBuffer(stencilAllocation.mtlBuffer, stencilAllocation.bufferOffset, stencilBytesPerRow, 0, copySize, textureMtl->GetTexture(), sliceIndex, mipIndex, destinationOrigin, MTL::BlitOptionStencilFromDepthStencil);
        return;
    }

    // Allocate a temporary buffer
    auto& bufferAllocator = m_memoryManager->GetStagingAllocator();
    auto allocation = bufferAllocator.AllocateBufferMemory(compressedImageSize, formatInfo.bytesPerBlock);
    memcpy(allocation.memPtr, pixelData, compressedImageSize);
    bufferAllocator.FlushReservation(allocation);

    // Copy the data from the temporary buffer to the texture
    blitCommandEncoder->copyFromBuffer(allocation.mtlBuffer, allocation.bufferOffset, bytesPerRow, 0, MTL::Size(width, height, 1), textureMtl->GetTexture(), sliceIndex, mipIndex, MTL::Origin(0, 0, offsetZ), GetTextureUploadBlitOption(formatInfo.pixelFormat));
    //}

    // B3: record what now resides in this subresource so an identical future upload can be skipped.
    if (reuseEligible)
        textureMtl->RecordUpload(reuseKey, reuseH0, reuseH1, compressedImageSize);
}

void MetalRenderer::texture_clearColorSlice(LatteTexture* hostTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
    if (!FormatIsRenderable(hostTexture->format))
    {
        cemuLog_logOnce(LogType::Force, "cannot clear color texture with format {}, because it's not renderable", hostTexture->format);
        return;
    }

    // Experimental "Partial Rendering": defer this clear and fold it into the next draw pass to the same
    // target (see m_pendingClears). Recording ends any open encoder, exactly as the immediate clear below
    // would have, so guest ordering (a clear breaks the current render pass) is preserved.
    if (ActiveSettings::ExperimentalPartialRendering())
    {
        if (m_commandEncoder)
            EndEncoding();
        PendingClear& rec = RecordPendingClear(hostTexture, sliceIndex, mipIndex, false);
        rec.r = r; rec.g = g; rec.b = b; rec.a = a;
        return;
    }

    auto mtlTexture = static_cast<LatteTextureMtl*>(hostTexture)->GetTexture();

    ClearColorTextureInternal(mtlTexture, sliceIndex, mipIndex, r, g, b, a);
}

void MetalRenderer::texture_clearDepthSlice(LatteTexture* hostTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
    clearStencil = (clearStencil && GetMtlPixelFormatInfo(hostTexture->format, true).hasStencil);
    if (!clearDepth && !clearStencil)
    {
        cemuLog_logOnce(LogType::Force, "skipping depth/stencil clear");
        return;
    }

    // Experimental "Partial Rendering": defer this clear (see m_pendingClears). clearStencil is stored
    // already format-gated so the flush/fold path never has to re-check it.
    if (ActiveSettings::ExperimentalPartialRendering())
    {
        if (m_commandEncoder)
            EndEncoding();
        PendingClear& rec = RecordPendingClear(hostTexture, (sint32)sliceIndex, mipIndex, true);
        rec.clearDepth = clearDepth;
        rec.clearStencil = clearStencil;
        rec.depthValue = depthValue;
        rec.stencilValue = stencilValue;
        return;
    }

    auto mtlTexture = static_cast<LatteTextureMtl*>(hostTexture)->GetTexture();
    ClearDepthTextureInternal(mtlTexture, sliceIndex, mipIndex, clearDepth, clearStencil, depthValue, stencilValue);
}

LatteTexture* MetalRenderer::texture_createTextureEx(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth, bool isRenderTarget)
{
    return new LatteTextureMtl(this, dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth, isRenderTarget);
}

void MetalRenderer::texture_setLatteTexture(LatteTextureView* textureView, uint32 textureUnit)
{
    m_state.m_textures[textureUnit] = static_cast<LatteTextureViewMtl*>(textureView);
}

void MetalRenderer::texture_notifyDelete(LatteTextureView* textureView)
{
    for (uint32 i = 0; i < std::size(m_state.m_textures); i++)
    {
        if (m_state.m_textures[i] == textureView)
            m_state.m_textures[i] = nullptr;
    }

    for (uint32 shaderType = 0; shaderType < METAL_SHADER_TYPE_TOTAL; shaderType++)
    {
        for (uint32 i = 0; i < MAX_MTL_TEXTURES; i++)
            m_state.m_encoderState.m_textures[shaderType][i] = nullptr;
    }

    // Drop any residency tracking that might reference this texture's underlying MTL resource: once
    // the texture is freed its pointer can be recycled for a new allocation, and a stale entry keyed on
    // that address could make DeclareResidency wrongly skip a useResource for the new resource. Clearing
    // over-declares at worst (never under-declares). No-op when experimental_skip_redundant_residency is
    // OFF, since the map is only ever populated on the ON path.
    m_residentResources.clear();
}

void MetalRenderer::texture_copyImageSubData(LatteTexture* src, sint32 srcMip, sint32 effectiveSrcX, sint32 effectiveSrcY, sint32 srcSlice, LatteTexture* dst, sint32 dstMip, sint32 effectiveDstX, sint32 effectiveDstY, sint32 dstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight, sint32 srcDepth_)
{
    // Source size seems to apply to the destination texture as well, therefore we need to adjust it when block size doesn't match
    Uvec2 srcBlockTexelSize = GetMtlPixelFormatInfo(src->format, src->isDepth).blockTexelSize;
    Uvec2 dstBlockTexelSize = GetMtlPixelFormatInfo(dst->format, dst->isDepth).blockTexelSize;
    if (srcBlockTexelSize.x != dstBlockTexelSize.x || srcBlockTexelSize.y != dstBlockTexelSize.y)
    {
        uint32 multX = (srcBlockTexelSize.x > dstBlockTexelSize.x ? srcBlockTexelSize.x / dstBlockTexelSize.x : dstBlockTexelSize.x / srcBlockTexelSize.x);
        effectiveCopyWidth *= multX;

        uint32 multY = (srcBlockTexelSize.y > dstBlockTexelSize.y ? srcBlockTexelSize.y / dstBlockTexelSize.y : dstBlockTexelSize.y / srcBlockTexelSize.y);
        effectiveCopyHeight *= multY;
    }

    auto blitCommandEncoder = GetBlitCommandEncoder();

    auto mtlSrc = static_cast<LatteTextureMtl*>(src)->GetTexture();
    auto mtlDst = static_cast<LatteTextureMtl*>(dst)->GetTexture();

    uint32 srcBaseLayer = 0;
    uint32 dstBaseLayer = 0;
    uint32 srcOffsetZ = 0;
    uint32 dstOffsetZ = 0;
    uint32 srcLayerCount = 1;
    uint32 dstLayerCount = 1;
    uint32 srcDepth = 1;
    uint32 dstDepth = 1;

    if (src->Is3DTexture())
    {
        srcOffsetZ = srcSlice;
        srcDepth = srcDepth_;
    }
    else
    {
        srcBaseLayer = srcSlice;
        srcLayerCount = srcDepth_;
    }

    if (dst->Is3DTexture())
    {
        dstOffsetZ = dstSlice;
        dstDepth = srcDepth_;
    }
    else
    {
        dstBaseLayer = dstSlice;
        dstLayerCount = srcDepth_;
    }

    // If copying whole textures, we can do a more efficient copy
    if (effectiveSrcX == 0 && effectiveSrcY == 0 && effectiveDstX == 0 && effectiveDstY == 0 &&
        srcOffsetZ == 0 && dstOffsetZ == 0 &&
        effectiveCopyWidth == src->GetMipWidth(srcMip) && effectiveCopyHeight == src->GetMipHeight(srcMip) && srcDepth == src->GetMipDepth(srcMip) &&
        effectiveCopyWidth == dst->GetMipWidth(dstMip) && effectiveCopyHeight == dst->GetMipHeight(dstMip) && dstDepth == dst->GetMipDepth(dstMip) &&
        srcLayerCount == dstLayerCount)
    {
        blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer, srcMip, mtlDst, dstBaseLayer, dstMip, srcLayerCount, 1);
    }
    else
    {
        if (srcLayerCount == dstLayerCount)
        {
            for (uint32 i = 0; i < srcLayerCount; i++)
            {
                blitCommandEncoder->copyFromTexture(mtlSrc, srcBaseLayer + i, srcMip, MTL::Origin(effectiveSrcX, effectiveSrcY, srcOffsetZ), MTL::Size(effectiveCopyWidth, effectiveCopyHeight, srcDepth), mtlDst, dstBaseLayer + i, dstMip, MTL::Origin(effectiveDstX, effectiveDstY, dstOffsetZ));
            }
        }
        else
        {
            for (uint32 i = 0; i < std::max(srcLayerCount, dstLayerCount); i++)
            {
                const uint32 currentSrcLayer = srcBaseLayer + (srcLayerCount == 1 ? 0 : i);
                const uint32 currentDstLayer = dstBaseLayer + (dstLayerCount == 1 ? 0 : i);
                const uint32 currentSrcZ = srcOffsetZ + (srcLayerCount == 1 ? i : 0);
                const uint32 currentDstZ = dstOffsetZ + (dstLayerCount == 1 ? i : 0);
                
                blitCommandEncoder->copyFromTexture(mtlSrc, currentSrcLayer, srcMip, MTL::Origin(effectiveSrcX, effectiveSrcY, currentSrcZ), MTL::Size(effectiveCopyWidth, effectiveCopyHeight, 1), mtlDst, currentDstLayer, dstMip, MTL::Origin(effectiveDstX, effectiveDstY, currentDstZ));
            }
        }
    }
}

LatteTextureReadbackInfo* MetalRenderer::texture_createReadback(LatteTextureView* textureView)
{
    size_t uploadSize = static_cast<LatteTextureMtl*>(textureView->baseTexture)->GetTexture()->allocatedSize();

    if ((m_readbackBufferWriteOffset + uploadSize) > TEXTURE_READBACK_SIZE)
    {
        m_readbackBufferWriteOffset = 0;
    }

    auto* result = new LatteTextureReadbackInfoMtl(this, textureView, m_readbackBufferWriteOffset);
    m_readbackBufferWriteOffset += uploadSize;

    return result;
}

void MetalRenderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
    // scale copy size to effective size
    sint32 effectiveCopyWidth = width;
    sint32 effectiveCopyHeight = height;
    LatteTexture_scaleToEffectiveSize(sourceTexture, &effectiveCopyWidth, &effectiveCopyHeight, 0);
    
    if (sourceTexture->isDepth == destinationTexture->isDepth)
    {
        cemu_assert_suspicious();
        return;
    }
    if (!LatteTexture_doesEffectiveRescaleRatioMatch(sourceTexture, srcMip, destinationTexture, dstMip))
    {
        cemuLog_logDebug(LogType::Force, "Metal surface copy with format conversion has mismatching dimensions");
        return;
    }
    if (sourceTexture->GetBPP() != destinationTexture->GetBPP())
    {
        cemuLog_logDebug(LogType::Force, "Metal surface copy with format conversion has mismatching BPP");
        return;
    }
    
    auto sourceView = static_cast<LatteTextureViewMtl*>(sourceTexture->GetOrCreateView(Latte::E_DIM::DIM_2D, sourceTexture->format, srcMip, 1, srcSlice, 1));
    auto destinationTextureMtl = static_cast<LatteTextureMtl*>(destinationTexture);
    MTL::Texture* destinationMtl = destinationTextureMtl->GetTexture();

    // Experimental (default OFF): when this surface copy fully covers the destination mip/slice, the
    // destination's previous contents are entirely overwritten by the copy, so loading them into tile
    // memory first is wasted bandwidth on a tile-based GPU. In that case use LoadActionDontCare for the
    // fully-written attachment (color, or depth). A partial copy keeps LoadActionLoad so the untouched
    // region is preserved. The stencil attachment (if any) is not written by the copy shader, so it
    // always keeps LoadActionLoad+StoreActionStore to preserve its contents. With the toggle off this is
    // exactly the original behavior (LoadActionLoad).
    MTL::LoadAction destLoadAction = MTL::LoadActionLoad;
    if (ActiveSettings::ExperimentalSurfaceCopyDestDontCare())
    {
        sint32 dstEffectiveWidth, dstEffectiveHeight;
        destinationTexture->GetEffectiveSize(dstEffectiveWidth, dstEffectiveHeight, dstMip);
        if (effectiveCopyWidth >= dstEffectiveWidth && effectiveCopyHeight >= dstEffectiveHeight)
            destLoadAction = MTL::LoadActionDontCare;
    }

    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPipelineState* pipeline = nullptr;
    if (destinationTexture->isDepth)
    {
        const auto& formatInfo = GetMtlPixelFormatInfo(destinationTexture->format, true);
        auto depthAttachment = renderPassDescriptor->depthAttachment();
        depthAttachment->setTexture(destinationMtl);
        depthAttachment->setLevel(dstMip);
        depthAttachment->setSlice(dstSlice);
        depthAttachment->setLoadAction(destLoadAction);
        depthAttachment->setStoreAction(MTL::StoreActionStore);
        
        if (formatInfo.hasStencil)
        {
            auto stencilAttachment = renderPassDescriptor->stencilAttachment();
            stencilAttachment->setTexture(destinationMtl);
            stencilAttachment->setLevel(dstMip);
            stencilAttachment->setSlice(dstSlice);
            stencilAttachment->setLoadAction(MTL::LoadActionLoad);
            stencilAttachment->setStoreAction(MTL::StoreActionStore);
        }
        
        auto& cachedPipeline = m_copyColorToDepthPipelines[formatInfo.pixelFormat];
        if (!cachedPipeline)
        {
            m_copyColorToDepthDesc->setDepthAttachmentPixelFormat(formatInfo.pixelFormat);
            m_copyColorToDepthDesc->setStencilAttachmentPixelFormat(formatInfo.hasStencil ? formatInfo.pixelFormat : MTL::PixelFormatInvalid);
            NS::Error* error = nullptr;
            cachedPipeline = m_device->newRenderPipelineState(m_copyColorToDepthDesc, &error);
            if (error)
                cemuLog_log(LogType::Force, "Failed to create Metal color-to-depth copy pipeline: {}", error->localizedDescription()->utf8String());
        }
        pipeline = cachedPipeline;
    }
    else
    {
        const MTL::PixelFormat pixelFormat = destinationMtl->pixelFormat();
        auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
        colorAttachment->setTexture(destinationMtl);
        colorAttachment->setLevel(dstMip);
        colorAttachment->setSlice(dstSlice);
        colorAttachment->setLoadAction(destLoadAction);
        colorAttachment->setStoreAction(MTL::StoreActionStore);
        
        auto& cachedPipeline = m_copyDepthToColorPipelines[pixelFormat];
        if (!cachedPipeline)
        {
            m_copyDepthToColorDesc->colorAttachments()->object(0)->setPixelFormat(pixelFormat);
            NS::Error* error = nullptr;
            cachedPipeline = m_device->newRenderPipelineState(m_copyDepthToColorDesc, &error);
            if (error)
                cemuLog_log(LogType::Force, "Failed to create Metal depth-to-color copy pipeline: {}", error->localizedDescription()->utf8String());
        }
        pipeline = cachedPipeline;
    }
    
    if (!pipeline)
        return;
    
    auto renderCommandEncoder = GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    renderCommandEncoder->setRenderPipelineState(pipeline);
    if (destinationTexture->isDepth)
        renderCommandEncoder->setDepthStencilState(m_copyColorToDepthState);
    renderCommandEncoder->setViewport(MTL::Viewport{0.0, 0.0, (double)effectiveCopyWidth, (double)effectiveCopyHeight, 0.0, 1.0});
    renderCommandEncoder->setScissorRect(MTL::ScissorRect{0, 0, (uint32)effectiveCopyWidth, (uint32)effectiveCopyHeight});
    SetTexture(renderCommandEncoder, METAL_SHADER_TYPE_FRAGMENT, sourceView->GetRGBAView(), GET_HELPER_TEXTURE_BINDING(0));
    renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
    EndEncoding();
}

void MetalRenderer::bufferCache_init(const sint32 bufferSize)
{
    m_memoryManager->InitBufferCache(bufferSize);
}

void MetalRenderer::bufferCache_upload(uint8* buffer, sint32 size, uint32 bufferOffset)
{
    m_memoryManager->UploadToBufferCache(buffer, bufferOffset, size);
}

void MetalRenderer::bufferCache_copy(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
    m_memoryManager->CopyBufferCache(srcOffset, dstOffset, size);
}

void MetalRenderer::bufferCache_copyStreamoutToMainBuffer(uint32 srcOffset, uint32 dstOffset, uint32 size)
{
    MTL::Buffer* dstBuffer = m_memoryManager->GetBufferCache();
    size_t dstBufferOffset = dstOffset;
    if (m_memoryManager->UseHostMemoryForCache())
    {
        if (m_memoryManager->IsRangeImported(dstOffset, size))
        {
            dstBuffer = m_memoryManager->GetImportedMemoryBuffer();
            dstBufferOffset = m_memoryManager->GetImportedMemoryOffset(dstOffset);
        }
        else
        {
            dstBufferOffset = LatteBufferCache_retrieveDataInCache(dstOffset, size);
        }
    }
    
    CopyBufferToBuffer(GetXfbRingBuffer(), srcOffset, dstBuffer, dstBufferOffset, size, MTL::RenderStageVertex | MTL::RenderStageMesh, ALL_MTL_RENDER_STAGES);
    m_memoryManager->TrackSharedCache(dstBuffer, dstBufferOffset, size, true);
}

MTL::Buffer* MetalRenderer::GetXfbRingBuffer(size_t minimumSize)
{
    const size_t initialCapacity = static_cast<size_t>(LatteStreamout_GetRingBufferSize());
    minimumSize = std::max(minimumSize, initialCapacity);
    if (m_xfbRingBuffer && m_xfbRingBuffer->length() >= minimumSize)
        return m_xfbRingBuffer;
    
    if (minimumSize > m_device->maxBufferLength())
    {
        cemuLog_logOnce(LogType::Force, "Metal streamout allocation exceeds the device buffer limit: {} bytes", minimumSize);
        return nullptr;
    }
    
    size_t allocationSize = m_xfbRingBuffer ? m_xfbRingBuffer->length() : initialCapacity;
    while (allocationSize < minimumSize && allocationSize <= (std::numeric_limits<size_t>::max() / 2))
        allocationSize *= 2;
    if (allocationSize < minimumSize)
        allocationSize = Align(minimumSize, 1024 * 1024);
    allocationSize = std::min<size_t>(allocationSize, m_device->maxBufferLength());

    MTL::Buffer* newBuffer = m_device->newBuffer(allocationSize, MTL::ResourceStorageModePrivate);
    if (!newBuffer)
    {
        cemuLog_logOnce(LogType::Force, "Failed to allocate {} byte Metal streamout buffer", allocationSize);
        return nullptr;
    }
#ifdef CEMU_DEBUG_ASSERT
    newBuffer->setLabel(GetLabel("Transform feedback buffer", newBuffer));
#endif
    if (m_xfbRingBuffer)
        m_retiredXfbRingBuffers.emplace_back(m_xfbRingBuffer);
    m_xfbRingBuffer = newBuffer;
    return m_xfbRingBuffer;
}

MTL::Texture* MetalRenderer::GetNullSampledTexture(Latte::E_DIM dim, bool integerFormat, bool depthFormat)
{
    const uint32 key = static_cast<uint32>(dim) | (integerFormat ? 0x100u : 0u) | (depthFormat ? 0x200u : 0u);
    auto& texture = m_nullSampledTextures[key];
    if (texture)
        return texture;
    
    NS_STACK_SCOPED MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setWidth(1);
    descriptor->setHeight(1);
    descriptor->setDepth(1);
    descriptor->setArrayLength(1);
    descriptor->setMipmapLevelCount(1);
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setPixelFormat(depthFormat ? MTL::PixelFormatDepth32Float : (integerFormat ? MTL::PixelFormatRGBA8Uint : MTL::PixelFormatRGBA8Unorm));
    
    switch (dim)
    {
        case Latte::E_DIM::DIM_1D:
            descriptor->setTextureType(MTL::TextureType1D);
            break;
        case Latte::E_DIM::DIM_2D:
        case Latte::E_DIM::DIM_2D_MSAA:
            descriptor->setTextureType(MTL::TextureType2D);
            break;
        case Latte::E_DIM::DIM_2D_ARRAY:
        case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
            descriptor->setTextureType(MTL::TextureType2DArray);
            break;
        case Latte::E_DIM::DIM_CUBEMAP:
            descriptor->setTextureType(MTL::TextureTypeCubeArray);
            break;
        case Latte::E_DIM::DIM_3D:
            descriptor->setTextureType(MTL::TextureType3D);
            break;
        default:
            descriptor->setTextureType(MTL::TextureType2D);
            break;
    }
    
    texture = m_device->newTexture(descriptor);
#ifdef CEMU_DEBUG_ASSERT
    if (texture)
        texture->setLabel(GetLabel("Typed null sampled texture", texture));
#endif
    return texture ? texture : m_nullTexture2D;
}

void MetalRenderer::buffer_bindVertexBuffer(uint32 bufferIndex, uint32 offset, uint32 size)
{
    cemu_assert_debug(bufferIndex < LATTE_MAX_VERTEX_BUFFERS);

    MTL::Buffer* buffer = m_memoryManager->GetBufferCache();
    if (!buffer || offset >= buffer->length())
    {
        m_state.m_vertexBuffers[bufferIndex] = nullptr;
        m_state.m_vertexBufferOffsets[bufferIndex] = INVALID_OFFSET;
        m_state.m_vertexBufferSizes[bufferIndex] = 0;
        return;
    }

    m_state.m_vertexBuffers[bufferIndex] = buffer;
    m_state.m_vertexBufferOffsets[bufferIndex] = offset;
    m_state.m_vertexBufferSizes[bufferIndex] = std::min<size_t>(size, buffer->length() - offset);
}

void MetalRenderer::buffer_bindUniformBuffer(LatteConst::ShaderType shaderType, uint32 bufferIndex, uint32 offset, uint32 size)
{
    cemu_assert_debug(bufferIndex < 16);
    MetalGeneralShaderType mtlShaderType = GetMtlGeneralShaderType(shaderType);
    cemu_assert_debug(mtlShaderType < METAL_GENERAL_SHADER_TYPE_TOTAL);

    if (size == 0)
    {
        m_state.m_uniformBuffers[mtlShaderType][bufferIndex] = nullptr;
        m_state.m_uniformBufferOffsets[mtlShaderType][bufferIndex] = INVALID_OFFSET;
        m_state.m_uniformBufferSizes[mtlShaderType][bufferIndex] = 0;
        return;
    }

    m_state.m_uniformBuffers[mtlShaderType][bufferIndex] = m_memoryManager->GetBufferCache();
    m_state.m_uniformBufferOffsets[mtlShaderType][bufferIndex] = offset;
    m_state.m_uniformBufferSizes[mtlShaderType][bufferIndex] = size;
}

RendererShader* MetalRenderer::shader_create(RendererShader::ShaderType type, uint64 baseHash, uint64 auxHash, const std::string& source, bool isGameShader, bool isGfxPackShader)
{
    return new RendererShaderMtl(this, type, baseHash, auxHash, isGameShader, isGfxPackShader, source);
}

void MetalRenderer::streamout_setupXfbBuffer(uint32 bufferIndex, sint32 ringBufferOffset, uint32 rangeAddr, uint32 rangeSize)
{
    cemu_assert_debug(bufferIndex < LATTE_NUM_STREAMOUT_BUFFER);
    auto& streamoutBuffer = m_state.m_streamoutState.buffers[bufferIndex];
    streamoutBuffer = {};
    if (ringBufferOffset < 0)
        return;

    const uint64 requiredSize = static_cast<uint64>(ringBufferOffset) + rangeSize;
    if (requiredSize > std::numeric_limits<size_t>::max() || !GetXfbRingBuffer(static_cast<size_t>(requiredSize)))
        return;

    streamoutBuffer.enabled = rangeSize != 0;
    streamoutBuffer.ringBufferOffset = static_cast<uint32>(ringBufferOffset);
    streamoutBuffer.rangeSize = rangeSize;
}

void MetalRenderer::streamout_begin()
{
    // Do nothing
}

void MetalRenderer::streamout_rendererFinishDrawcall()
{
    m_state.m_streamoutState = {};
}

void MetalRenderer::draw_beginSequence()
{
    // Diagnostic-only: RAII at function scope times the whole per-pass setup (incl. all early returns).
    ScopedStageTimer _stBeginSeq(performanceMonitor.cpuTime_dcBeginSeq, m_captureCpuStageTimings);
    m_state.m_skipDrawSequence = false;

    m_performanceMonitor.m_drawPassBegins++;

    // New draw sequence = new CP draw pass boundary: any context/resource/sampler write since the
    // last pass ended it, so advance the per-pass generation. Experimental state-cache fast-paths
    // use this token to detect that nothing feeding their hashes has changed within a pass.
    m_drawPassGeneration++;

    bool streamoutEnable = LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] != 0;

    // update shader state
    LatteSHRC_UpdateActiveShaders();
    if (LatteGPUState.activeShaderHasError)
    {
        cemuLog_logOnce(LogType::Force, "Skipping drawcalls due to shader error\n");
        m_state.m_skipDrawSequence = true;
        cemu_assert_debug(false);
        return;
    }

    // update render target and texture state
    LatteGPUState.requiresTextureBarrier = false;
    while (true)
    {
        LatteGPUState.repeatTextureInitialization = false;
        if (!LatteMRT::UpdateCurrentFBO())
        {
            cemuLog_logOnce(LogType::Force, "Rendertarget invalid\n");
            m_state.m_skipDrawSequence = true;
            return; // no render target
        }

        if (!hasValidFramebufferAttached && !streamoutEnable)
        {
            cemuLog_logOnce(LogType::Force, "Drawcall with no color buffer or depth buffer attached\n");
            m_state.m_skipDrawSequence = true;
            return; // no render target
        }
        LatteTexture_updateTextures();
        if (!LatteGPUState.repeatTextureInitialization)
            break;
    }

    // apply render target
    LatteMRT::ApplyCurrentState();

    // viewport and scissor box
    LatteRenderTarget_updateViewport();
    LatteRenderTarget_updateScissorBox();

    if (!LatteGPUState.contextNew.IsRasterizationEnabled() && !streamoutEnable)
        m_state.m_skipDrawSequence = true;
}

void MetalRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, bool isFirst)
{
    m_performanceMonitor.m_drawCalls++;

    if (m_state.m_skipDrawSequence)
    {
        LatteGPUState.drawCallCounter++;
        return;
    }

    // fast clear color as depth
    if (LatteGPUState.contextNew.GetSpecialStateValues()[8] != 0)
    {
        LatteDraw_handleSpecialState8_clearAsDepth();
        LatteGPUState.drawCallCounter++;
        return;
    }
    else if (LatteGPUState.contextNew.GetSpecialStateValues()[5] != 0)
    {
        draw_handleSpecialState5();
        LatteGPUState.drawCallCounter++;
        return;
    }

    auto& encoderState = m_state.m_encoderState;

    // Shaders
    LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
    LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
    LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
    const auto fetchShader = LatteSHRC_GetActiveFetchShader();

    if (!m_state.m_isFirstDrawInRenderPass)
    {
        bool endRenderPass = CheckIfRenderPassNeedsFlush(pixelShader);
        if (!endRenderPass)
            endRenderPass = CheckIfRenderPassNeedsFlush(vertexShader);
        if (!endRenderPass && geometryShader)
            endRenderPass = CheckIfRenderPassNeedsFlush(geometryShader);
        
        if (endRenderPass)
        {
            EndEncoding();
            cemuLog_logOnce(LogType::Force, "Ending Metal render pass due to render target self-dependency");
        }
    }

    // Primitive type
    const LattePrimitiveMode primitiveMode = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
    auto mtlPrimitiveType = GetMtlPrimitiveType(primitiveMode);

    bool usesGeometryShader = UseGeometryShader(LatteGPUState.contextNew, geometryShader != nullptr);
    if (usesGeometryShader && !m_supportsMeshShaders)
        return;

    const bool usesVertexStreamout = !usesGeometryShader && vertexShader->hasStreamoutBufferWrite;
    bool fetchVertexManually = usesGeometryShader || usesVertexStreamout || fetchShader->mtlFetchVertexManually;
    
    
    PrepareOcclusionQueryDraw();

    // Index buffer
    Renderer::INDEX_TYPE hostIndexType;
    uint32 hostIndexCount;
    uint32 indexMin = 0;
    uint32 indexMax = 0;
    Renderer::IndexAllocation indexAllocation;
    {
        ScopedStageTimer _stIndex(performanceMonitor.cpuTime_dcIndex, m_captureCpuStageTimings);
        LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMin, indexMax, hostIndexType, hostIndexCount, indexAllocation);
    }
    auto indexAllocationMtl = static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(indexAllocation.rendererInternal);
    const sint32 signedBaseVertex = static_cast<sint32>(baseVertex);
    m_state.m_drawResources.indexBuffer = indexAllocationMtl ? indexAllocationMtl->mtlBuffer : nullptr;
    m_state.m_drawResources.indexBufferOffset = indexAllocationMtl ? indexAllocationMtl->bufferOffset : 0;
    m_state.m_drawResources.indexBufferSize = indexAllocationMtl ? indexAllocationMtl->size : 0;
    m_state.m_drawResources.indexType = static_cast<uint32>(hostIndexType);
    m_state.m_drawResources.baseVertex = signedBaseVertex;
    m_state.m_drawResources.baseInstance = baseInstance;
    
    uint32 minVertexIndex = baseVertex;
    uint32 maxVertexIndex = count > 0 ? baseVertex + count - 1 : baseVertex;
    if (hostIndexType != INDEX_TYPE::NONE)
    {
        sint64 signedMinVertexIndex = (sint64)indexMin + signedBaseVertex;
        sint64 signedMaxVertexIndex = (sint64)indexMax + signedBaseVertex;
        minVertexIndex = signedMinVertexIndex <= 0 ? 0 : (uint32)std::min<sint64>(signedMinVertexIndex, std::numeric_limits<uint32>::max());
        maxVertexIndex = signedMaxVertexIndex <= 0 ? 0 : (uint32)std::min<sint64>(signedMaxVertexIndex, std::numeric_limits<uint32>::max());
    }

    // Buffer cache
    if (m_captureCpuStageTimings) performanceMonitor.cpuTime_dcBufferSync.beginMeasuring();
    if (m_memoryManager->UseHostMemoryForCache())
    {
        // direct memory access (Wii U memory space imported as a buffer), update buffer bindings
        LatteBufferCache_processDCFlushQueue();
        LatteBufferCache_processDeallocations();
        draw_updateVertexBuffersDirectAccess(minVertexIndex, maxVertexIndex, baseInstance, instanceCount, fetchVertexManually);
        if (vertexShader)
            draw_updateUniformBuffersDirectAccess(vertexShader, mmSQ_VTX_UNIFORM_BLOCK_START);
        if (geometryShader)
            draw_updateUniformBuffersDirectAccess(geometryShader, mmSQ_GS_UNIFORM_BLOCK_START);
        if (pixelShader)
            draw_updateUniformBuffersDirectAccess(pixelShader, mmSQ_PS_UNIFORM_BLOCK_START);
    }
    else
    {
        // synchronize vertex and uniform cache and update buffer bindings
        // We need to call this before getting the render command encoder, since it can cause buffer copies
        LatteBufferCache_Sync(minVertexIndex, maxVertexIndex, baseInstance, instanceCount);
    }

    PrepareUniformBufferSizes(vertexShader);
    if (usesGeometryShader)
        PrepareUniformBufferSizes(geometryShader);
    PrepareUniformBufferSizes(pixelShader);
    if (m_captureCpuStageTimings) performanceMonitor.cpuTime_dcBufferSync.endMeasuring();

    // Render pass
    auto renderCommandEncoder = GetRenderCommandEncoder();

    // Render pipeline state
    PipelineObject* pipelineObj;
    {
        ScopedStageTimer _stPipeline(performanceMonitor.cpuTime_dcPipeline, m_captureCpuStageTimings);
        pipelineObj = m_pipelineCache->GetRenderPipelineState(fetchShader, vertexShader, geometryShader, pixelShader, m_state.m_lastUsedFBO.m_attachmentsInfo, m_state.m_activeFBO.m_attachmentsInfo, m_state.m_activeFBO.m_fbo->m_size, count, LatteGPUState.contextNew);
    }
    if (!pipelineObj->m_pipeline)
        return;

    // Diagnostic-only ICB-batching premise probe (see m_icbProbePrevPipeline). Behavior-neutral: this only
    // counts how long the runs of consecutive same-pipeline draws are within one continuous pass; it submits
    // nothing. A run continues when this draw's pipeline equals the previous draw's AND they are in the same
    // draw-pass generation (a pass boundary, shader/bind/context change all bump m_drawPassGeneration and so
    // legitimately end a batchable run). This is the empirical test of whether "consecutive compatible draws"
    // exist to batch at all - if the longest run stays ~1 in busy passes, ICB draw batching cannot pay off.
    if (m_icbProbePrevPipeline == pipelineObj->m_pipeline && m_icbProbePrevGeneration == m_drawPassGeneration)
    {
        m_performanceMonitor.m_drawPipelineRepeats++;
        m_icbProbeRunLen++;
    }
    else
    {
        m_icbProbeRunLen = 1;
    }
    if (m_icbProbeRunLen > m_performanceMonitor.m_drawLongestPipelineRun)
        m_performanceMonitor.m_drawLongestPipelineRun = m_icbProbeRunLen;
    m_icbProbePrevPipeline = pipelineObj->m_pipeline;
    m_icbProbePrevGeneration = m_drawPassGeneration;

    if (pipelineObj->m_pipeline != encoderState.m_renderPipelineState)
       {
        renderCommandEncoder->setRenderPipelineState(pipelineObj->m_pipeline);
          encoderState.m_renderPipelineState = pipelineObj->m_pipeline;
       }

    // Depth stencil state

    const bool hasDepthStencilAttachment = m_state.m_activeFBO.m_fbo->depthBuffer.texture != nullptr;
    MTL::DepthStencilState* depthStencilState = m_depthStencilCache->GetDepthStencilState(LatteGPUState.contextNew, hasDepthStencilAttachment);
    if (depthStencilState != encoderState.m_depthStencilState)
    {
        renderCommandEncoder->setDepthStencilState(depthStencilState);
        encoderState.m_depthStencilState = depthStencilState;
    }

    // Stencil reference
    bool stencilEnable = hasDepthStencilAttachment && LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
    if (stencilEnable)
    {
        bool backStencilEnable = LatteGPUState.contextNew.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();
        uint32 stencilRefFront = LatteGPUState.contextNew.DB_STENCILREFMASK.get_STENCILREF_F();
        uint32 stencilRefBack;
        if (backStencilEnable)
            stencilRefBack = LatteGPUState.contextNew.DB_STENCILREFMASK_BF.get_STENCILREF_B();
        else
            stencilRefBack = stencilRefFront;

        if (stencilRefFront != encoderState.m_stencilRefFront || stencilRefBack != encoderState.m_stencilRefBack)
        {
            renderCommandEncoder->setStencilReferenceValues(stencilRefFront, stencilRefBack);

            encoderState.m_stencilRefFront = stencilRefFront;
            encoderState.m_stencilRefBack = stencilRefBack;
        }
    }

    // Blend color
    uint32* blendColorConstantU32 = LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;

    if (blendColorConstantU32[0] != encoderState.m_blendColor[0] || blendColorConstantU32[1] != encoderState.m_blendColor[1] || blendColorConstantU32[2] != encoderState.m_blendColor[2] || blendColorConstantU32[3] != encoderState.m_blendColor[3])
    {
        float* blendColorConstant = (float*)LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED;
        renderCommandEncoder->setBlendColor(blendColorConstant[0], blendColorConstant[1], blendColorConstant[2], blendColorConstant[3]);

        encoderState.m_blendColor[0] = blendColorConstantU32[0];
        encoderState.m_blendColor[1] = blendColorConstantU32[1];
        encoderState.m_blendColor[2] = blendColorConstantU32[2];
        encoderState.m_blendColor[3] = blendColorConstantU32[3];
    }

    // polygon control
    const auto& polygonControlReg = LatteGPUState.contextNew.PA_SU_SC_MODE_CNTL;
    const auto frontFace = polygonControlReg.get_FRONT_FACE();
    uint32 cullFront = polygonControlReg.get_CULL_FRONT();
    uint32 cullBack = polygonControlReg.get_CULL_BACK();
    uint32 polyOffsetFrontEnable = polygonControlReg.get_OFFSET_FRONT_ENABLED();

    if (polyOffsetFrontEnable)
    {
        uint32 frontScaleU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue();
        uint32 frontOffsetU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue();
        uint32 offsetClampU32 = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.getRawValue();

        if (frontOffsetU32 != encoderState.m_depthBias || frontScaleU32 != encoderState.m_depthSlope || offsetClampU32 != encoderState.m_depthClamp)
        {
               float frontScale = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE();
               float frontOffset = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET();
               float offsetClamp = LatteGPUState.contextNew.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP();

               frontScale /= 16.0f;

            renderCommandEncoder->setDepthBias(frontOffset, frontScale, offsetClamp);

            encoderState.m_depthBias = frontOffsetU32;
            encoderState.m_depthSlope = frontScaleU32;
            encoderState.m_depthClamp = offsetClampU32;
        }
    }
    else
    {
        if (0 != encoderState.m_depthBias || 0 != encoderState.m_depthSlope || 0 != encoderState.m_depthClamp)
        {
            renderCommandEncoder->setDepthBias(0.0f, 0.0f, 0.0f);

            encoderState.m_depthBias = 0;
            encoderState.m_depthSlope = 0;
            encoderState.m_depthClamp = 0;
        }
    }

    // Depth clip mode
    cemu_assert_debug(LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_NEAR_DISABLE() == LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE()); // near or far clipping can be disabled individually
    bool zClipEnable = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_ZCLIP_FAR_DISABLE() == false;

    if (zClipEnable != encoderState.m_depthClipEnable)
    {
        renderCommandEncoder->setDepthClipMode(zClipEnable ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp);
        encoderState.m_depthClipEnable = zClipEnable;
    }

    // Visibility result mode
    // Deduplicated against the encoder state: while an occlusion query is active the offset
    // advances every draw (each draw gets its own pool slot, see m_currentIndex increment
    // after the draw), so this genuinely re-issues each draw. While no query is active the
    // value is always (Disabled, 0) - previously re-submitted on every single draw - so the
    // dedup skips that redundant encoder call for the vast majority of gameplay draws. The
    // cached default matches a fresh encoder, and ResetEncoderState() restores it, so a
    // skipped call never leaves a wrong mode/offset in effect.
    MTL::VisibilityResultMode visibilityMode;
    size_t visibilityOffset;
    if (m_occlusionQuery.m_active)
    {
        visibilityMode = MTL::VisibilityResultModeCounting;
        visibilityOffset = (m_occlusionQuery.m_currentBuffer * OCCLUSION_QUERY_POOL_SIZE + m_occlusionQuery.m_currentIndex) * sizeof(uint64);
    }
    else
    {
        visibilityMode = MTL::VisibilityResultModeDisabled;
        visibilityOffset = 0;
    }
    if (visibilityMode != encoderState.m_visibilityResultMode || visibilityOffset != encoderState.m_visibilityResultOffset)
    {
        renderCommandEncoder->setVisibilityResultMode(visibilityMode, visibilityOffset);
        encoderState.m_visibilityResultMode = visibilityMode;
        encoderState.m_visibilityResultOffset = visibilityOffset;
    }

    // todo - how does culling behave with rects?
    // right now we just assume that their winding is always CW
    if (primitiveMode == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
    {
        if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CW)
            cullFront = cullBack;
        else
            cullBack = cullFront;
    }

    // Cull mode

    // Cull front and back is handled by disabling rasterization
    if (!(cullFront && cullBack))
    {
        MTL::CullMode cullMode;
           if (cullFront)
              cullMode = MTL::CullModeFront;
           else if (cullBack)
              cullMode = MTL::CullModeBack;
           else
              cullMode = MTL::CullModeNone;

        if (cullMode != encoderState.m_cullMode)
           {
               renderCommandEncoder->setCullMode(cullMode);
              encoderState.m_cullMode = cullMode;
           }
    }

    // Front face
    MTL::Winding frontFaceWinding;
    if (frontFace == Latte::LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW)
        frontFaceWinding = MTL::WindingCounterClockwise;
    else
        frontFaceWinding = MTL::WindingClockwise;

    if (frontFaceWinding != encoderState.m_frontFaceWinding)
       {
           renderCommandEncoder->setFrontFacingWinding(frontFaceWinding);
          encoderState.m_frontFaceWinding = frontFaceWinding;
       }

    // Viewport
    if (m_state.m_viewport.originX != encoderState.m_viewport.originX ||
        m_state.m_viewport.originY != encoderState.m_viewport.originY ||
        m_state.m_viewport.width != encoderState.m_viewport.width ||
        m_state.m_viewport.height != encoderState.m_viewport.height ||
        m_state.m_viewport.znear != encoderState.m_viewport.znear ||
        m_state.m_viewport.zfar != encoderState.m_viewport.zfar)
    {
        renderCommandEncoder->setViewport(m_state.m_viewport);

        encoderState.m_viewport = m_state.m_viewport;
    }

    // Scissor
    if (m_state.m_scissor.x != encoderState.m_scissor.x ||
        m_state.m_scissor.y != encoderState.m_scissor.y ||
        m_state.m_scissor.width != encoderState.m_scissor.width ||
        m_state.m_scissor.height != encoderState.m_scissor.height)
    {
        encoderState.m_scissor = m_state.m_scissor;

        // TODO: clamp scissor to render target dimensions?
        //scissor.width = ;
        //scissor.height = ;
        renderCommandEncoder->setScissorRect(encoderState.m_scissor);
    }

    // Resources

    if (!fetchVertexManually || vertexShader->resourceMapping.argumentBufferBindingPoint < 0)
    {
        for (uint8 i = 0; i < MAX_MTL_VERTEX_BUFFERS; i++)
        {
            MTL::Buffer* buffer = m_state.m_vertexBuffers[i];
            size_t offset = m_state.m_vertexBufferOffsets[i];
            if (buffer && offset != INVALID_OFFSET)
            {
                SetBuffer(renderCommandEncoder, GetMtlShaderType(vertexShader->shaderType, usesGeometryShader), buffer, offset, GET_MTL_VERTEX_BUFFER_INDEX(i));
            }
        }
    }

    // Prepare streamout
    const uint32 streamoutVertexCount = usesVertexStreamout && hostIndexType != INDEX_TYPE::NONE ? hostIndexCount : count;
    m_state.m_streamoutState.verticesPerInstance = streamoutVertexCount;
    LatteStreamout_PrepareDrawcall(streamoutVertexCount, instanceCount);

    // Uniform buffers, textures and samplers
    bool bindStageOk;
    {
        ScopedStageTimer _stBind(performanceMonitor.cpuTime_dcBindStage, m_captureCpuStageTimings);
        // Preserves the original short-circuit semantics exactly: fail if VS fails, or (when a geometry
        // stage applies) GS fails, or PS fails - and calls each stage in the same order / only when reached.
        bindStageOk = BindStageResources(renderCommandEncoder, vertexShader, usesGeometryShader) &&
                      (!(usesGeometryShader && geometryShader) || BindStageResources(renderCommandEncoder, geometryShader, usesGeometryShader)) &&
                      BindStageResources(renderCommandEncoder, pixelShader, usesGeometryShader);
    }
    if (!bindStageOk)
    {
        streamout_rendererFinishDrawcall();
        LatteGPUState.drawCallCounter++;
        return;
    }

    for (const auto& group : fetchShader->bufferGroups)
    {
        const uint32 i = group.attributeBufferIndex;
        if (i < MAX_MTL_VERTEX_BUFFERS)
            m_memoryManager->TrackSharedCache(m_state.m_vertexBuffers[i], m_state.m_vertexBufferOffsets[i], m_state.m_vertexBufferSizes[i]);
    }

    // Draw
    if (m_captureCpuStageTimings) performanceMonitor.cpuTime_dcDrawEmit.beginMeasuring();
    if (usesGeometryShader)
    {
        if (hostIndexType != INDEX_TYPE::NONE && vertexShader->resourceMapping.argumentBufferBindingPoint < 0)
            SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_OBJECT, indexAllocationMtl->mtlBuffer, indexAllocationMtl->bufferOffset, vertexShader->resourceMapping.indexBufferBinding);

        uint8 hostIndexTypeU8 = (uint8)hostIndexType;
        if (vertexShader->resourceMapping.argumentBufferBindingPoint < 0 &&
            vertexShader->resourceMapping.indexTypeBinding < MAX_MTL_BUFFERS)
        {
            renderCommandEncoder->setObjectBytes(&hostIndexTypeU8, sizeof(hostIndexTypeU8), vertexShader->resourceMapping.indexTypeBinding);
            encoderState.m_buffers[METAL_SHADER_TYPE_OBJECT][vertexShader->resourceMapping.indexTypeBinding] = {nullptr};
        }
        else if (vertexShader->resourceMapping.argumentBufferBindingPoint < 0)
        {
            cemuLog_logOnce(LogType::Force, "invalid Metal index type binding {}", (uint32)vertexShader->resourceMapping.indexTypeBinding);
        }

        uint32 verticesPerPrimitive = GetVerticesPerPrimitive(primitiveMode);
        uint64 primitivesPerInstance = 0;
        if (verticesPerPrimitive != 0)
        {
            if (PrimitiveRequiresConnection(primitiveMode))
            {
                if (count >= verticesPerPrimitive)
                    primitivesPerInstance = (uint64)count - verticesPerPrimitive + 1;
            }
            else
            {
                primitivesPerInstance = count / verticesPerPrimitive;
            }
        }
        else
        {
            cemuLog_logOnce(LogType::Force, "invalid Metal mesh primitive mode {}", (uint32)primitiveMode);
        }

        uint64 threadgroupCount = primitivesPerInstance * instanceCount;
        if (threadgroupCount > 0)
            renderCommandEncoder->drawMeshThreadgroups(MTL::Size(threadgroupCount, 1, 1), MTL::Size(verticesPerPrimitive, 1, 1), MTL::Size(1, 1, 1));
    }
    else if (usesVertexStreamout)
    {
        renderCommandEncoder->drawPrimitives(mtlPrimitiveType, 0, streamoutVertexCount, instanceCount, 0);
    }
    else
    {
        if (hostIndexType != INDEX_TYPE::NONE)
           {
               auto mtlIndexType = GetMtlIndexType(hostIndexType);
            renderCommandEncoder->drawIndexedPrimitives(mtlPrimitiveType, hostIndexCount, mtlIndexType, indexAllocationMtl->mtlBuffer, indexAllocationMtl->bufferOffset, instanceCount, static_cast<NS::Integer>(signedBaseVertex), baseInstance);
           }
           else
           {
              renderCommandEncoder->drawPrimitives(mtlPrimitiveType, baseVertex, count, instanceCount, baseInstance);
           }
    }
    if (m_captureCpuStageTimings) performanceMonitor.cpuTime_dcDrawEmit.endMeasuring();

    m_state.m_isFirstDrawInRenderPass = false;

    // Occlusion queries
    if (m_occlusionQuery.m_active)
        ++m_occlusionQuery.m_currentIndex;

    // Streamout
    LatteStreamout_FinishDrawcall(m_memoryManager->UseHostMemoryForCache());

    // Debug
    if (fetchVertexManually)
        m_performanceMonitor.m_manualVertexFetchDraws++;
    if (usesGeometryShader)
        m_performanceMonitor.m_meshDraws++;
    if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN)
        m_performanceMonitor.m_triangleFans++;

    LatteGPUState.drawCallCounter++;
}

void MetalRenderer::draw_endSequence()
{
    LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
    // post-drawcall logic
    if (pixelShader)
        LatteRenderTarget_trackUpdates();
    bool hasReadback = LatteTextureReadback_Update();
    m_recordedDrawcalls++;
    // The number of draw calls needs to twice as big, since we are interrupting the render pass
    // TODO: ucomment?
    if (m_recordedDrawcalls >= m_commitTreshold * 2/* || hasReadback*/)
    {
        CommitCommandBuffer();

        // TODO: where should this be called?
        LatteTextureReadback_UpdateFinishedTransfers(false);
    }
}

void MetalRenderer::draw_updateVertexBuffersDirectAccess(uint32 minIndex, uint32 maxIndex, uint32 baseInstance, uint32 instanceCount, bool fetchVertexManually)
{
    LatteFetchShader* parsedFetchShader = LatteSHRC_GetActiveFetchShader();
    if (!parsedFetchShader)
        return;

    for (auto& bufferGroup : parsedFetchShader->bufferGroups)
    {
        uint32 bufferIndex = bufferGroup.attributeBufferIndex;
        uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
        MPTR bufferAddress = LatteGPUState.contextRegister[bufferBaseRegisterIndex + 0];
        uint32 bufferStride = (LatteGPUState.contextRegister[bufferBaseRegisterIndex + 2] >> 11) & 0xFFFF;

        if (bufferAddress == MPTR_NULL) [[unlikely]]
            bufferAddress = m_memoryManager->GetImportedMemBaseAddress();

        uint32 bufferSize = 0;
        if (bufferGroup.hasVtxIndexAccess)
            bufferSize = bufferStride * (maxIndex + 1) + bufferGroup.maxOffset;
        if (bufferGroup.hasInstanceIndexAccess)
        {
            uint32 instanceBufferSize = bufferStride * ((baseInstance + instanceCount) + 1) + bufferGroup.maxOffset;
            bufferSize = std::max(bufferSize, instanceBufferSize);
        }
        if (bufferSize == 0 || bufferStride == 0)
            bufferSize += 128;

        if (m_memoryManager->IsRangeImported(bufferAddress, bufferSize))
        {
            uint32 firstByte = 0;
            if (!fetchVertexManually && bufferGroup.hasVtxIndexAccess && !bufferGroup.hasInstanceIndexAccess)
                firstByte = static_cast<uint32>(std::min<uint64>((uint64)bufferStride * minIndex, bufferSize));
            if (LatteBufferCache_hostIsRangeVolatile(bufferAddress + firstByte, bufferSize - firstByte))
            {
                auto* allocation = m_memoryManager->GetCachedSnapshot(MetalMemoryManager::VertexSnapshotBase + bufferIndex,
                    memory_getPointerFromVirtualOffset(bufferAddress), bufferSize, firstByte);
                
                m_state.m_vertexBuffers[bufferIndex] = allocation->mtlBuffer;
                m_state.m_vertexBufferOffsets[bufferIndex] = allocation->bufferOffset;
                m_state.m_vertexBufferSizes[bufferIndex] = bufferSize;
            }
            else
            {
                size_t bufferOffset = m_memoryManager->GetImportedMemoryOffset(bufferAddress);
                m_state.m_vertexBuffers[bufferIndex] = m_memoryManager->GetImportedMemoryBuffer();
                m_state.m_vertexBufferOffsets[bufferIndex] = bufferOffset;
                m_state.m_vertexBufferSizes[bufferIndex] = m_state.m_vertexBuffers[bufferIndex] && bufferOffset < m_state.m_vertexBuffers[bufferIndex]->length() ? std::min<size_t>(bufferSize, m_state.m_vertexBuffers[bufferIndex]->length() - bufferOffset) : 0;
                m_memoryManager->NotifyImportedMemoryRangeModified(bufferOffset, bufferSize);
            }
        }
        else
        {
            uint32 bindOffset = LatteBufferCache_retrieveDataInCache(bufferAddress, bufferSize);
            m_state.m_vertexBuffers[bufferIndex] = m_memoryManager->GetBufferCache();
            m_state.m_vertexBufferOffsets[bufferIndex] = bindOffset;
            m_state.m_vertexBufferSizes[bufferIndex] = m_state.m_vertexBuffers[bufferIndex] && bindOffset < m_state.m_vertexBuffers[bufferIndex]->length() ? std::min<size_t>(bufferSize, m_state.m_vertexBuffers[bufferIndex]->length() - bindOffset) : 0;
        }
    }
}

void MetalRenderer::draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset)
{
    if (shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
    {
        for (const auto& buf : shader->list_quickBufferList)
        {
            sint32 i = buf.index;
            MPTR physicalAddr = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 0];
            uint32 uniformSize = LatteGPUState.contextRegister[uniformBufferRegOffset + i * 7 + 1] + 1;

            if (physicalAddr == MPTR_NULL) [[unlikely]]
            {
                cemu_assert_unimplemented();
                MetalGeneralShaderType shaderType = GetMtlGeneralShaderType(shader->shaderType);
                m_state.m_uniformBuffers[shaderType][i] = nullptr;
                m_state.m_uniformBufferOffsets[shaderType][i] = INVALID_OFFSET;
                m_state.m_uniformBufferSizes[shaderType][i] = 0;
                continue;
            }
            uniformSize = std::min<uint32>(uniformSize, buf.size);

            cemu_assert_debug(physicalAddr < 0x50000000);

            uint32 bufferIndex = i;
            cemu_assert_debug(bufferIndex < 16);

            MetalGeneralShaderType shaderType = GetMtlGeneralShaderType(shader->shaderType);
            if (m_memoryManager->IsRangeImported(physicalAddr, uniformSize))
            {
                auto* allocation = m_memoryManager->GetCachedSnapshot(MetalMemoryManager::UniformSnapshotBase + shaderType * MAX_MTL_BUFFERS + bufferIndex,
                    memory_getPointerFromVirtualOffset(physicalAddr), uniformSize);

                m_state.m_uniformBuffers[shaderType][bufferIndex] = allocation->mtlBuffer;
                m_state.m_uniformBufferOffsets[shaderType][bufferIndex] = allocation->bufferOffset;
                m_state.m_uniformBufferSizes[shaderType][bufferIndex] = uniformSize;
            }
            else
            {
                uint32 bindOffset = LatteBufferCache_retrieveDataInCache(physicalAddr, uniformSize);
                m_state.m_uniformBuffers[shaderType][bufferIndex] = m_memoryManager->GetBufferCache();
                m_state.m_uniformBufferOffsets[shaderType][bufferIndex] = bindOffset;
                m_state.m_uniformBufferSizes[shaderType][bufferIndex] = uniformSize;
            }
        }
    }
}

void MetalRenderer::draw_handleSpecialState5()
{
    LatteMRT::UpdateCurrentFBO();
    LatteRenderTarget_updateViewport();

    LatteTextureView* colorBuffer = LatteMRT::GetColorAttachment(0);
    LatteTextureView* depthBuffer = LatteMRT::GetDepthAttachment();
    sint32 vpWidth, vpHeight;
    LatteMRT::GetVirtualViewportDimensions(vpWidth, vpHeight);

    surfaceCopy_copySurfaceWithFormatConversion(
        depthBuffer->baseTexture, depthBuffer->firstMip, depthBuffer->firstSlice,
        colorBuffer->baseTexture, colorBuffer->firstMip, colorBuffer->firstSlice,
        vpWidth, vpHeight);
}

Renderer::IndexAllocation MetalRenderer::indexData_reserveIndexMemory(uint32 size)
{
    auto allocation = m_memoryManager->GetIndexAllocator().AllocateBufferMemory(size, 128);

    return {allocation->memPtr, allocation};
}

void MetalRenderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
    m_memoryManager->GetIndexAllocator().FreeReservation(static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(allocation.rendererInternal));
}

void MetalRenderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
    m_memoryManager->GetIndexAllocator().FlushReservation(static_cast<MetalSynchronizedHeapAllocator::AllocatorReservation*>(allocation.rendererInternal));
}

LatteQueryObject* MetalRenderer::occlusionQuery_create() {
    auto* query = new LatteQueryObjectMtl(this);
    m_occlusionQuery.m_queries.push_back(query);
    return query;
}

void MetalRenderer::occlusionQuery_destroy(LatteQueryObject* queryObj) {
    auto queryObjMtl = static_cast<LatteQueryObjectMtl*>(queryObj);
    std::erase(m_occlusionQuery.m_queries, queryObjMtl);
    delete queryObjMtl;
}

void MetalRenderer::PrepareOcclusionQueryDraw()
{
    if (!m_occlusionQuery.m_active || m_occlusionQuery.m_currentIndex < OCCLUSION_QUERY_POOL_SIZE)
        return;

    const uint32 previousBuffer = m_occlusionQuery.m_currentBuffer;
    m_occlusionQuery.m_bufferCompletion[previousBuffer] = GetCommandBuffer()->retain();
    CommitCommandBuffer();
    for (auto* query : m_occlusionQuery.m_queries)
        query->SealCurrentRange(previousBuffer);

    const uint32 nextBuffer = (previousBuffer + 1) % OCCLUSION_QUERY_BUFFER_COUNT;
    auto*& completion = m_occlusionQuery.m_bufferCompletion[nextBuffer];
    if (completion)
    {

        if (!CommandBufferCompleted(completion))
        {
            performanceMonitor.gpuTime_occlusionTime.beginMeasuring(); // diagnostic-only (behavior-neutral)
            completion->waitUntilCompleted();
            performanceMonitor.gpuTime_occlusionTime.endMeasuring();
        }

        for (auto* query : m_occlusionQuery.m_queries)
            query->AccumulateBuffer(nextBuffer);
        
        completion->release();
        completion = nullptr;
    }
    m_occlusionQuery.m_currentBuffer = nextBuffer;
    std::fill_n(GetOcclusionQueryResultsPtr(), OCCLUSION_QUERY_POOL_SIZE, uint64{0});
    m_occlusionQuery.m_currentIndex = 0;
}

void MetalRenderer::occlusionQuery_flush() {
    CommitCommandBuffer();
    if (m_occlusionQuery.m_lastCommandBuffer)
    {
        performanceMonitor.gpuTime_occlusionTime.beginMeasuring(); // diagnostic-only (behavior-neutral)
        m_occlusionQuery.m_lastCommandBuffer->waitUntilCompleted();
        performanceMonitor.gpuTime_occlusionTime.endMeasuring();
    }
}

void MetalRenderer::occlusionQuery_updateState() {
    ProcessFinishedCommandBuffers();
}

void MetalRenderer::SetBuffer(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Buffer* buffer, size_t offset, uint32 index)
{
    if (index >= MAX_MTL_BUFFERS)
    {
        cemuLog_logOnce(LogType::Force, "invalid Metal buffer binding {}", index);
        return;
    }
    
    auto& boundBuffer = m_state.m_encoderState.m_buffers[shaderType][index];
    if (buffer == boundBuffer.m_buffer && offset == boundBuffer.m_offset)
        return;

    if (buffer == boundBuffer.m_buffer)
    {
        // Update just the offset
        boundBuffer.m_offset = offset;

        switch (shaderType)
        {
        case METAL_SHADER_TYPE_VERTEX:
            renderCommandEncoder->setVertexBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_OBJECT:
            renderCommandEncoder->setObjectBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_MESH:
            renderCommandEncoder->setMeshBufferOffset(offset, index);
            break;
        case METAL_SHADER_TYPE_FRAGMENT:
            renderCommandEncoder->setFragmentBufferOffset(offset, index);
            break;
        }

        return;
    }

    boundBuffer = {buffer, offset};

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshBuffer(buffer, offset, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentBuffer(buffer, offset, index);
        break;
    }
}

void MetalRenderer::SetTexture(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::Texture* texture, uint32 index)
{
    if (index >= MAX_MTL_TEXTURES)
    {
        cemuLog_logOnce(LogType::Force, "invalid Metal texture binding {}", index);
        return;
    }
    
    auto& boundTexture = m_state.m_encoderState.m_textures[shaderType][index];
    if (texture == boundTexture)
        return;

    boundTexture = texture;

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshTexture(texture, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentTexture(texture, index);
        break;
    }
}

void MetalRenderer::SetSamplerState(MTL::RenderCommandEncoder* renderCommandEncoder, MetalShaderType shaderType, MTL::SamplerState* samplerState, uint32 index)
{
    if (index >= MAX_MTL_SAMPLERS)
    {
        cemuLog_logOnce(LogType::Force, "invalid Metal sampler binding {}", index);
        return;
    }
    
    auto& boundSamplerState = m_state.m_encoderState.m_samplers[shaderType][index];
    if (samplerState == boundSamplerState)
        return;

    boundSamplerState = samplerState;

    switch (shaderType)
    {
    case METAL_SHADER_TYPE_VERTEX:
        renderCommandEncoder->setVertexSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_OBJECT:
        renderCommandEncoder->setObjectSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_MESH:
        renderCommandEncoder->setMeshSamplerState(samplerState, index);
        break;
    case METAL_SHADER_TYPE_FRAGMENT:
        renderCommandEncoder->setFragmentSamplerState(samplerState, index);
        break;
    }
}

MTL::CommandBuffer* MetalRenderer::GetCommandBuffer()
{
    bool needsNewCommandBuffer = (!m_currentCommandBuffer.m_commandBuffer || m_currentCommandBuffer.m_commited);
    if (needsNewCommandBuffer)
    {
        // Debug
        //m_commandQueue->insertDebugCaptureBoundary();

        auto pool = NS::AutoreleasePool::alloc()->init();
        MTL::CommandBuffer* mtlCommandBuffer = m_commandQueue->commandBuffer()->retain();
        pool->release();
        m_currentCommandBuffer = {mtlCommandBuffer};

        // Wait for the previous command buffer
        if (m_eventValue != -1)
            mtlCommandBuffer->encodeWait(m_event, m_eventValue);

        m_recordedDrawcalls = 0;
        m_commitTreshold = m_defaultCommitTreshlod;
        // Experimental Adaptive Commit Cadence (experimental_extended_commit_threshold). Replaces the
        // old static "2x" batching threshold, which caused the reported 30->24->27 FPS saw-tooth: a
        // fixed, too-high commit threshold delayed the GPU start of each frame and let the CPU race
        // ahead until it stalled on drawable-pool exhaustion, then recovered, then repeated. Instead we
        // read a threshold the controller maintains within [baseline, 2x baseline] using in-flight
        // command-buffer queue depth as an AIMD signal (see UpdateAdaptiveCommitThreshold): additive
        // increase while the GPU keeps up, multiplicative decrease the moment it falls behind — the
        // classic converging (non-oscillating) congestion-control shape. RequestSoonCommit() still
        // forces a prompt commit for readback / occlusion-query ordering (it overrides m_commitTreshold
        // directly to m_recordedDrawcalls+8), and explicit commits (present/flush) call
        // CommitCommandBuffer() unconditionally. Per-draw output is byte-identical; only submission
        // cadence changes. When OFF this is one atomic bool read per command-buffer creation and the
        // static baseline threshold is used (today's exact behavior).
        if (ActiveSettings::ExperimentalExtendedCommitThreshold())
            m_commitTreshold = m_adaptiveCommitThreshold;

        // Debug
        m_performanceMonitor.m_commandBuffers++;

        return mtlCommandBuffer;
    }
    else
    {
        return m_currentCommandBuffer.m_commandBuffer;
    }
}

MTL::RenderCommandEncoder* MetalRenderer::GetTemporaryRenderCommandEncoder(MTL::RenderPassDescriptor* renderPassDescriptor)
{
    // M5: a temporary render pass is a GPU op; materialize any deferred clears first so
    // guest order is preserved (this encoder is never a fold target).
    FlushPendingClears();

    EndEncoding();

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto renderCommandEncoder = commandBuffer->renderCommandEncoder(renderPassDescriptor)->retain();
    pool->release();
#ifdef CEMU_DEBUG_ASSERT
    renderCommandEncoder->setLabel(GetLabel("Temporary render command encoder", renderCommandEncoder));
#endif
    m_commandEncoder = renderCommandEncoder;
    m_encoderType = MetalEncoderType::Render;

    // A temporary render encoder replaces the live encoder without going through ResetEncoderState, so bump the
    // epoch here too (experimental_binding_dirty_masks): a later draw must never skip its texture/sampler binds
    // believing they are still live on the previous encoder. Monotonic; only read when that toggle is ON.
    m_encoderEpoch++;

    // Debug
    m_performanceMonitor.m_renderPasses++;

    return renderCommandEncoder;
}

// Some render passes clear the attachments, forceRecreate is supposed to be used in those cases
MTL::RenderCommandEncoder* MetalRenderer::GetRenderCommandEncoder(bool forceRecreate)
{
    bool fboChanged = m_state.m_fboChanged;
    m_state.m_fboChanged = false;

    // Check if we need to begin a new render pass
    if (m_commandEncoder)
    {
        if (!forceRecreate)
        {
            if (m_encoderType == MetalEncoderType::Render)
            {
                bool needsNewRenderPass = false;
                if (fboChanged)
                {
                    needsNewRenderPass = (m_state.m_lastUsedFBO.m_fbo == nullptr);
                    if (!needsNewRenderPass)
                    {
                        for (uint8 i = 0; i < 8; i++)
                        {
                            if (m_state.m_activeFBO.m_fbo->colorBuffer[i].texture && m_state.m_activeFBO.m_fbo->colorBuffer[i].texture != m_state.m_lastUsedFBO.m_fbo->colorBuffer[i].texture)
                            {
                                needsNewRenderPass = true;
                                break;
                            }
                        }
                    }

                    if (!needsNewRenderPass)
                    {
                        if (m_state.m_activeFBO.m_fbo->depthBuffer.texture && (m_state.m_activeFBO.m_fbo->depthBuffer.texture != m_state.m_lastUsedFBO.m_fbo->depthBuffer.texture || ( m_state.m_activeFBO.m_fbo->depthBuffer.hasStencil && !m_state.m_lastUsedFBO.m_fbo->depthBuffer.hasStencil)))
                        {
                            needsNewRenderPass = true;
                        }
                    }
                }

                if (!needsNewRenderPass)
                {
                    return (MTL::RenderCommandEncoder*)m_commandEncoder;
                }
            }
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    // M5 (Partial Rendering): fold any deferred guest clears into this render pass'
    // load actions (TBDR: skips the standalone clear pass' store + this pass' load).
    // Reachable only on the new-encoder path: deferral ends any open encoder, so the
    // reuse fast path above is never taken while clears are pending. Records that
    // cannot be folded atomically fall back to a standalone immediate clear.
    std::vector<uint32> m5FoldedColor;
    bool m5FoldedDepth = false;
    bool m5FoldedStencil = false;
    if (!m_pendingClears.empty())
    {
        std::vector<PendingClear> pending;
        pending.swap(m_pendingClears);
        auto* fbo = m_state.m_activeFBO.m_fbo;
        auto* desc = fbo->GetRenderPassDescriptor();
        for (const auto& rec : pending)
        {
            bool folded = false;
            if (!rec.isDepth)
            {
                for (uint32 i = 0; i < 8; i++)
                {
                    LatteTextureView* view = fbo->colorBuffer[i].texture;
                    if (view && view->baseTexture == rec.latteTexture &&
                        view->firstMip == rec.mipIndex && view->numMip == 1 &&
                        view->firstSlice == rec.sliceIndex && view->numSlice == 1 &&
                        view->dim == Latte::E_DIM::DIM_2D)
                    {
                        auto att = desc->colorAttachments()->object(i);
                        att->setClearColor(MTL::ClearColor(rec.r, rec.g, rec.b, rec.a));
                        att->setLoadAction(MTL::LoadActionClear);
                        m5FoldedColor.push_back(i);
                        folded = true;
                        break;
                    }
                }
            }
            // __M5_DEPTH_FOLD__
            else
            {
                LatteTextureView* dview = fbo->depthBuffer.texture;
                bool depthMatches = dview && dview->baseTexture == rec.latteTexture &&
                    dview->firstMip == rec.mipIndex && dview->numMip == 1 &&
                    dview->firstSlice == rec.sliceIndex && dview->numSlice == 1 &&
                    dview->dim == Latte::E_DIM::DIM_2D;
                // The descriptor only carries a stencil attachment when the FBO's depth
                // buffer actually has stencil; without one a stencil clear can't be folded.
                bool hasStencilAtt = (desc->stencilAttachment()->texture() != nullptr);
                // Fold the record only if it can be folded in full (atomic): depth
                // attachment present, and, if it clears stencil, a stencil attachment too.
                if (depthMatches && (!rec.clearStencil || hasStencilAtt))
                {
                    if (rec.clearDepth)
                    {
                        auto att = desc->depthAttachment();
                        att->setClearDepth(rec.depthValue);
                        att->setLoadAction(MTL::LoadActionClear);
                        m5FoldedDepth = true;
                    }
                    if (rec.clearStencil)
                    {
                        auto att = desc->stencilAttachment();
                        att->setClearStencil(rec.stencilValue);
                        att->setLoadAction(MTL::LoadActionClear);
                        m5FoldedStencil = true;
                    }
                    folded = true;
                }
            }
            if (!folded)
                EmitPendingClearNow(rec);
        }
    }

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto renderCommandEncoder = commandBuffer->renderCommandEncoder(m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor())->retain();
    pool->release();

    // M5: the descriptor's load-action state was captured at encoder creation above, so
    // restore the folded attachments to LoadActionLoad now (the descriptor is persistent
    // and shared across passes; a stale Clear would wrongly re-clear on the next pass).
    if (!m5FoldedColor.empty() || m5FoldedDepth || m5FoldedStencil)
    {
        auto* desc = m_state.m_activeFBO.m_fbo->GetRenderPassDescriptor();
        for (uint32 i : m5FoldedColor)
            desc->colorAttachments()->object(i)->setLoadAction(MTL::LoadActionLoad);
        if (m5FoldedDepth)
            desc->depthAttachment()->setLoadAction(MTL::LoadActionLoad);
        if (m5FoldedStencil)
            desc->stencilAttachment()->setLoadAction(MTL::LoadActionLoad);
    }
#ifdef CEMU_DEBUG_ASSERT
    renderCommandEncoder->setLabel(GetLabel("Render command encoder", renderCommandEncoder));
#endif
    m_commandEncoder = renderCommandEncoder;
    m_encoderType = MetalEncoderType::Render;

    // Update state
    m_state.m_lastUsedFBO = m_state.m_activeFBO;
    m_state.m_isFirstDrawInRenderPass = true;

    ResetEncoderState();

    // Debug
    m_performanceMonitor.m_renderPasses++;

    return renderCommandEncoder;
}

MTL::ComputeCommandEncoder* MetalRenderer::GetComputeCommandEncoder()
{
    // M5: compute is not a fold target; materialize deferred clears first (guest order).
    FlushPendingClears();

    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Compute)
        {
            return (MTL::ComputeCommandEncoder*)m_commandEncoder;
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto computeCommandEncoder = commandBuffer->computeCommandEncoder()->retain();
    pool->release();
    m_commandEncoder = computeCommandEncoder;
    m_encoderType = MetalEncoderType::Compute;

    ResetEncoderState();

    return computeCommandEncoder;
}

MTL::BlitCommandEncoder* MetalRenderer::GetBlitCommandEncoder()
{
    // M5: blit is not a fold target; materialize deferred clears first (guest order).
    FlushPendingClears();

    if (m_commandEncoder)
    {
        if (m_encoderType == MetalEncoderType::Blit)
        {
            return (MTL::BlitCommandEncoder*)m_commandEncoder;
        }

        EndEncoding();
    }

    auto commandBuffer = GetCommandBuffer();

    auto pool = NS::AutoreleasePool::alloc()->init();
    auto blitCommandEncoder = commandBuffer->blitCommandEncoder()->retain();
    pool->release();
    m_commandEncoder = blitCommandEncoder;
    m_encoderType = MetalEncoderType::Blit;

    ResetEncoderState();

    return blitCommandEncoder;
}

void MetalRenderer::EndEncoding()
{
    if (m_commandEncoder)
    {
        m_commandEncoder->endEncoding();
        m_commandEncoder->release();
        m_commandEncoder = nullptr;
        m_encoderType = MetalEncoderType::None;

        // The residency tracking for the "Skip Redundant GPU Residency" toggle is scoped to the
        // encoder we are tearing down here. This is the single funnel every encoder transition
        // passes through (m_commandEncoder is nulled only here, and every Get*CommandEncoder that
        // creates a new encoder calls EndEncoding first), so clearing here guarantees no residency
        // state from a previous encoder can ever be consulted against a different one. Harmless when
        // the toggle is OFF (the map is always empty in that case).
        m_residentResources.clear();

        // Commit the command buffer if enough draw calls have been recorded
        if (m_recordedDrawcalls >= m_commitTreshold)
            CommitCommandBuffer();
    }
}

void MetalRenderer::CommitCommandBuffer()
{
    if (!m_currentCommandBuffer.m_commandBuffer)
        return;

    // M5: defensively materialize any deferred clears before committing so they cannot
    // linger past a command-buffer boundary in a way that reorders them relative to this
    // buffer's other work. Safe against re-entry: FlushPendingClears swaps the list out
    // first, so the commit it may trigger internally sees an empty list.
    FlushPendingClears();

    EndEncoding();

    ProcessFinishedCommandBuffers();

    // Commit the command buffer
    if (!m_currentCommandBuffer.m_commited)
    {
        // Handled differently, since it seems like Metal doesn't always call the completion handler
        //commandBuffer.m_commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
        //    m_memoryManager->GetTemporaryBufferAllocator().CommandBufferFinished(commandBuffer.m_commandBuffer);
        //});

        // Signal event
        m_eventValue = (m_eventValue + 1) % EVENT_VALUE_WRAP;
        auto mtlCommandBuffer = m_currentCommandBuffer.m_commandBuffer;
        mtlCommandBuffer->encodeSignalEvent(m_event, m_eventValue);

        mtlCommandBuffer->commit();
        m_currentCommandBuffer.m_commited = true;

        m_executingCommandBuffers.push_back(mtlCommandBuffer);

        // Debug
        //m_commandQueue->insertDebugCaptureBoundary();
    }
}

void MetalRenderer::ProcessFinishedCommandBuffers()
{
    // Check for finished command buffers
    for (auto it = m_executingCommandBuffers.begin(); it != m_executingCommandBuffers.end();)
    {
        auto commandBuffer = *it;
        if (CommandBufferCompleted(commandBuffer))
        {
            // Diagnostic-only (behavior-neutral): accumulate real GPU execution time from the completed
            // command buffer's timestamps. No completion handler and no added synchronization are needed -
            // the CB is already Completed here. CBs are serialized on the GPU (each encodeWaits the prior
            // one's MTL::Event, see GetCommandBuffer), so summing per-CB (end-start) spans approximates GPU
            // wall-clock busy time for the frame window. Guard against unset/zero timestamps (e.g. Error).
            const double gpuStart = commandBuffer->GPUStartTime();
            const double gpuEnd = commandBuffer->GPUEndTime();
            if (gpuStart > 0.0 && gpuEnd > gpuStart)
            {
                m_performanceMonitor.m_gpuActiveAccumUs += (gpuEnd - gpuStart) * 1000000.0;
                m_performanceMonitor.m_gpuActiveAccumCBs++;
            }
            m_memoryManager->CleanupBuffers(commandBuffer);
            commandBuffer->release();
            it = m_executingCommandBuffers.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Adaptive Commit Cadence: fold the freshly-reaped in-flight queue depth into the batching-threshold
    // controller (consumed by GetCommandBuffer). Toggle-gated, so this is completely inert when OFF.
    if (ActiveSettings::ExperimentalExtendedCommitThreshold())
        UpdateAdaptiveCommitThreshold();
}

void MetalRenderer::UpdateAdaptiveCommitThreshold()
{
    // Adaptive Commit Cadence controller (experimental_extended_commit_threshold). Called after the
    // reap loop in ProcessFinishedCommandBuffers, so m_executingCommandBuffers.size() is the current
    // count of command buffers the CPU has committed but the GPU has not finished. Because each command
    // buffer GPU-waits on the previous one's event (see GetCommandBuffer), the GPU drains this list in
    // order, so the depth is a direct measure of how far the CPU has run ahead of the GPU:
    //   depth 0  -> GPU has caught up / is idle-waiting (we are CPU-bound): safe to batch a little more,
    //               which cuts per-command-buffer submit overhead on exactly the CPU-bound frames we
    //               care about. Additive increase.
    //   depth 1  -> healthy "one buffer ahead" pipeline: hold (hysteresis band).
    //   depth>=2 -> the CPU is pulling ahead of the GPU; this is the precursor to the fixed-threshold
    //               saw-tooth (race ahead -> drawable-pool exhaustion). Multiplicative decrease toward
    //               the baseline so command buffers commit promptly and the GPU restarts sooner.
    // Additive-increase / multiplicative-decrease is the classic congestion-control shape: it converges
    // to an operating point rather than sustaining the large oscillation a fixed high threshold caused.
    // The threshold is always clamped to [baseline, 2x baseline]; the read/write here are single-threaded
    // (render thread owns all command submission), so no atomics are needed. Signal only — never a
    // correctness gate.
    const uint32 baseline = m_defaultCommitTreshlod;
    const uint32 ceiling = m_defaultCommitTreshlod * 2;
    const size_t queueDepth = m_executingCommandBuffers.size();
    if (queueDepth >= 2)
    {
        // multiplicative decrease: halve the surplus over baseline, snapping back fast
        m_adaptiveCommitThreshold = baseline + (m_adaptiveCommitThreshold - baseline) / 2;
    }
    else if (queueDepth == 0)
    {
        // additive increase: nudge up while the GPU is not the bottleneck
        uint32 step = baseline / 8;
        if (step == 0)
            step = 1;
        m_adaptiveCommitThreshold += step;
    }
    // queueDepth == 1: hold.
    // clamp to [baseline, 2x baseline] (also repairs any pre-init value)
    if (m_adaptiveCommitThreshold < baseline)
        m_adaptiveCommitThreshold = baseline;
    else if (m_adaptiveCommitThreshold > ceiling)
        m_adaptiveCommitThreshold = ceiling;
}

bool MetalRenderer::AcquireDrawable(bool mainWindow)
{
    auto& layer = GetLayer(mainWindow);
    if (!layer.GetLayer())
        return false;

#if BOOST_OS_IOS
    const auto outputBit = mainWindow ? 1u : 2u;
    if (!layer.GetDrawable() && !(WindowSystem::GetWindowInfo().visible_outputs.load() & outputBit))
        return false;
#endif
    
    const bool latteBufferUsesSRGB = mainWindow ? LatteGPUState.tvBufferUsesSRGB : LatteGPUState.drcBufferUsesSRGB;
    const auto pixelFormat = latteBufferUsesSRGB ? MTL::PixelFormatBGRA8Unorm_sRGB : MTL::PixelFormatBGRA8Unorm;
    if (layer.GetLayer()->pixelFormat() != pixelFormat)
        layer.GetLayer()->setPixelFormat(pixelFormat);
    m_state.m_usesSRGB = latteBufferUsesSRGB;

    if (layer.AcquireDrawable())
        return true;

    // Drawable-exhaustion recovery (always on): nextDrawable() returned nil, meaning every drawable
    // in CAMetalLayer's small pool is still owned by an in-flight (presented) command buffer. If we
    // just keep returning false, the caller (BeginFrame/SwapBuffer) skips presenting forever - and
    // because presentation is what recycles drawables, the pool never refills, so the skip becomes
    // permanent. Combined with the guest parking in GX2WaitTimeStamp on the then-frozen retire
    // marker, that is the aggressive-frame-pacing freeze. Reap any command buffers that have since
    // completed (dropping our references to the drawables they presented, so CoreAnimation can
    // recycle them) and retry exactly once. ProcessFinishedCommandBuffers() only removes buffers
    // whose status() is Completed/Error, so no in-flight buffer is freed - this is safe. Still nil =>
    // skip just this frame and return to the loop (the sim-vsync clock keeps advancing); we try again
    // next frame instead of blocking.
    ProcessFinishedCommandBuffers();
    return layer.AcquireDrawable();
}

bool MetalRenderer::CheckIfRenderPassNeedsFlush(LatteDecompilerShader* shader)
{
    if (!shader)
        return false;

    sint32 textureCount = shader->resourceMapping.getTextureCount();
    for (int i = 0; i < textureCount; ++i)
    {
        const auto relative_textureUnit = shader->resourceMapping.getTextureUnitFromBindingPoint(i);
        auto hostTextureUnit = relative_textureUnit;
        auto textureDim = shader->textureUnitDim[relative_textureUnit];
        
        uint8 renderTargetIndex = shader->textureRenderTargetIndex[relative_textureUnit];
        if (m_supportsFramebufferFetch && renderTargetIndex != 255)
        {
            auto format = LatteMRT::GetColorBufferFormat(renderTargetIndex, LatteGPUState.contextNew);
            if (GetMtlPixelFormat(format, false) != MTL::PixelFormatInvalid)
                continue;
        }

        auto texUnitRegIndex = hostTextureUnit * 7;
        switch (shader->shaderType)
        {
        case LatteConst::ShaderType::Vertex:
            hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
            texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
            break;
        case LatteConst::ShaderType::Pixel:
            hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
            texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
            break;
        case LatteConst::ShaderType::Geometry:
            hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
            texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
            break;
        default:
            UNREACHABLE;
        }

        auto textureView = m_state.m_textures[hostTextureUnit];
        if (!textureView)
            continue;

        LatteTexture* baseTexture = textureView->baseTexture;

        // If the texture is also used in the current render pass, we need to end the render pass to "flush" the texture
        for (uint8 i = 0; i < LATTE_NUM_COLOR_TARGET; i++)
        {
            auto colorTarget = m_state.m_activeFBO.m_fbo->colorBuffer[i].texture;
            if (colorTarget && colorTarget->baseTexture == baseTexture)
                return true;
        }
    }

    return false;
}

void MetalRenderer::PrepareUniformBufferSizes(LatteDecompilerShader* shader)
{
    if (!shader)
        return;
    const auto stage = GetMtlGeneralShaderType(shader->shaderType);
    for (const auto& required : shader->list_quickBufferList)
    {
        const auto i = required.index;
        if (i >= LATTE_NUM_MAX_UNIFORM_BUFFERS || shader->resourceMapping.uniformBuffersBindingPoint[i] < 0)
            continue;
        auto*& buffer = m_state.m_uniformBuffers[stage][i];
        auto& offset = m_state.m_uniformBufferOffsets[stage][i];
        auto& size = m_state.m_uniformBufferSizes[stage][i];
        if (!buffer || offset >= buffer->length())
            continue;
        size = std::min(size, buffer->length() - offset);
        if (required.size <= size)
            continue;
        const bool gpuCopy = buffer->storageMode() == MTL::StorageModePrivate ||
            (buffer == m_memoryManager->GetBufferCache() && m_memoryManager->SharedCacheBusy(offset, size, true));
        if (gpuCopy)
            GetBlitCommandEncoder();
        else
            GetCommandBuffer();
        auto& allocator = m_memoryManager->GetStagingAllocator();
        auto allocation = allocator.AllocateBufferMemory(required.size, 16);
        std::memset(allocation.memPtr, 0, allocation.size);
        if (!gpuCopy)
            std::memcpy(allocation.memPtr, static_cast<uint8*>(buffer->contents()) + offset, size);
        allocator.FlushReservation(allocation);
        if (gpuCopy && size)
        {
            CopyBufferToBuffer(buffer, offset, allocation.mtlBuffer, allocation.bufferOffset, size, ALL_MTL_RENDER_STAGES, ALL_MTL_RENDER_STAGES);
            m_memoryManager->TrackSharedCache(buffer, offset, size);
        }
        buffer = allocation.mtlBuffer;
        offset = allocation.bufferOffset;
        size = required.size;
    }
}

void MetalRenderer::DeclareResidency(MTL::RenderCommandEncoder* enc, const MTL::Resource* resource, MTL::ResourceUsage usage, MTL::RenderStages stage)
{
    // Diagnostic-only: all DeclareResidency calls originate inside BindStageResources, so this timer is a
    // clean subset of cpuTime_dcBindStage. RAII at function scope times every return path. Zero cost off.
    ScopedStageTimer _stRes(performanceMonitor.cpuTime_dcResidency, m_captureCpuStageTimings);
    // OFF path (point (f)): behaviorally identical to the original call sites - a direct useResource
    // with the same resource pointer, usage, and stage. m_residentResources stays empty and is never
    // consulted, so with the toggle off this path matches the previous code exactly.
    if (!ActiveSettings::ExperimentalSkipRedundantResidency())
    {
        enc->useResource(resource, usage, stage);
        return;
    }

    // ON path. Pack usage and stage into disjoint 16-bit halves of a 32-bit key. Their raw bit ranges
    // overlap (ResourceUsage: Read=1/Write=2/Sample=4; RenderStages: Vertex=1/Fragment=2/Tile=4/
    // Object=8/Mesh=16), so a naive usage|stage would alias; keeping them in separate halves makes the
    // mask unambiguous (point (b)). If either enum ever carries a value we cannot represent in 16 bits,
    // fall back to an unconditional useResource rather than risk truncating the mask and skipping a
    // declaration we should not (fail-safe for points (b)/(d)).
    const uint32 usageBits = (uint32)usage;
    const uint32 stageBits = (uint32)stage;
    if (usageBits > 0xFFFF || stageBits > 0xFFFF)
    {
        enc->useResource(resource, usage, stage);
        return;
    }
    const uint32 need = usageBits | (stageBits << 16);

    // Key on the exact pointer that is handed to useResource just below, so the tracked identity can
    // never diverge from the object Metal is told about (point (a)).
    uint32& have = m_residentResources[(const void*)resource];
    if ((have & need) == need)
        return; // already resident on this encoder with a fully covering usage+stage mask -> safe to skip

    // Not fully covered. Because we only skip when the recorded mask already covers every requested
    // bit, we can never under-declare (point (d)): any new usage or stage bit forces a real useResource.
    // The map is cleared in EndEncoding(), so `have` only ever reflects the current encoder (points
    // (c)/(e)).
    have |= need;
    enc->useResource(resource, usage, stage);
}

bool MetalRenderer::BindStageResources(MTL::RenderCommandEncoder* renderCommandEncoder, LatteDecompilerShader* shader, bool usesGeometryShader)
{
    auto mtlShaderType = GetMtlShaderType(shader->shaderType, usesGeometryShader);
    auto* rendererShader = static_cast<RendererShaderMtl*>(shader->shader);
    MTL::ArgumentEncoder* argumentEncoder = nullptr;
    MetalArgumentBindings argumentBindings{};
    const bool shaderUsesArgumentBuffer = shader->resourceMapping.argumentBufferBindingPoint >= 0;
    if (shaderUsesArgumentBuffer)
    {
        argumentEncoder = rendererShader->GetArgumentEncoder();
        if (!argumentEncoder)
        {
            cemuLog_logOnce(LogType::Force, "Metal shader {:016x} has no argument encoder", shader->baseHash);
            return false;
        }
        
        const uint32 encodedLength = rendererShader->GetArgumentBufferEncodedLength();
        if (encodedLength == 0)
        {
            cemuLog_logOnce(LogType::Force, "Metal shader {:016x} has an empty argument-buffer layout", shader->baseHash);
            return false;
        }
        
        argumentBindings[MetalArgumentBuffer::Dummy] = {MetalArgumentBinding::Type::Constant, nullptr, 0};
    }
    else
    {
        // Experimental "Direct Shader Bindings": this eligible game shader has no argument buffer, so its
        // resources are bound straight to the render encoder in the branches below. Count it as proof the
        // conversion happened (stays 0 when the toggle is OFF, since every game shader then has an encoder).
        m_performanceMonitor.m_directBindingDraws++;
    }
    
    MTL::RenderStages renderStage = MTL::RenderStageVertex;
    switch (mtlShaderType)
    {
        case METAL_SHADER_TYPE_VERTEX:
            renderStage = MTL::RenderStageVertex;
            break;
        case METAL_SHADER_TYPE_OBJECT:
            renderStage = MTL::RenderStageObject;
            break;
        case METAL_SHADER_TYPE_MESH:
            renderStage = MTL::RenderStageMesh;
            break;
        case METAL_SHADER_TYPE_FRAGMENT:
            renderStage = MTL::RenderStageFragment;
            break;
        default:
            UNREACHABLE;
    }
    
    // Experimental per-draw-pass sampler fast-path toggle (sampled once per stage; the only OFF-path
    // cost is this atomic bool read). See MetalSamplerCache::GetPassSampler for the correctness proof.
    const bool samplerFastPathOn = ActiveSettings::ExperimentalSamplerCacheFastPath();
    // Experimental per-draw-pass texture-binding fast-path toggle (same sampling/OFF-path story).
    // See MetalTextureBindCache::GetPassTexture for the correctness proof.
    const bool textureFastPathOn = ActiveSettings::ExperimentalPassTextureFastPath();

    // Experimental "Skip Repeated Texture Binds" (experimental_binding_dirty_masks): a shader's textures and
    // samplers are frozen for the whole draw pass, and in the DIRECT binding ABI (no argument buffer) the loop
    // below only writes them straight to the render encoder. So for draws after the first in a pass we can skip
    // the entire resolve+bind loop, provided the same encoder is still live. The three-part key defeats both
    // hazards: m_drawPassGeneration (a bind/context/sampler/shader change ends the pass and bumps it) proves the
    // bindings are still valid, m_encoderEpoch (bumped on every ResetEncoderState) proves the encoder was not
    // recreated mid-pass by a command-buffer commit (which would wipe the binds), and the shader pointer proves
    // the same shader owns this stage. This is inert for argument-buffer shaders: their texture/sampler entries
    // must be re-written into argumentBindings for the whole-buffer encode every draw, so the loop must run.
    // OFF (or Direct Shader Bindings OFF) => bindLoopSkipEligible is false => the loop always runs (byte-identical).
    const bool bindLoopSkipEligible = !argumentEncoder && ActiveSettings::ExperimentalBindingDirtyMasks();
    bool runBindLoop = true;
    if (bindLoopSkipEligible &&
        m_bindLoopShader[mtlShaderType] == static_cast<const void*>(shader) &&
        m_bindLoopGeneration[mtlShaderType] == m_drawPassGeneration &&
        m_bindLoopEpoch[mtlShaderType] == m_encoderEpoch)
    {
        runBindLoop = false;
        m_performanceMonitor.m_bindLoopSkips++;
    }
    if (runBindLoop)
    {
    for (sint32 relative_textureUnit = 0; relative_textureUnit < LATTE_NUM_MAX_TEX_UNITS; relative_textureUnit++)
    {
        if (shader->resourceMapping.textureUnitToBindingPoint[relative_textureUnit] < 0)
            continue;
        
        auto hostTextureUnit = relative_textureUnit;
        
        uint8 renderTargetIndex = shader->textureRenderTargetIndex[relative_textureUnit];
        if (m_supportsFramebufferFetch && renderTargetIndex != 255)
        {
            auto format = LatteMRT::GetColorBufferFormat(renderTargetIndex, LatteGPUState.contextNew);
            if (GetMtlPixelFormat(format, false) != MTL::PixelFormatInvalid)
                continue;
        }
        
        auto textureDim = shader->textureUnitDim[relative_textureUnit];
        auto texUnitRegIndex = hostTextureUnit * 7;
        switch (shader->shaderType)
        {
            case LatteConst::ShaderType::Vertex:
                hostTextureUnit += LATTE_CEMU_VS_TEX_UNIT_BASE;
                texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
                break;
            case LatteConst::ShaderType::Pixel:
                hostTextureUnit += LATTE_CEMU_PS_TEX_UNIT_BASE;
                texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
                break;
            case LatteConst::ShaderType::Geometry:
                hostTextureUnit += LATTE_CEMU_GS_TEX_UNIT_BASE;
                texUnitRegIndex += Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS;
                break;
            default:
                UNREACHABLE;
        }
        
        uint32 binding = shader->resourceMapping.textureUnitToBindingPoint[relative_textureUnit];
        if (binding >= MAX_MTL_TEXTURES)
        {
            cemuLog_logOnce(LogType::Force, "invalid texture binding {}", binding);
            continue;
        }
        sint32 samplerBinding = shader->resourceMapping.textureUnitToSamplerBindingPoint[relative_textureUnit];
        
        auto textureView = m_state.m_textures[hostTextureUnit];
        MTL::SamplerState* sampler = m_nearestSampler;
        uint32 stageSamplerIndex = shader->textureUnitSamplerAssignment[relative_textureUnit];
        if (samplerBinding >= 0 && stageSamplerIndex != LATTE_DECOMPILER_SAMPLER_NONE)
        {
            // Experimental fast path: the sampler resolved for this (stage, unit) is frozen for the whole
            // draw pass, so reuse the pointer from a previous draw in the same pass and skip the aniso
            // mutation, the sampler hash and the map probe. Any generation mismatch (or toggle OFF) falls
            // through to the exact original resolve + re-cache below, so a wrong sampler can never bind.
            MTL::SamplerState* cachedSampler = samplerFastPathOn ? m_samplerCache->GetPassSampler(shader->shaderType, relative_textureUnit, m_drawPassGeneration) : nullptr;
            if (cachedSampler)
            {
                sampler = cachedSampler;
            }
            else
            {
                uint32 samplerIndex = stageSamplerIndex + LatteDecompiler_getTextureSamplerBaseIndex(shader->shaderType);
                _LatteRegisterSetSampler* samplerWords = LatteGPUState.contextNew.SQ_TEX_SAMPLER + samplerIndex;
                if (textureView && textureView->baseTexture->overwriteInfo.anisotropicLevel >= 0)
                    samplerWords->WORD0.set_MAX_ANISO_RATIO(textureView->baseTexture->overwriteInfo.anisotropicLevel);
                sampler = m_samplerCache->GetSamplerState(LatteGPUState.contextNew, shader->shaderType, stageSamplerIndex, samplerWords);
                if (samplerFastPathOn)
                    m_samplerCache->SetPassSampler(shader->shaderType, relative_textureUnit, m_drawPassGeneration, sampler);
            }
        }
        if (samplerBinding >= 0)
        {
            if (!sampler)
            {
                cemuLog_logOnce(LogType::Force, "Metal shader {:016x} could not allocate a sampler", shader->baseHash);
                return false;
            }
            if (argumentEncoder)
                argumentBindings[MetalArgumentBuffer::SamplerBase + samplerBinding] = {MetalArgumentBinding::Type::Sampler, sampler, 0};
            else
                SetSamplerState(renderCommandEncoder, mtlShaderType, sampler, samplerBinding);
        }
        
        // Experimental per-draw-pass texture-binding fast path: the MTL::Texture* resolved for this
        // (stage, unit) is frozen for the whole draw pass (a bind, context-reg or shader change ends
        // the pass and bumps m_drawPassGeneration), so reuse a pointer resolved by an earlier draw in
        // the same pass and skip the null-texture selection, the dimension checks and GetSwizzledView.
        // A generation mismatch (or the toggle OFF) falls through to the exact original resolve and
        // re-caches, so a wrong texture can never bind. Identical invariant/proof to the sampler fast
        // path above; only a non-null resolve is cached (a null re-resolves cheaply each draw).
        MTL::Texture* mtlTexture = textureFastPathOn ? m_textureBindCache->GetPassTexture(shader->shaderType, relative_textureUnit, m_drawPassGeneration) : nullptr;
        if (!mtlTexture)
        {
            const bool integerTexture = shader->textureIsIntegerFormat[relative_textureUnit];
            const bool depthTexture = shader->textureUsesDepthCompare[relative_textureUnit] && IsValidDepthTextureType(textureDim);
            MTL::Texture* nullTexture = GetNullSampledTexture(textureDim, integerTexture, depthTexture);
            if (!textureView)
            {
                mtlTexture = nullTexture;
            }
            else if (textureDim == Latte::E_DIM::DIM_1D && (textureView->dim != Latte::E_DIM::DIM_1D))
            {
                mtlTexture = nullTexture;
            }
            else if (textureDim == Latte::E_DIM::DIM_2D && (textureView->dim != Latte::E_DIM::DIM_2D && textureView->dim != Latte::E_DIM::DIM_2D_MSAA))
            {
                mtlTexture = nullTexture;
            }
            else if (textureDim != Latte::E_DIM::DIM_1D &&
                     textureDim != Latte::E_DIM::DIM_2D &&
                     textureView->dim != textureDim)
            {
                mtlTexture = nullTexture;
            }
            else
            {
                // get texture register word 0
                uint32 word4 = LatteGPUState.contextRegister[texUnitRegIndex + 4];
                mtlTexture = textureView->GetSwizzledView(word4);
            }
            if (textureFastPathOn && mtlTexture)
                m_textureBindCache->SetPassTexture(shader->shaderType, relative_textureUnit, m_drawPassGeneration, mtlTexture);
        }
        
        if (argumentEncoder)
        {
            argumentBindings[MetalArgumentBuffer::TextureBase + relative_textureUnit] = {MetalArgumentBinding::Type::Texture, mtlTexture, 0};
            DeclareResidency(renderCommandEncoder, mtlTexture, MTL::ResourceUsageRead | MTL::ResourceUsageSample, renderStage);
        }
        else
            SetTexture(renderCommandEncoder, mtlShaderType, mtlTexture, binding);
    }
    if (bindLoopSkipEligible)
    {
        // Record that this stage's DIRECT texture+sampler binds are now live on the current encoder for this
        // pass, so subsequent draws in the pass (same generation + epoch + shader) can skip the loop above.
        m_bindLoopShader[mtlShaderType] = static_cast<const void*>(shader);
        m_bindLoopGeneration[mtlShaderType] = m_drawPassGeneration;
        m_bindLoopEpoch[mtlShaderType] = m_encoderEpoch;
    }
    } // end if (runBindLoop)

    // Support buffer
    auto GET_UNIFORM_DATA_PTR = [&](size_t index) { return supportBufferData + (index / 4); };
    
    sint32 shaderAluConst;
    sint32 shaderUniformRegisterOffset;
    
    switch (shader->shaderType)
    {
        case LatteConst::ShaderType::Vertex:
            shaderAluConst = 0x400;
            shaderUniformRegisterOffset = mmSQ_VTX_UNIFORM_BLOCK_START;
            break;
        case LatteConst::ShaderType::Pixel:
            shaderAluConst = 0;
            shaderUniformRegisterOffset = mmSQ_PS_UNIFORM_BLOCK_START;
            break;
        case LatteConst::ShaderType::Geometry:
            shaderAluConst = 0; // geometry shader has no ALU const
            shaderUniformRegisterOffset = mmSQ_GS_UNIFORM_BLOCK_START;
            break;
        default:
            UNREACHABLE;
    }
    
    if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
    {
        if (shader->uniform.uniformRangeSize > sizeof(supportBufferData))
        {
            cemuLog_logOnce(LogType::Force, "Metal shader {:016x} exceeds the support buffer capacity", shader->baseHash);
            return false;
        }
        if (shader->uniform.list_ufTexRescale.empty() == false)
        {
            for (auto& entry : shader->uniform.list_ufTexRescale)
            {
                float* xyScale = LatteTexture_getEffectiveTextureScale(shader->shaderType, entry.texUnit);
                memcpy(entry.currentValue, xyScale, sizeof(float) * 2);
                memcpy(GET_UNIFORM_DATA_PTR(entry.uniformLocation), xyScale, sizeof(float) * 2);
            }
        }
        if (shader->uniform.loc_alphaTestRef >= 0)
        {
            *GET_UNIFORM_DATA_PTR(shader->uniform.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
        }
        if (shader->uniform.loc_pointSize >= 0)
        {
            const auto& pointSizeReg = LatteGPUState.contextNew.PA_SU_POINT_SIZE;
            float pointWidth = (float)pointSizeReg.get_WIDTH() / 8.0f;
            if (pointWidth == 0.0f)
                pointWidth = 1.0f / 8.0f; // minimum size
            *GET_UNIFORM_DATA_PTR(shader->uniform.loc_pointSize) = pointWidth;
        }
        if (shader->uniform.loc_remapped >= 0)
        {
            LatteBufferCache_LoadRemappedUniforms(shader, GET_UNIFORM_DATA_PTR(shader->uniform.loc_remapped));
        }
        if (shader->uniform.loc_uniformRegister >= 0)
        {
            uint32* uniformRegData = (uint32*)(LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + shaderAluConst);
            memcpy(GET_UNIFORM_DATA_PTR(shader->uniform.loc_uniformRegister), uniformRegData, shader->uniform.count_uniformRegister * 16);
        }
        if (shader->uniform.loc_windowSpaceToClipSpaceTransform >= 0)
        {
            sint32 viewportWidth;
            sint32 viewportHeight;
            LatteRenderTarget_GetCurrentVirtualViewportSize(&viewportWidth, &viewportHeight); // always call after _updateViewport()
            float* v = GET_UNIFORM_DATA_PTR(shader->uniform.loc_windowSpaceToClipSpaceTransform);
            v[0] = 2.0f / (float)viewportWidth;
            v[1] = 2.0f / (float)viewportHeight;
        }
        if (shader->uniform.loc_fragCoordScale >= 0)
        {
            LatteMRT::GetCurrentFragCoordScale(GET_UNIFORM_DATA_PTR(shader->uniform.loc_fragCoordScale));
        }
        if (shader->uniform.loc_baseVertex >= 0)
            *reinterpret_cast<sint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_baseVertex)) = m_state.m_drawResources.baseVertex;
        if (shader->uniform.loc_baseInstance >= 0)
            *reinterpret_cast<uint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_baseInstance)) = m_state.m_drawResources.baseInstance;
        if (shader->uniform.loc_verticesPerInstance >= 0)
        {
            *reinterpret_cast<uint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_verticesPerInstance)) = m_state.m_streamoutState.verticesPerInstance;
            for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
            {
                if (shader->uniform.loc_streamoutBufferBase[b] >= 0)
                {
                    *reinterpret_cast<uint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferBase[b])) = m_state.m_streamoutState.buffers[b].ringBufferOffset;
                }
                if (shader->uniform.loc_streamoutBufferSize[b] >= 0)
                    *reinterpret_cast<uint32*>(GET_UNIFORM_DATA_PTR(shader->uniform.loc_streamoutBufferSize[b])) = m_state.m_streamoutState.buffers[b].rangeSize;
            }
        }
        
        size_t size = shader->uniform.uniformRangeSize;
        auto* allocation = m_memoryManager->GetCachedSnapshot(MetalMemoryManager::SupportSnapshotBase + mtlShaderType, supportBufferData, size);
        if (argumentEncoder && shader->resourceMapping.supportBufferDirectBinding < 0)
        {
            argumentBindings[MetalArgumentBuffer::SupportBuffer] = {MetalArgumentBinding::Type::Buffer, allocation->mtlBuffer, allocation->bufferOffset};
            DeclareResidency(renderCommandEncoder, allocation->mtlBuffer, MTL::ResourceUsageRead, renderStage);
        }
        else
        {
            // Direct support-buffer binding. Two cases reach here:
            //  - Direct ABI (no argument encoder): the support buffer is bound at uniformVarsBufferBindingPoint,
            //    exactly as before.
            //  - Experimental "Support Buffer Indirection" (argument encoder present + supportBufferDirectBinding
            //    >= 0): the support buffer was pulled out of the argument buffer and is bound at its dedicated
            //    direct slot so its per-draw change no longer forces a whole-stage arg-buffer re-encode.
            // In both cases argumentBindings[SupportBuffer] is left Unused, so the reflected argument encoder
            // (which omits the id(SupportBuffer) member in the indirection case) skips it and, critically, the
            // arg-buffer cache key no longer rotates with the support snapshot -- the whole point of the toggle.
            // A direct setBuffer makes the resource resident automatically, so no DeclareResidency is needed.
            const sint32 supportSlot = (shader->resourceMapping.supportBufferDirectBinding >= 0)
                ? shader->resourceMapping.supportBufferDirectBinding
                : shader->resourceMapping.uniformVarsBufferBindingPoint;
            SetBuffer(renderCommandEncoder, mtlShaderType, allocation->mtlBuffer, allocation->bufferOffset, supportSlot);
            if (argumentEncoder)
                m_performanceMonitor.m_supportIndirectionDraws++;
        }
    }
    
    // Uniform buffers
    for (sint32 i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
    {
        if (shader->resourceMapping.uniformBuffersBindingPoint[i] >= 0)
        {
            uint32 binding = shader->resourceMapping.uniformBuffersBindingPoint[i];
            if (binding >= MAX_MTL_BUFFERS)
            {
                cemuLog_logOnce(LogType::Force, "invalid buffer binding {}", binding);
                continue;
            }
            
            MetalGeneralShaderType shaderType = GetMtlGeneralShaderType(shader->shaderType);
            MTL::Buffer* buffer = m_state.m_uniformBuffers[shaderType][i];
            size_t offset = m_state.m_uniformBufferOffsets[shaderType][i];
            size_t size = m_state.m_uniformBufferSizes[shaderType][i];
            if (!buffer || offset == INVALID_OFFSET || offset >= buffer->length())
            {
                buffer = m_nullBuffer;
                offset = 0;
            }
            else
            {
                size = std::min(size, buffer->length() - offset);
            }

            m_memoryManager->TrackSharedCache(buffer, offset, size);

            if (argumentEncoder)
            {
                argumentBindings[MetalArgumentBuffer::UniformBufferBase + i] = {MetalArgumentBinding::Type::Buffer, buffer, offset};
                DeclareResidency(renderCommandEncoder, buffer, MTL::ResourceUsageRead, renderStage);
            }
            else
                SetBuffer(renderCommandEncoder, mtlShaderType, buffer, offset, binding);
        }
    }
    
    // Storage buffer
    if (shader->resourceMapping.tfStorageBindingPoint >= 0)
    {
        MTL::Buffer* xfbRingBuffer = GetXfbRingBuffer() ? GetXfbRingBuffer() : m_nullBuffer;
        if (argumentEncoder)
        {
            argumentBindings[MetalArgumentBuffer::StreamoutBuffer] = {MetalArgumentBinding::Type::Buffer, xfbRingBuffer, 0};
            DeclareResidency(renderCommandEncoder, xfbRingBuffer, MTL::ResourceUsageWrite, renderStage);
        }
        else
            SetBuffer(renderCommandEncoder, mtlShaderType, xfbRingBuffer, 0, shader->resourceMapping.tfStorageBindingPoint);
    }
    
    if (argumentEncoder && shader->shaderType == LatteConst::ShaderType::Vertex)
    {
        const LatteFetchShader* fetchShader = LatteSHRC_GetActiveFetchShader();
        const bool fetchVertexManually =
        usesGeometryShader ||
        (fetchShader && fetchShader->mtlFetchVertexManually) ||
        (shader->hasStreamoutBufferWrite && !usesGeometryShader);
        if (fetchVertexManually && fetchShader)
        {
            bool encodedVertexBuffers[LATTE_MAX_VERTEX_BUFFERS]{};
            for (const auto& bufferGroup : fetchShader->bufferGroups)
            {
                const uint32 bufferIndex = bufferGroup.attributeBufferIndex;
                if (bufferIndex >= LATTE_MAX_VERTEX_BUFFERS || encodedVertexBuffers[bufferIndex])
                    continue;
                
                MTL::Buffer* vertexBuffer = m_state.m_vertexBuffers[bufferIndex];
                size_t vertexBufferOffset = m_state.m_vertexBufferOffsets[bufferIndex];
                size_t vertexBufferSize = m_state.m_vertexBufferSizes[bufferIndex];
                if (!vertexBuffer || vertexBufferOffset == INVALID_OFFSET || vertexBufferOffset >= vertexBuffer->length())
                {
                    vertexBuffer = m_nullBuffer;
                    vertexBufferOffset = 0;
                    vertexBufferSize = 0;
                }
                argumentBindings[MetalArgumentBuffer::VertexBufferBase + bufferIndex] = {MetalArgumentBinding::Type::Buffer, vertexBuffer, vertexBufferOffset};
                DeclareResidency(renderCommandEncoder, vertexBuffer, MTL::ResourceUsageRead, renderStage);
                vertexBufferSize = std::min<size_t>(vertexBufferSize, vertexBuffer->length() - vertexBufferOffset);
                argumentBindings[MetalArgumentBuffer::VertexBufferSizeBase + bufferIndex] = {MetalArgumentBinding::Type::Constant, nullptr,
                    static_cast<uint32>(std::min<size_t>(vertexBufferSize, std::numeric_limits<uint32>::max()))};
                encodedVertexBuffers[bufferIndex] = true;
            }
        }
        
        const bool usesLogicalVertexIds = usesGeometryShader || shader->hasStreamoutBufferWrite;
        if (usesLogicalVertexIds)
        {
            MTL::Buffer* indexBuffer = m_state.m_drawResources.indexBuffer;
            size_t indexBufferOffset = m_state.m_drawResources.indexBufferOffset;
            size_t indexBufferSize = m_state.m_drawResources.indexBufferSize;
            if (!indexBuffer || indexBufferOffset >= indexBuffer->length())
            {
                indexBuffer = m_nullBuffer;
                indexBufferOffset = 0;
                indexBufferSize = 0;
            }
            argumentBindings[MetalArgumentBuffer::IndexBuffer] = {MetalArgumentBinding::Type::Buffer, indexBuffer, indexBufferOffset};
            DeclareResidency(renderCommandEncoder, indexBuffer, MTL::ResourceUsageRead, renderStage);
            indexBufferSize = std::min<size_t>(indexBufferSize, indexBuffer->length() - indexBufferOffset);
            argumentBindings[MetalArgumentBuffer::IndexBufferSize] = {MetalArgumentBinding::Type::Constant, nullptr,
                static_cast<uint32>(std::min<size_t>(indexBufferSize, std::numeric_limits<uint32>::max()))};
            argumentBindings[MetalArgumentBuffer::IndexType] = {MetalArgumentBinding::Type::Constant, nullptr, m_state.m_drawResources.indexType};
        }
    }
    
    if (argumentEncoder)
    {
        // Residency declarations above are needed for every encoder, including when the immutable
        // argument-buffer contents can be reused (the arg buffer is deduped by GetCachedArgumentBuffer,
        // but each referenced resource must still be made resident on whichever encoder will draw).
        // The "Skip Redundant GPU Residency" experimental toggle only removes the *repeat*
        // declarations of an already-resident resource on the same encoder (via DeclareResidency); the
        // first declaration on each encoder still happens, so this path is unchanged when the toggle is OFF.
        MetalSynchronizedHeapAllocator::AllocatorReservation* allocation;
        {
            ScopedStageTimer _stArg(performanceMonitor.cpuTime_dcArgEncode, m_captureCpuStageTimings);
            allocation = m_memoryManager->GetCachedArgumentBuffer(mtlShaderType, argumentEncoder, argumentBindings);
        }
        SetBuffer(renderCommandEncoder, mtlShaderType, allocation->mtlBuffer, allocation->bufferOffset, shader->resourceMapping.argumentBufferBindingPoint);
    }
    return true;
}

void MetalRenderer::ClearColorTextureInternal(MTL::Texture* mtlTexture, sint32 sliceIndex, sint32 mipIndex, float r, float g, float b, float a)
{
    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    auto colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    colorAttachment->setTexture(mtlTexture);
    colorAttachment->setClearColor(MTL::ClearColor(r, g, b, a));
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setStoreAction(MTL::StoreActionStore);
    colorAttachment->setSlice(sliceIndex);
    colorAttachment->setLevel(mipIndex);

    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    EndEncoding();

    // Debug
    m_performanceMonitor.m_clears++;
}

void MetalRenderer::ClearDepthTextureInternal(MTL::Texture* mtlTexture, uint32 sliceIndex, sint32 mipIndex, bool clearDepth, bool clearStencil, float depthValue, uint32 stencilValue)
{
    NS_STACK_SCOPED MTL::RenderPassDescriptor* renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    if (clearDepth)
    {
        auto depthAttachment = renderPassDescriptor->depthAttachment();
        depthAttachment->setTexture(mtlTexture);
        depthAttachment->setClearDepth(depthValue);
        depthAttachment->setLoadAction(MTL::LoadActionClear);
        depthAttachment->setStoreAction(MTL::StoreActionStore);
        depthAttachment->setSlice(sliceIndex);
        depthAttachment->setLevel(mipIndex);
    }
    if (clearStencil)
    {
        auto stencilAttachment = renderPassDescriptor->stencilAttachment();
        stencilAttachment->setTexture(mtlTexture);
        stencilAttachment->setClearStencil(stencilValue);
        stencilAttachment->setLoadAction(MTL::LoadActionClear);
        stencilAttachment->setStoreAction(MTL::StoreActionStore);
        stencilAttachment->setSlice(sliceIndex);
        stencilAttachment->setLevel(mipIndex);
    }

    GetTemporaryRenderCommandEncoder(renderPassDescriptor);
    EndEncoding();

    // Debug
    m_performanceMonitor.m_clears++;
}

MetalRenderer::PendingClear& MetalRenderer::RecordPendingClear(LatteTexture* tex, sint32 slice, sint32 mip, bool isDepth)
{
    for (auto& rec : m_pendingClears)
    {
        if (rec.latteTexture == tex && rec.sliceIndex == slice && rec.mipIndex == mip && rec.isDepth == isDepth)
            return rec;
    }
    m_pendingClears.push_back({});
    PendingClear& rec = m_pendingClears.back();
    rec.latteTexture = tex;
    rec.sliceIndex = slice;
    rec.mipIndex = mip;
    rec.isDepth = isDepth;
    return rec;
}

void MetalRenderer::EmitPendingClearNow(const PendingClear& rec)
{
    MTL::Texture* mtlTexture = static_cast<LatteTextureMtl*>(rec.latteTexture)->GetTexture();
    if (rec.isDepth)
        ClearDepthTextureInternal(mtlTexture, rec.sliceIndex, rec.mipIndex, rec.clearDepth, rec.clearStencil, rec.depthValue, rec.stencilValue);
    else
        ClearColorTextureInternal(mtlTexture, rec.sliceIndex, rec.mipIndex, rec.r, rec.g, rec.b, rec.a);
}

void MetalRenderer::FlushPendingClears()
{
    if (m_pendingClears.empty())
        return;

    // Swap out first so any re-entrant flush (an internal EndEncoding -> opportunistic
    // CommitCommandBuffer -> FlushPendingClears) sees an empty list and returns.
    std::vector<PendingClear> pending;
    pending.swap(m_pendingClears);
    for (const auto& rec : pending)
        EmitPendingClearNow(rec);
}

void MetalRenderer::NotifyLatteTextureDeleted(LatteTexture* tex)
{
    if (m_pendingClears.empty())
        return;

    // Drop (do not materialize) pending clears for a texture being destroyed:
    // its post-clear contents are unobservable once the texture is gone.
    m_pendingClears.erase(std::remove_if(m_pendingClears.begin(), m_pendingClears.end(),
        [tex](const PendingClear& rec) { return rec.latteTexture == tex; }), m_pendingClears.end());
}

void MetalRenderer::CopyBufferToBuffer(MTL::Buffer* src, uint32 srcOffset, MTL::Buffer* dst, uint32 dstOffset, uint32 size, MTL::RenderStages after, MTL::RenderStages before)
{
    // TODO: uncomment and fix performance issues
    // Do the copy in a vertex shader on Apple GPUs
    /*
    if (m_isAppleGPU && m_encoderType == MetalEncoderType::Render)
    {
        auto renderCommandEncoder = static_cast<MTL::RenderCommandEncoder*>(m_commandEncoder);

        MTL::Resource* barrierBuffers[] = {src};
        renderCommandEncoder->memoryBarrier(barrierBuffers, 1, after, after | MTL::RenderStageVertex);

        renderCommandEncoder->setRenderPipelineState(m_copyBufferToBufferPipeline->GetRenderPipelineState());
        m_state.m_encoderState.m_renderPipelineState = m_copyBufferToBufferPipeline->GetRenderPipelineState();

        SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, src, srcOffset, GET_HELPER_BUFFER_BINDING(0));
        SetBuffer(renderCommandEncoder, METAL_SHADER_TYPE_VERTEX, dst, dstOffset, GET_HELPER_BUFFER_BINDING(1));

        renderCommandEncoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(size));

        barrierBuffers[0] = dst;
        renderCommandEncoder->memoryBarrier(barrierBuffers, 1, before | MTL::RenderStageVertex, before);
    }
    else
    {
    */
        auto blitCommandEncoder = GetBlitCommandEncoder();

        blitCommandEncoder->copyFromBuffer(src, srcOffset, dst, dstOffset, size);
    //}
}

void MetalRenderer::SwapBuffer(bool mainWindow)
{
    auto& layer = GetLayer(mainWindow);
    const bool drawableAlreadyAcquired = layer.GetDrawable() != nullptr;

    if (!AcquireDrawable(mainWindow))
        return;
    
    if (!drawableAlreadyAcquired)
        ClearColorTextureInternal(layer.GetDrawable()->texture(), 0, 0, 0.0f, 0.0f, 0.0f, 1.0f);

    auto commandBuffer = GetCommandBuffer();
    layer.PresentDrawable(commandBuffer);
}

void MetalRenderer::EnsureImGuiBackend()
{
    if (!ImGui::GetIO().BackendRendererUserData)
    {
        ImGui_ImplMetal_Init(m_device);
        //ImGui_ImplMetal_CreateFontsTexture(m_device);
    }
}

void MetalRenderer::StartCapture()
{
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    auto desc = MTL::CaptureDescriptor::alloc()->init();
    desc->setCaptureObject(m_device);

    // Check if a debugger with support for GPU capture is attached
    if (captureManager->supportsDestination(MTL::CaptureDestinationDeveloperTools))
    {
        desc->setDestination(MTL::CaptureDestinationDeveloperTools);
    }
    else
    {
        if (GetConfig().gpu_capture_dir.GetValue().empty())
        {
            cemuLog_log(LogType::Force, "No GPU capture directory specified, cannot do a GPU capture");
            return;
        }

        // Check if the GPU trace document destination is available
        if (!captureManager->supportsDestination(MTL::CaptureDestinationGPUTraceDocument))
        {
            cemuLog_log(LogType::Force, "GPU trace document destination is not available, cannot do a GPU capture");
            return;
        }

        // Get current date and time as a string
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d_%H-%M-%S");
        std::string now_str = oss.str();

        std::string capturePath = fmt::format("{}/cemu_{}.gputrace", GetConfig().gpu_capture_dir.GetValue(), now_str);
        desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
        desc->setOutputURL(ToNSURL(capturePath));
    }

    NS::Error* error = nullptr;
    captureManager->startCapture(desc, &error);
    if (error)
    {
        cemuLog_log(LogType::Force, "Failed to start GPU capture: {}", error->localizedDescription()->utf8String());
    }

    m_capturing = true;
}

void MetalRenderer::EndCapture()
{
    auto captureManager = MTL::CaptureManager::sharedCaptureManager();
    captureManager->stopCapture();

    m_capturing = false;
}
