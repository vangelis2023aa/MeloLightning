#pragma once

#include <Metal/Metal.hpp>

// Forward declaration keeps the MetalFX types confined to the implementation TU; only this class
// ever touches <MetalFX/MetalFX.hpp>.
namespace MTLFX { class SpatialScaler; }

// Wraps a single MTLFX::SpatialScaler together with the two owned intermediate textures used to run
// spatial upscaling at present time (a reduced-resolution color input and a full-resolution output).
//
// All MetalFX usage in the renderer is confined to this class. Everything here requires iOS 16+ and
// a device for which MTLFX::SpatialScalerDescriptor::supportsDevice() is true; construction of the
// scaler is guarded with __builtin_available and fails gracefully (IsReady()/Configure() return
// false) when MetalFX is unavailable, so callers can always fall back to the normal present path.
//
// Ownership: the scaler and both textures are created +1 and released by this class. The textures
// are recreated only when the configuration key changes (input/output size, color format, or
// color-processing mode), so per-frame reuse is a cheap no-op.
class MetalFXSpatialUpscaler
{
public:
	explicit MetalFXSpatialUpscaler(MTL::Device* device);
	~MetalFXSpatialUpscaler();

	MetalFXSpatialUpscaler(const MetalFXSpatialUpscaler&) = delete;
	MetalFXSpatialUpscaler& operator=(const MetalFXSpatialUpscaler&) = delete;

	// Runtime capability probe (iOS 16 + device support). Safe to call on any OS version/device.
	static bool IsSupported(MTL::Device* device);

	// (Re)configure the scaler and owned textures for the given geometry / color format /
	// color-processing mode. Idempotent and cheap when the key is unchanged. Returns true only when
	// the scaler is ready to encode; on any failure it releases partial state and returns false.
	bool Configure(uint32 inputWidth, uint32 inputHeight, uint32 outputWidth, uint32 outputHeight,
				   MTL::PixelFormat colorFormat, sint32 colorProcessingMode);

	bool IsReady() const { return m_scaler != nullptr; }

	// Valid after a successful Configure(). The caller copies its source into GetInputTexture(),
	// calls Encode(), then samples GetOutputTexture().
	MTL::Texture* GetInputTexture() const { return m_inputTexture; }
	MTL::Texture* GetOutputTexture() const { return m_outputTexture; }

	uint32 GetOutputWidth() const { return m_outputWidth; }
	uint32 GetOutputHeight() const { return m_outputHeight; }

	// Encodes the spatial-upscale pass (GetInputTexture() -> GetOutputTexture()) into commandBuffer.
	// The command buffer must have NO open encoder (the caller ends encoding first); MetalFX encodes
	// its own pass directly onto the command buffer. No-op if not ready.
	void Encode(MTL::CommandBuffer* commandBuffer);

private:
	void ReleaseTextures();
	void ReleaseScaler();

	MTL::Device* m_device = nullptr;

	MTLFX::SpatialScaler* m_scaler = nullptr;
	MTL::Texture* m_inputTexture = nullptr;
	MTL::Texture* m_outputTexture = nullptr;

	// Current configuration key. m_colorProcessingMode == -1 means "never configured".
	uint32 m_inputWidth = 0;
	uint32 m_inputHeight = 0;
	uint32 m_outputWidth = 0;
	uint32 m_outputHeight = 0;
	MTL::PixelFormat m_colorFormat = MTL::PixelFormatInvalid;
	sint32 m_colorProcessingMode = -1;
};
