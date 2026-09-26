#include "Cafe/HW/Latte/Renderer/Metal/MetalFXUpscaler.h"

#include <MetalFX/MetalFX.hpp>

#include "Cemu/Logging/CemuLogging.h"

MetalFXSpatialUpscaler::MetalFXSpatialUpscaler(MTL::Device* device)
	: m_device(device)
{
}

MetalFXSpatialUpscaler::~MetalFXSpatialUpscaler()
{
	ReleaseScaler();
	ReleaseTextures();
}

bool MetalFXSpatialUpscaler::IsSupported(MTL::Device* device)
{
	if (!device)
		return false;

	if (__builtin_available(iOS 16.0, macOS 13.0, *))
		return MTLFX::SpatialScalerDescriptor::supportsDevice(device);

	return false;
}

bool MetalFXSpatialUpscaler::Configure(uint32 inputWidth, uint32 inputHeight, uint32 outputWidth, uint32 outputHeight,
									   MTL::PixelFormat colorFormat, sint32 colorProcessingMode)
{
	if (inputWidth == 0 || inputHeight == 0 || outputWidth == 0 || outputHeight == 0)
		return false;

	// Fast path: unchanged key -> reuse the existing scaler and textures (per-frame no-op).
	if (m_scaler &&
		inputWidth == m_inputWidth && inputHeight == m_inputHeight &&
		outputWidth == m_outputWidth && outputHeight == m_outputHeight &&
		colorFormat == m_colorFormat && colorProcessingMode == m_colorProcessingMode)
	{
		return true;
	}

	// Key changed (or first use): tear down and rebuild.
	ReleaseScaler();
	ReleaseTextures();

	m_inputWidth = inputWidth;
	m_inputHeight = inputHeight;
	m_outputWidth = outputWidth;
	m_outputHeight = outputHeight;
	m_colorFormat = colorFormat;
	m_colorProcessingMode = colorProcessingMode;

	if (__builtin_available(iOS 16.0, macOS 13.0, *))
	{
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		MTLFX::SpatialScalerDescriptor* desc = MTLFX::SpatialScalerDescriptor::alloc()->init();
		desc->setColorTextureFormat(colorFormat);
		desc->setOutputTextureFormat(colorFormat);
		desc->setInputWidth(inputWidth);
		desc->setInputHeight(inputHeight);
		desc->setOutputWidth(outputWidth);
		desc->setOutputHeight(outputHeight);

		MTLFX::SpatialScalerColorProcessingMode cpMode;
		switch (colorProcessingMode)
		{
		case 1:  cpMode = MTLFX::SpatialScalerColorProcessingModeLinear; break;
		case 2:  cpMode = MTLFX::SpatialScalerColorProcessingModeHDR; break;
		default: cpMode = MTLFX::SpatialScalerColorProcessingModePerceptual; break;
		}
		desc->setColorProcessingMode(cpMode);

		m_scaler = desc->newSpatialScaler(m_device); // nil if this device/format/geometry is unsupported
		desc->release();

		if (!m_scaler)
		{
			pool->release();
			cemuLog_log(LogType::Force, "MetalFX: failed to create spatial scaler ({}x{} -> {}x{}); falling back", inputWidth, inputHeight, outputWidth, outputHeight);
			return false;
		}

		// Owned intermediates, created with exactly the usage flags MetalFX asks for. Private storage
		// (GPU-only); default hazard tracking keeps copy-in -> scale -> sample ordered within one
		// command buffer without an explicit fence.
		auto makeTexture = [&](uint32 w, uint32 h, MTL::TextureUsage usage) -> MTL::Texture* {
			MTL::TextureDescriptor* texDesc = MTL::TextureDescriptor::alloc()->init();
			texDesc->setTextureType(MTL::TextureType2D);
			texDesc->setPixelFormat(colorFormat);
			texDesc->setWidth(w);
			texDesc->setHeight(h);
			texDesc->setUsage(usage);
			texDesc->setStorageMode(MTL::StorageModePrivate);
			MTL::Texture* tex = m_device->newTexture(texDesc);
			texDesc->release();
			return tex;
		};

		m_inputTexture = makeTexture(inputWidth, inputHeight, m_scaler->colorTextureUsage());
		m_outputTexture = makeTexture(outputWidth, outputHeight, m_scaler->outputTextureUsage());

		// Remember the color-input usage requirement so a caller can decide whether an external source
		// texture may be bound directly (see SetColorTexture / GetRequiredColorTextureUsage).
		m_colorTextureUsage = m_scaler->colorTextureUsage();

		if (!m_inputTexture || !m_outputTexture)
		{
			ReleaseScaler();
			ReleaseTextures();
			pool->release();
			cemuLog_log(LogType::Force, "MetalFX: failed to create intermediate textures; falling back");
			return false;
		}

		// Bind the persistent textures + input content size once; they are reused every frame until
		// the configuration key changes.
		m_scaler->setColorTexture(m_inputTexture);
		m_scaler->setOutputTexture(m_outputTexture);
		m_scaler->setInputContentWidth(inputWidth);
		m_scaler->setInputContentHeight(inputHeight);

		pool->release();
		return true;
	}

	return false;
}

void MetalFXSpatialUpscaler::Encode(MTL::CommandBuffer* commandBuffer)
{
	if (!commandBuffer || !m_scaler)
		return;

	if (__builtin_available(iOS 16.0, macOS 13.0, *))
		m_scaler->encodeToCommandBuffer(commandBuffer);
}

void MetalFXSpatialUpscaler::SetColorTexture(MTL::Texture* texture)
{
	if (!m_scaler || !texture)
		return;

	if (__builtin_available(iOS 16.0, macOS 13.0, *))
		m_scaler->setColorTexture(texture);
}

void MetalFXSpatialUpscaler::ReleaseTextures()
{
	if (m_inputTexture)
	{
		m_inputTexture->release();
		m_inputTexture = nullptr;
	}
	if (m_outputTexture)
	{
		m_outputTexture->release();
		m_outputTexture = nullptr;
	}
}

void MetalFXSpatialUpscaler::ReleaseScaler()
{
	if (m_scaler)
	{
		m_scaler->release();
		m_scaler = nullptr;
	}
	m_colorTextureUsage = MTL::TextureUsageUnknown;
}
