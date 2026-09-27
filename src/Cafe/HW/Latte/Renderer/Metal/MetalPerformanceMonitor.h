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
    // Developer-only fragmentation counters (see AppendOverlayDebugInfo). Observation only.
    uint32 m_drawCalls = 0;      // guest draws submitted to draw_execute this frame
    uint32 m_drawPassBegins = 0; // CP continuous-draw-pass begins this frame (fragmentation numerator)
    uint32 m_snapshotMisses = 0; // snapshot cache misses (re-copies) this frame

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
        m_drawCalls = 0;
        m_drawPassBegins = 0;
        m_snapshotMisses = 0;
    }
};
