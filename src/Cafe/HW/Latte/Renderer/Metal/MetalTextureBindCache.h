#pragma once

#include <Metal/Metal.hpp>

#include "HW/Latte/Core/LatteConst.h"
#include "HW/Latte/ISA/LatteReg.h"

// Experimental per-draw-pass texture-binding fast path (experimental_pass_texture_fastpath).
// Mirrors MetalSamplerCache's GetPassSampler/SetPassSampler. Within one CP draw pass the
// MTL::Texture* that BindStageResources resolves for a given (shaderType, textureUnit) is frozen: it
// is a pure function of the bound texture view (m_state.m_textures[...]), that texture unit's
// SQ_TEX_RESOURCE WORD4 swizzle register and the active shader identity - all constant across a pass
// (any change to a texture binding, a context register or the active shader ends the pass and bumps
// m_drawPassGeneration). A draw-pass generation plus a per-entry generation stamp therefore lets a
// later draw in the same pass reuse the resolved pointer directly, skipping the null-texture
// selection, the dimension checks and textureView->GetSwizzledView(word4). This is the same
// invariant and the same soundness proof as the shipped sampler fast path.
//
// GetPassTexture returns nullptr on a miss; the caller then resolves normally and records the result
// via SetPassTexture. The cached MTL::Texture* is only ever reused when its stamp equals the CURRENT
// pass generation, i.e. within the very pass in which the bound texture view is guaranteed alive and
// bound, so the pointer can never dangle (cross-pass reuse is intentionally not attempted - no
// texture-view generation counter exists to prove it sound). GPU-thread only. When the toggle is OFF
// neither method is called and the arrays stay untouched (stamps keep their initial 0, which
// m_drawPassGeneration - starting at 1 and only incrementing - never equals, so no stale hit is
// possible). Only a non-null resolve is cached; a null resolve is simply re-resolved each draw
// (cheap, and never wrong).
class MetalTextureBindCache
{
public:
	MTL::Texture* GetPassTexture(LatteConst::ShaderType shaderType, uint32 textureUnit, uint32 generation) const
	{
		const uint32 s = (uint32)shaderType;
		if (m_fpTextureGen[s][textureUnit] == generation)
			return m_fpTexture[s][textureUnit];
		return nullptr;
	}

	void SetPassTexture(LatteConst::ShaderType shaderType, uint32 textureUnit, uint32 generation, MTL::Texture* texture)
	{
		const uint32 s = (uint32)shaderType;
		m_fpTexture[s][textureUnit] = texture;
		m_fpTextureGen[s][textureUnit] = generation;
	}

private:
	// Per-(shaderType, textureUnit) resolved-texture fast-path cache; see the header comment.
	uint32 m_fpTextureGen[(uint32)LatteConst::ShaderType::TotalCount][LATTE_NUM_MAX_TEX_UNITS] = {};
	MTL::Texture* m_fpTexture[(uint32)LatteConst::ShaderType::TotalCount][LATTE_NUM_MAX_TEX_UNITS] = {};
};
