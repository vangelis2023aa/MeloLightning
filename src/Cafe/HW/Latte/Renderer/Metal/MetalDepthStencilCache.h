#pragma once

#include <Metal/Metal.hpp>

#include "HW/Latte/ISA/LatteReg.h"

class MetalDepthStencilCache
{
public:
    MetalDepthStencilCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalDepthStencilCache();

    MTL::DepthStencilState* GetDepthStencilState(const LatteContextRegister& lcr, bool hasDepthStencilAttachment);

private:
    class MetalRenderer* m_mtlr;

    std::unordered_map<uint64, MTL::DepthStencilState*> m_depthStencilCache;

	// Experimental per-draw-pass fast path (shares experimental_pipeline_cache_fast_path). Every
	// input to CalculateDepthStencilHash is a context register and is therefore frozen for the whole
	// draw pass; the only value that can differ between draws in a pass is hasDepthStencilAttachment.
	// Reuse the last resolved state when the draw-pass generation and that flag both match, skipping
	// the hash + map probe. GPU-thread only; when the toggle is OFF m_fpDepthStencilState stays null
	// and the fast path is never entered.
	uint32 m_fpGeneration = 0;
	bool m_fpHasDepthStencilAttachment = false;
	MTL::DepthStencilState* m_fpDepthStencilState = nullptr;

    uint64 CalculateDepthStencilHash(const LatteContextRegister& lcr, bool hasDepthStencilAttachment);
};
