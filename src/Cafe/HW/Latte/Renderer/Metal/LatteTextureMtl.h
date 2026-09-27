#pragma once

#include <Metal/Metal.hpp>

#include <unordered_map>

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "HW/Latte/ISA/LatteReg.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

class LatteTextureMtl : public LatteTexture
{
public:
	LatteTextureMtl(class MetalRenderer* mtlRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth, bool isRenderTarget);
	~LatteTextureMtl();

	MTL::Texture* GetTexture() const {
	    return m_texture;
	}

	void AllocateOnHost() override;

	// Experimental "skip redundant texture upload" (experimental_skip_redundant_upload). Per-subresource
	// 128-bit fingerprint (+ byte size) of the data last uploaded to this texture via texture_loadSlice.
	// The texture is (re)allocated exactly once, in the constructor, and this object owns that single
	// MTL::Texture for its whole life, so a fingerprint recorded here always describes the CURRENT
	// resident content of that subresource. The caller only records/consults these while the texture has
	// never been GPU-written (isUpdatedOnGPU == false, a monotonic flag), so the resident content came
	// solely from these uploads and a fingerprint match => the identical bytes are already resident.
	bool TryReuseUpload(uint64 key, uint64 h0, uint64 h1, uint32 size) const
	{
		auto it = m_uploadFingerprints.find(key);
		return it != m_uploadFingerprints.end() && it->second.size == size && it->second.h0 == h0 && it->second.h1 == h1;
	}
	void RecordUpload(uint64 key, uint64 h0, uint64 h1, uint32 size)
	{
		m_uploadFingerprints[key] = {h0, h1, size};
	}

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;

private:
	struct UploadFingerprint { uint64 h0; uint64 h1; uint32 size; };

	class MetalRenderer* m_mtlr;

	MTL::Texture* m_texture;

	std::unordered_map<uint64, UploadFingerprint> m_uploadFingerprints; // keyed by (mipIndex<<32 | sliceIndex)
};
