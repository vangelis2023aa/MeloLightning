#pragma once

#include <Metal/Metal.hpp>

#include "HW/Latte/Core/LatteConst.h"
#include "HW/Latte/ISA/LatteReg.h"

class MetalSamplerCache
{
public:
    MetalSamplerCache(class MetalRenderer* metalRenderer) : m_mtlr{metalRenderer} {}
    ~MetalSamplerCache();

    MTL::SamplerState* GetSamplerState(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords);

	// Experimental per-draw-pass fast path (experimental_sampler_cache_fast_path). Within one CP draw
	// pass the sampler resolved for a given (shaderType, textureUnit) is frozen: the SQ_TEX_SAMPLER
	// registers, the shader identity that maps a unit to a sampler, and the bound texture whose
	// anisotropic override is folded into WORD0 are all constant across a pass (any change to those
	// ends the pass). A draw-pass generation plus a per-entry generation stamp therefore lets a later
	// draw in the same pass reuse the resolved MTL::SamplerState* directly, skipping the aniso register
	// mutation, CalculateSamplerHash (which re-reads WORD0/1/2 and resolves the border colour) and the
	// unordered_map probe. GetPassSampler returns nullptr on a miss; the caller then resolves normally
	// and records it via SetPassSampler. Cached sampler objects are owned by m_samplerCache and are only
	// ever inserted (never erased outside the destructor), so a cached pointer can never dangle within
	// the cache's lifetime. GPU-thread only. When the toggle is OFF neither is called and the arrays
	// stay untouched (m_fpSamplerGen entries keep their initial 0, which m_drawPassGeneration — starting
	// at 1 and only incrementing — never equals, so no stale hit is possible).
	MTL::SamplerState* GetPassSampler(LatteConst::ShaderType shaderType, uint32 textureUnit, uint32 generation) const
	{
		const uint32 s = (uint32)shaderType;
		if (m_fpSamplerGen[s][textureUnit] == generation)
			return m_fpSampler[s][textureUnit];
		return nullptr;
	}

	void SetPassSampler(LatteConst::ShaderType shaderType, uint32 textureUnit, uint32 generation, MTL::SamplerState* sampler)
	{
		const uint32 s = (uint32)shaderType;
		m_fpSampler[s][textureUnit] = sampler;
		m_fpSamplerGen[s][textureUnit] = generation;
	}

private:
    class MetalRenderer* m_mtlr;

    std::unordered_map<uint64, MTL::SamplerState*> m_samplerCache;

	// Per-(shaderType, textureUnit) sampler-state fast-path cache; see GetPassSampler above.
	uint32 m_fpSamplerGen[(uint32)LatteConst::ShaderType::TotalCount][LATTE_NUM_MAX_TEX_UNITS] = {};
	MTL::SamplerState* m_fpSampler[(uint32)LatteConst::ShaderType::TotalCount][LATTE_NUM_MAX_TEX_UNITS] = {};

    uint64 CalculateSamplerHash(const LatteContextRegister& lcr, LatteConst::ShaderType shaderType, uint32 stageSamplerIndex, const _LatteRegisterSetSampler* samplerWords);
};
