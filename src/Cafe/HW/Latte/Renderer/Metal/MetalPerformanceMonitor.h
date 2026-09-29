#pragma once

class MetalPerformanceMonitor
{
public:
    // Per frame data
    uint32 m_commandBuffers = 0;
    uint32 m_renderPasses = 0;
    uint32 m_clears = 0;
    uint32 m_manualVertexFetchDraws = 0;
    uint32 m_meshDraws = 0;
    uint32 m_triangleFans = 0;
    uint64 m_snapshotBytes = 0;
    uint32 m_snapshotReuses = 0;
    uint32 m_argumentBufferEncodes = 0;
    uint32 m_argumentBufferReuses = 0;
    // Experimental "Direct Shader Bindings" proof counter: number of BindStageResources calls this frame
    // that took the direct (no argument-buffer) path for an eligible game shader. When the toggle is OFF
    // this stays 0 (every game shader keeps its argument buffer); when ON it rises as the arg-buffer
    // encodes/reuses above fall, which is the direct evidence the storm was moved off the arg-buffer path.
    uint32 m_directBindingDraws = 0;
    // Experimental "Skip Repeated Texture Binds" proof counter (experimental_binding_dirty_masks): number of
    // BindStageResources calls this frame that skipped the frozen texture+sampler bind loop for a DIRECT-ABI
    // stage because its (generation, encoder-epoch, shader) key still matched. 0 when the toggle is OFF (or when
    // Direct Shader Bindings is OFF, since only direct-ABI stages are eligible). As it rises it should track the
    // number of draws-after-the-first within passes; it is the direct evidence the per-draw bind scan was elided.
    uint32 m_bindLoopSkips = 0;
    // Experimental "Support Buffer Indirection" proof counter (experimental_support_buffer_indirection): number
    // of BindStageResources calls this frame that bound the support buffer DIRECTLY for a shader that is still on
    // the argument buffer (i.e. the support buffer was pulled out of the arg buffer to a dedicated slot). 0 when
    // the toggle is OFF (every arg-buffer shader keeps its support buffer inside the arg buffer). As it rises,
    // m_argumentBufferReuses should rise and m_argumentBufferEncodes should fall by roughly the same amount --
    // that pair is the direct evidence the per-draw whole-stage arg-buffer re-encode storm was eliminated.
    uint32 m_supportIndirectionDraws = 0;
    // Developer-only fragmentation counters (see AppendOverlayDebugInfo). Observation only.
    uint32 m_drawCalls = 0;      // guest draws submitted to draw_execute this frame
    uint32 m_drawPassBegins = 0; // CP continuous-draw-pass begins this frame (fragmentation numerator)
    uint32 m_snapshotMisses = 0; // snapshot cache misses (re-copies) this frame

    // Developer-only frame-budget diagnostics (see AppendOverlayDebugInfo). Behavior-neutral: read only.
    //
    // GPU execution time: measured from MTL::CommandBuffer::GPUStartTime()/GPUEndTime() read off each
    // command buffer as it is reaped (status==Completed) in ProcessFinishedCommandBuffers. This is real
    // GPU-domain time, NOT a CPU estimate. Because every command buffer GPU-waits on the previous one's
    // event (encodeWait in GetCommandBuffer), the CBs do not overlap on the GPU, so the sum of per-CB
    // (end-start) spans approximates GPU wall-clock busy time for the frame. It is APPROXIMATE only in
    // that CB reaping can straddle a frame boundary (a CB submitted late in frame N may be reaped in
    // N+1); accumulate during the frame, snapshot at ResetPerFrameData.
    double m_gpuActiveUs = 0.0;       // display: summed GPU exec time of CBs reaped this frame window (us)
    uint32 m_gpuActiveCBs = 0;        // display: number of CBs contributing to m_gpuActiveUs
    double m_gpuActiveAccumUs = 0.0;  // accumulator (rolled into m_gpuActiveUs at frame end)
    uint32 m_gpuActiveAccumCBs = 0;   // accumulator
    // Present cost: wall-clock (CLOCK_MONOTONIC_RAW) around the SwapBuffer()s + final CommitCommandBuffer
    // in SwapBuffers(). Captures drawable acquisition + PresentDrawable + final commit. This is CPU
    // wall-clock on the Latte thread. It does NOT include the vsync pacing wait (that is IT_HLE_WAIT_FOR_FLIP,
    // measured separately as gpuTime_flipTime). Written once per frame in SwapBuffers and intentionally NOT
    // cleared by ResetPerFrameData, so it holds the previous frame's value for display (1-frame display lag,
    // matching the getPreviousFrameValue() LattePerfStatTimers).
    uint64 m_presentTimeNs = 0;       // display: present + drawable acquire + final commit (ns)

    MetalPerformanceMonitor() = default;
    ~MetalPerformanceMonitor() = default;

    void ResetPerFrameData()
    {
        m_commandBuffers = 0;
        m_renderPasses = 0;
        m_clears = 0;
        m_manualVertexFetchDraws = 0;
        m_meshDraws = 0;
        m_triangleFans = 0;
        m_snapshotBytes = 0;
        m_snapshotReuses = 0;
        m_argumentBufferEncodes = 0;
        m_argumentBufferReuses = 0;
        m_directBindingDraws = 0;
        m_bindLoopSkips = 0;
        m_supportIndirectionDraws = 0;
        m_drawCalls = 0;
        m_drawPassBegins = 0;
        m_snapshotMisses = 0;
        // Snapshot the GPU-active accumulator into the displayed value, then reset the accumulator for the
        // next frame. m_presentTimeNs is deliberately left untouched (it is set directly in SwapBuffers).
        m_gpuActiveUs = m_gpuActiveAccumUs;
        m_gpuActiveCBs = m_gpuActiveAccumCBs;
        m_gpuActiveAccumUs = 0.0;
        m_gpuActiveAccumCBs = 0;
    }
};
