#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>
#include <Types.h>
#include <Graphics/Parameters.h>

#ifndef REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
#if __has_include(<vulkan/vulkan.h>)
#define REALITYVK_INTERNAL_HAS_VULKAN_HEADERS 1
#include <vulkan/vulkan.h>
#else
#define REALITYVK_INTERNAL_HAS_VULKAN_HEADERS 0
#endif
#elif REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
#include <vulkan/vulkan.h>
#endif

namespace vulkan {

#if REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
	class TextureUploader
	{
	public:
		struct UploadRequest {
		u32 x = 0;
		u32 y = 0;
		u32 width = 0;
		u32 height = 0;
		u32 mipLevel = 0;
		graphics::ColorFormatParam format = graphics::colorFormat::RGBA;
		graphics::DatatypeParam type = graphics::datatype::UNSIGNED_BYTE;
		const void * data = nullptr;
			s32 unpackAlignment = 1;
		};

		struct BlitRequest {
			s32 srcX0 = 0;
			s32 srcY0 = 0;
			s32 srcX1 = 0;
			s32 srcY1 = 0;
			s32 dstX0 = 0;
			s32 dstY0 = 0;
			s32 dstX1 = 0;
			s32 dstY1 = 0;
			bool linearFilter = false;
			bool depthCopy = false;
		};

		struct ReadbackRequest {
			u32 x = 0;
			u32 y = 0;
			u32 width = 0;
			u32 height = 0;
			u32 mipLevel = 0;
		};

	bool init(
		VkPhysicalDevice _physicalDevice,
		VkDevice _device,
		VkCommandPool _commandPool,
		VkQueue _graphicsQueue)
	{
		destroy();
		if (_physicalDevice == VK_NULL_HANDLE || _device == VK_NULL_HANDLE
			|| _commandPool == VK_NULL_HANDLE || _graphicsQueue == VK_NULL_HANDLE) {
			return false;
		}
		m_physicalDevice = _physicalDevice;
		m_device = _device;
		m_commandPool = _commandPool;
		m_graphicsQueue = _graphicsQueue;
		return true;
	}

	void destroy()
	{
		m_physicalDevice = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
		m_commandPool = VK_NULL_HANDLE;
		m_graphicsQueue = VK_NULL_HANDLE;
	}

	bool upload2D(
		VkImage _image,
		VkFormat _imageFormat,
		u32 _mipLevels,
		VkImageLayout & _imageLayout,
		const UploadRequest & _request) const
	{
		if (_image == VK_NULL_HANDLE || _request.data == nullptr || _request.width == 0 || _request.height == 0)
			return false;
		if (_request.mipLevel >= std::max<u32>(1U, _mipLevels))
			return false;

		std::vector<u8> uploadBytes;
		if (!_normalizeUploadData(_request, _imageFormat, uploadBytes) || uploadBytes.empty())
			return false;

		BufferAllocation stagingBuffer{};
		if (!_createBuffer(
			static_cast<VkDeviceSize>(uploadBytes.size()),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer)) {
			return false;
		}

		void * mapped = nullptr;
		if (vkMapMemory(m_device, stagingBuffer.memory, 0, static_cast<VkDeviceSize>(uploadBytes.size()), 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
			_destroyBuffer(stagingBuffer);
			return false;
		}
		std::memcpy(mapped, uploadBytes.data(), uploadBytes.size());
		vkUnmapMemory(m_device, stagingBuffer.memory);

		const VkImageAspectFlags aspectMask = _aspectMaskForFormat(_imageFormat);
		const bool copySubmitted = _runSingleUseCommands([&](VkCommandBuffer _commandBuffer) {
			_transitionImageLayout(
				_commandBuffer,
				_image,
				_imageLayout,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				std::max<u32>(1U, _mipLevels),
				aspectMask);

			VkBufferImageCopy copyRegion{};
			copyRegion.bufferOffset = 0;
			copyRegion.bufferRowLength = 0;
			copyRegion.bufferImageHeight = 0;
			copyRegion.imageSubresource.aspectMask = aspectMask;
			copyRegion.imageSubresource.mipLevel = _request.mipLevel;
			copyRegion.imageSubresource.baseArrayLayer = 0;
			copyRegion.imageSubresource.layerCount = 1;
			copyRegion.imageOffset.x = static_cast<s32>(_request.x);
			copyRegion.imageOffset.y = static_cast<s32>(_request.y);
			copyRegion.imageOffset.z = 0;
			copyRegion.imageExtent.width = _request.width;
			copyRegion.imageExtent.height = _request.height;
			copyRegion.imageExtent.depth = 1;
			vkCmdCopyBufferToImage(
				_commandBuffer,
				stagingBuffer.buffer,
				_image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&copyRegion);

			_transitionImageLayout(
				_commandBuffer,
				_image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				std::max<u32>(1U, _mipLevels),
				aspectMask);
		});

		_destroyBuffer(stagingBuffer);
		if (copySubmitted)
			_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		return copySubmitted;
	}

		void transitionToShaderRead(
			VkImage _image,
			VkFormat _imageFormat,
			u32 _mipLevels,
			VkImageLayout & _imageLayout) const
	{
		if (_image == VK_NULL_HANDLE || _imageLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			return;
		const VkImageAspectFlags aspectMask = _aspectMaskForFormat(_imageFormat);
		const bool transitioned = _runSingleUseCommands([&](VkCommandBuffer _commandBuffer) {
			_transitionImageLayout(
				_commandBuffer,
				_image,
				_imageLayout,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				std::max<u32>(1U, _mipLevels),
				aspectMask);
		});
			if (transitioned)
				_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		bool clearColor(
			VkImage _image,
			VkFormat _imageFormat,
			u32 _mipLevels,
			VkImageLayout & _imageLayout,
			f32 _r,
			f32 _g,
			f32 _b,
			f32 _a) const
		{
			if (_image == VK_NULL_HANDLE)
				return false;
			if (_aspectMaskForFormat(_imageFormat) != VK_IMAGE_ASPECT_COLOR_BIT)
				return false;
			const bool cleared = _runSingleUseCommands([&](VkCommandBuffer _commandBuffer) {
				_transitionImageLayout(
					_commandBuffer,
					_image,
					_imageLayout,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					std::max<u32>(1U, _mipLevels),
					VK_IMAGE_ASPECT_COLOR_BIT);
				VkClearColorValue clearValue{};
				clearValue.float32[0] = _r;
				clearValue.float32[1] = _g;
				clearValue.float32[2] = _b;
				clearValue.float32[3] = _a;
				VkImageSubresourceRange clearRange{};
				clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				clearRange.baseMipLevel = 0;
				clearRange.levelCount = std::max<u32>(1U, _mipLevels);
				clearRange.baseArrayLayer = 0;
				clearRange.layerCount = 1;
				vkCmdClearColorImage(
					_commandBuffer,
					_image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					&clearValue,
					1,
					&clearRange);
				_transitionImageLayout(
					_commandBuffer,
					_image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					std::max<u32>(1U, _mipLevels),
					VK_IMAGE_ASPECT_COLOR_BIT);
			});
			if (cleared)
				_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			return cleared;
		}

		bool clearDepth(
			VkImage _image,
			VkFormat _imageFormat,
			u32 _mipLevels,
			VkImageLayout & _imageLayout,
			f32 _depth) const
		{
			if (_image == VK_NULL_HANDLE)
				return false;
			const VkImageAspectFlags aspectMask = _aspectMaskForFormat(_imageFormat);
			if ((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0U)
				return false;
			const bool cleared = _runSingleUseCommands([&](VkCommandBuffer _commandBuffer) {
				_transitionImageLayout(
					_commandBuffer,
					_image,
					_imageLayout,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					std::max<u32>(1U, _mipLevels),
					aspectMask);
				VkClearDepthStencilValue clearValue{};
				clearValue.depth = _depth;
				clearValue.stencil = 0U;
				VkImageSubresourceRange clearRange{};
				clearRange.aspectMask = aspectMask;
				clearRange.baseMipLevel = 0;
				clearRange.levelCount = std::max<u32>(1U, _mipLevels);
				clearRange.baseArrayLayer = 0;
				clearRange.layerCount = 1;
				vkCmdClearDepthStencilImage(
					_commandBuffer,
					_image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					&clearValue,
					1,
					&clearRange);
				_transitionImageLayout(
					_commandBuffer,
					_image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					std::max<u32>(1U, _mipLevels),
					aspectMask);
			});
			if (cleared)
				_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			return cleared;
		}

		bool blit2D(
			VkImage _srcImage,
			VkFormat _srcFormat,
			u32 _srcMipLevels,
			VkImageLayout & _srcLayout,
			VkImage _dstImage,
			VkFormat _dstFormat,
			u32 _dstMipLevels,
			VkImageLayout & _dstLayout,
			const BlitRequest & _request) const
		{
			if (_srcImage == VK_NULL_HANDLE || _dstImage == VK_NULL_HANDLE)
				return false;
			const VkImageAspectFlags srcAspect = _aspectMaskForFormat(_srcFormat);
			const VkImageAspectFlags dstAspect = _aspectMaskForFormat(_dstFormat);
			if (_request.depthCopy) {
				if ((srcAspect & VK_IMAGE_ASPECT_DEPTH_BIT) == 0U || (dstAspect & VK_IMAGE_ASPECT_DEPTH_BIT) == 0U)
					return false;
			} else {
				if (srcAspect != VK_IMAGE_ASPECT_COLOR_BIT || dstAspect != VK_IMAGE_ASPECT_COLOR_BIT)
					return false;
			}
			const bool submitted = _runSingleUseCommands([&](VkCommandBuffer _commandBuffer) {
				_transitionImageLayout(
					_commandBuffer,
					_srcImage,
					_srcLayout,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					std::max<u32>(1U, _srcMipLevels),
					srcAspect);
				_transitionImageLayout(
					_commandBuffer,
					_dstImage,
					_dstLayout,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					std::max<u32>(1U, _dstMipLevels),
					dstAspect);

				if (_request.depthCopy) {
					const s32 srcMinX = std::min(_request.srcX0, _request.srcX1);
					const s32 srcMinY = std::min(_request.srcY0, _request.srcY1);
					const s32 dstMinX = std::min(_request.dstX0, _request.dstX1);
					const s32 dstMinY = std::min(_request.dstY0, _request.dstY1);
					const s32 srcWidth = std::max(_request.srcX0, _request.srcX1) - srcMinX;
					const s32 srcHeight = std::max(_request.srcY0, _request.srcY1) - srcMinY;
					const s32 dstWidth = std::max(_request.dstX0, _request.dstX1) - dstMinX;
					const s32 dstHeight = std::max(_request.dstY0, _request.dstY1) - dstMinY;
					const s32 copyWidth = std::min(srcWidth, dstWidth);
					const s32 copyHeight = std::min(srcHeight, dstHeight);
					if (copyWidth > 0 && copyHeight > 0) {
						VkImageCopy copyRegion{};
						copyRegion.srcSubresource.aspectMask = srcAspect;
						copyRegion.srcSubresource.mipLevel = 0;
						copyRegion.srcSubresource.baseArrayLayer = 0;
						copyRegion.srcSubresource.layerCount = 1;
						copyRegion.srcOffset = { srcMinX, srcMinY, 0 };
						copyRegion.dstSubresource.aspectMask = dstAspect;
						copyRegion.dstSubresource.mipLevel = 0;
						copyRegion.dstSubresource.baseArrayLayer = 0;
						copyRegion.dstSubresource.layerCount = 1;
						copyRegion.dstOffset = { dstMinX, dstMinY, 0 };
						copyRegion.extent = {
							static_cast<u32>(copyWidth),
							static_cast<u32>(copyHeight),
							1U
						};
						vkCmdCopyImage(
							_commandBuffer,
							_srcImage,
							VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							_dstImage,
							VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							1,
							&copyRegion);
					}
				} else {
					const s32 srcWidth = std::max(_request.srcX0, _request.srcX1) - std::min(_request.srcX0, _request.srcX1);
					const s32 srcHeight = std::max(_request.srcY0, _request.srcY1) - std::min(_request.srcY0, _request.srcY1);
					const s32 dstWidth = std::max(_request.dstX0, _request.dstX1) - std::min(_request.dstX0, _request.dstX1);
					const s32 dstHeight = std::max(_request.dstY0, _request.dstY1) - std::min(_request.dstY0, _request.dstY1);
					if (srcWidth > 0 && srcHeight > 0 && dstWidth > 0 && dstHeight > 0) {
						VkImageBlit blitRegion{};
						blitRegion.srcSubresource.aspectMask = srcAspect;
						blitRegion.srcSubresource.mipLevel = 0;
						blitRegion.srcSubresource.baseArrayLayer = 0;
						blitRegion.srcSubresource.layerCount = 1;
						blitRegion.srcOffsets[0] = { _request.srcX0, _request.srcY0, 0 };
						blitRegion.srcOffsets[1] = { _request.srcX1, _request.srcY1, 1 };
						blitRegion.dstSubresource.aspectMask = dstAspect;
						blitRegion.dstSubresource.mipLevel = 0;
						blitRegion.dstSubresource.baseArrayLayer = 0;
						blitRegion.dstSubresource.layerCount = 1;
						blitRegion.dstOffsets[0] = { _request.dstX0, _request.dstY0, 0 };
						blitRegion.dstOffsets[1] = { _request.dstX1, _request.dstY1, 1 };
						vkCmdBlitImage(
							_commandBuffer,
							_srcImage,
							VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							_dstImage,
							VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							1,
							&blitRegion,
							_request.linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
					}
				}

				_transitionImageLayout(
					_commandBuffer,
					_srcImage,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					std::max<u32>(1U, _srcMipLevels),
					srcAspect);
				_transitionImageLayout(
					_commandBuffer,
					_dstImage,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					std::max<u32>(1U, _dstMipLevels),
					dstAspect);
			});
			if (submitted) {
				_srcLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				_dstLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			}
			return submitted;
		}

		bool readback2D(
			VkImage _image,
			VkFormat _imageFormat,
			u32 _mipLevels,
			VkImageLayout & _imageLayout,
			const ReadbackRequest & _request,
			std::vector<u8> & _outBytes,
			u32 & _outRowBytes,
			u32 & _outBytesPerPixel) const
		{
			_outBytes.clear();
			_outRowBytes = 0U;
			_outBytesPerPixel = 0U;

			if (_image == VK_NULL_HANDLE || _request.width == 0U || _request.height == 0U)
				return false;
			if (_request.mipLevel >= std::max<u32>(1U, _mipLevels))
				return false;

			const VkImageAspectFlags transitionAspect = _aspectMaskForFormat(_imageFormat);
			VkImageAspectFlags copyAspect = transitionAspect;
			if ((copyAspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U)
				copyAspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			else if ((copyAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U)
				copyAspect = VK_IMAGE_ASPECT_STENCIL_BIT;
			else
				copyAspect = VK_IMAGE_ASPECT_COLOR_BIT;

			const u32 bytesPerPixel = _bytesPerPixelForFormat(_imageFormat);
			if (bytesPerPixel == 0U)
				return false;

			size_t rowBytes = 0U;
			if (!_multiplySize(static_cast<size_t>(_request.width), static_cast<size_t>(bytesPerPixel), rowBytes))
				return false;
			size_t totalBytes = 0U;
			if (!_multiplySize(rowBytes, static_cast<size_t>(_request.height), totalBytes))
				return false;
			if (totalBytes == 0U)
				return false;

			BufferAllocation stagingBuffer{};
			if (!_createBuffer(
				static_cast<VkDeviceSize>(totalBytes),
				VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				stagingBuffer)) {
				return false;
			}

			const bool submitted = _runSingleUseCommands([&](VkCommandBuffer _commandBuffer) {
				_transitionImageLayout(
					_commandBuffer,
					_image,
					_imageLayout,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					std::max<u32>(1U, _mipLevels),
					transitionAspect);

				VkBufferImageCopy copyRegion{};
				copyRegion.bufferOffset = 0U;
				copyRegion.bufferRowLength = 0U;
				copyRegion.bufferImageHeight = 0U;
				copyRegion.imageSubresource.aspectMask = copyAspect;
				copyRegion.imageSubresource.mipLevel = _request.mipLevel;
				copyRegion.imageSubresource.baseArrayLayer = 0U;
				copyRegion.imageSubresource.layerCount = 1U;
				copyRegion.imageOffset.x = static_cast<s32>(_request.x);
				copyRegion.imageOffset.y = static_cast<s32>(_request.y);
				copyRegion.imageOffset.z = 0;
				copyRegion.imageExtent.width = _request.width;
				copyRegion.imageExtent.height = _request.height;
				copyRegion.imageExtent.depth = 1U;
				vkCmdCopyImageToBuffer(
					_commandBuffer,
					_image,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					stagingBuffer.buffer,
					1U,
					&copyRegion);

				_transitionImageLayout(
					_commandBuffer,
					_image,
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					std::max<u32>(1U, _mipLevels),
					transitionAspect);
			});
			if (!submitted) {
				_destroyBuffer(stagingBuffer);
				return false;
			}

			void * mapped = nullptr;
			if (vkMapMemory(m_device, stagingBuffer.memory, 0, static_cast<VkDeviceSize>(totalBytes), 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
				_destroyBuffer(stagingBuffer);
				return false;
			}

			_outBytes.resize(totalBytes);
			std::memcpy(_outBytes.data(), mapped, totalBytes);
			vkUnmapMemory(m_device, stagingBuffer.memory);
			_destroyBuffer(stagingBuffer);

			_outRowBytes = static_cast<u32>(rowBytes);
			_outBytesPerPixel = bytesPerPixel;
			_imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			return true;
		}

private:
	struct BufferAllocation {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
	};

	static u32 _componentsForFormat(graphics::ColorFormatParam _format)
	{
		if (_format == graphics::colorFormat::RED
			|| _format == graphics::colorFormat::DEPTH
			|| _format == graphics::colorFormat::LUMINANCE) {
			return 1U;
		}
		if (_format == graphics::colorFormat::RG)
			return 2U;
		if (_format == graphics::colorFormat::RED_GREEN_BLUE)
			return 3U;
		return 4U;
	}

	struct UploadLayout {
		size_t rowSizeBytes = 0;
		size_t srcRowPitch = 0;
		size_t totalBytes = 0;
	};

	static bool _multiplySize(size_t _left, size_t _right, size_t & _out)
	{
		if (_left == 0U || _right == 0U) {
			_out = 0U;
			return true;
		}
		if (_left > (std::numeric_limits<size_t>::max)() / _right)
			return false;
		_out = _left * _right;
		return true;
	}

	static bool _alignSize(size_t _value, size_t _alignment, size_t & _out)
	{
		if (_alignment <= 1U) {
			_out = _value;
			return true;
		}
		const size_t remainder = _value % _alignment;
		if (remainder == 0U) {
			_out = _value;
			return true;
		}
		const size_t padding = _alignment - remainder;
		if (_value > (std::numeric_limits<size_t>::max)() - padding)
			return false;
		_out = _value + padding;
		return true;
	}

	static u32 _bytesPerComponent(graphics::DatatypeParam _type)
	{
		if (_type == graphics::datatype::UNSIGNED_SHORT || _type == graphics::datatype::UNSIGNED_SHORT_4_4_4_4
			|| _type == graphics::datatype::UNSIGNED_SHORT_5_5_5_1 || _type == graphics::datatype::UNSIGNED_SHORT_5_6_5) {
			return 2U;
		}
		if (_type == graphics::datatype::UNSIGNED_INT || _type == graphics::datatype::FLOAT)
			return 4U;
		return 1U;
	}

	static bool _isPackedType(graphics::DatatypeParam _type)
	{
		return _type == graphics::datatype::UNSIGNED_SHORT_4_4_4_4
			|| _type == graphics::datatype::UNSIGNED_SHORT_5_5_5_1
			|| _type == graphics::datatype::UNSIGNED_SHORT_5_6_5;
	}

	static VkImageAspectFlags _aspectMaskForFormat(VkFormat _format)
	{
		switch (_format) {
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT:
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		case VK_FORMAT_S8_UINT:
			return VK_IMAGE_ASPECT_STENCIL_BIT;
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		default:
			return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	static u32 _bytesPerPixelForFormat(VkFormat _format)
	{
		switch (_format) {
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_R8_UINT:
		case VK_FORMAT_S8_UINT:
			return 1U;
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_R16_UNORM:
		case VK_FORMAT_R16_SFLOAT:
		case VK_FORMAT_D16_UNORM:
			return 2U;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT:
			return 4U;
		case VK_FORMAT_R32G32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return 8U;
		default:
			return 0U;
		}
	}

	u32 _findMemoryType(u32 _typeFilter, VkMemoryPropertyFlags _requiredProperties) const
	{
		if (m_physicalDevice == VK_NULL_HANDLE)
			return UINT32_MAX;

		VkPhysicalDeviceMemoryProperties memoryProperties{};
		vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
		for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
			if ((_typeFilter & (1U << i)) == 0U)
				continue;
			if ((memoryProperties.memoryTypes[i].propertyFlags & _requiredProperties) == _requiredProperties)
				return i;
		}
		return UINT32_MAX;
	}

	bool _createBuffer(
		VkDeviceSize _size,
		VkBufferUsageFlags _usage,
		VkMemoryPropertyFlags _properties,
		BufferAllocation & _allocation) const
	{
		if (m_device == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE || _size == 0)
			return false;

		VkBufferCreateInfo bufferCreateInfo{};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.size = _size;
		bufferCreateInfo.usage = _usage;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_device, &bufferCreateInfo, nullptr, &_allocation.buffer) != VK_SUCCESS)
			return false;

		VkMemoryRequirements memoryRequirements{};
		vkGetBufferMemoryRequirements(m_device, _allocation.buffer, &memoryRequirements);
		const u32 memoryTypeIndex = _findMemoryType(memoryRequirements.memoryTypeBits, _properties);
		if (memoryTypeIndex == UINT32_MAX) {
			vkDestroyBuffer(m_device, _allocation.buffer, nullptr);
			_allocation.buffer = VK_NULL_HANDLE;
			return false;
		}

		VkMemoryAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = memoryRequirements.size;
		allocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_device, &allocateInfo, nullptr, &_allocation.memory) != VK_SUCCESS) {
			vkDestroyBuffer(m_device, _allocation.buffer, nullptr);
			_allocation.buffer = VK_NULL_HANDLE;
			return false;
		}

		if (vkBindBufferMemory(m_device, _allocation.buffer, _allocation.memory, 0) != VK_SUCCESS) {
			vkDestroyBuffer(m_device, _allocation.buffer, nullptr);
			vkFreeMemory(m_device, _allocation.memory, nullptr);
			_allocation.buffer = VK_NULL_HANDLE;
			_allocation.memory = VK_NULL_HANDLE;
			return false;
		}
		return true;
	}

	void _destroyBuffer(BufferAllocation & _allocation) const
	{
		if (m_device == VK_NULL_HANDLE)
			return;
		if (_allocation.buffer != VK_NULL_HANDLE) {
			vkDestroyBuffer(m_device, _allocation.buffer, nullptr);
			_allocation.buffer = VK_NULL_HANDLE;
		}
		if (_allocation.memory != VK_NULL_HANDLE) {
			vkFreeMemory(m_device, _allocation.memory, nullptr);
			_allocation.memory = VK_NULL_HANDLE;
		}
	}

	static bool _repackNormalizedByteUpload(
		const UploadRequest & _request,
		VkFormat _dstFormat,
		std::vector<u8> & _bytes)
	{
		if (_request.type != graphics::datatype::UNSIGNED_BYTE)
			return true;
		if (_request.width == 0U || _request.height == 0U)
			return false;

		const u32 srcComponents = _componentsForFormat(_request.format);
		if (srcComponents == 0U)
			return false;
		const size_t srcPixelBytes = static_cast<size_t>(srcComponents);
		const u32 dstBytesPerPixelRaw = _bytesPerPixelForFormat(_dstFormat);
		if (dstBytesPerPixelRaw == 0U)
			return false;
		const size_t dstPixelBytes = static_cast<size_t>(dstBytesPerPixelRaw);
		if (srcPixelBytes == dstPixelBytes)
			return true;

		const size_t srcRowBytes = static_cast<size_t>(_request.width) * srcPixelBytes;
		const size_t expectedSrcBytes = srcRowBytes * static_cast<size_t>(_request.height);
		if (_bytes.size() != expectedSrcBytes)
			return false;

		const size_t dstRowBytes = static_cast<size_t>(_request.width) * dstPixelBytes;
		const size_t dstTotalBytes = dstRowBytes * static_cast<size_t>(_request.height);
		std::vector<u8> repacked(dstTotalBytes, 0U);

		auto decodeSource = [&](const u8 * _src, u8 & _r, u8 & _g, u8 & _b, u8 & _a) {
			_r = 0U;
			_g = 0U;
			_b = 0U;
			_a = 255U;
			if (_request.format == graphics::colorFormat::RED) {
				_r = _src[0];
				return;
			}
			if (_request.format == graphics::colorFormat::LUMINANCE) {
				_r = _src[0];
				_g = _src[0];
				_b = _src[0];
				return;
			}
			if (_request.format == graphics::colorFormat::RG) {
				_r = _src[0];
				_g = _src[1];
				return;
			}
			if (_request.format == graphics::colorFormat::RED_GREEN_BLUE) {
				_r = _src[0];
				_g = _src[1];
				_b = _src[2];
				return;
			}
			_r = _src[0];
			_g = srcComponents > 1U ? _src[1] : 0U;
			_b = srcComponents > 2U ? _src[2] : 0U;
			_a = srcComponents > 3U ? _src[3] : 255U;
		};

		for (u32 y = 0; y < _request.height; ++y) {
			const u8 * srcRow = _bytes.data() + srcRowBytes * static_cast<size_t>(y);
			u8 * dstRow = repacked.data() + dstRowBytes * static_cast<size_t>(y);
			for (u32 x = 0; x < _request.width; ++x) {
				const u8 * srcPixel = srcRow + static_cast<size_t>(x) * srcPixelBytes;
				u8 * dstPixel = dstRow + static_cast<size_t>(x) * dstPixelBytes;
				u8 r = 0U;
				u8 g = 0U;
				u8 b = 0U;
				u8 a = 255U;
				decodeSource(srcPixel, r, g, b, a);

				switch (_dstFormat) {
				case VK_FORMAT_R8_UNORM:
				case VK_FORMAT_R8_UINT:
					dstPixel[0] = r;
					break;
				case VK_FORMAT_R8G8_UNORM:
					dstPixel[0] = r;
					dstPixel[1] = g;
					break;
				case VK_FORMAT_R8G8B8A8_UNORM:
				case VK_FORMAT_R8G8B8A8_SRGB:
					dstPixel[0] = r;
					dstPixel[1] = g;
					dstPixel[2] = b;
					dstPixel[3] = a;
					break;
				case VK_FORMAT_B8G8R8A8_UNORM:
				case VK_FORMAT_B8G8R8A8_SRGB:
					dstPixel[0] = b;
					dstPixel[1] = g;
					dstPixel[2] = r;
					dstPixel[3] = a;
					break;
				default:
					return false;
				}
			}
		}

		_bytes.swap(repacked);
		return true;
	}

	bool _normalizeUploadData(const UploadRequest & _request, VkFormat _dstFormat, std::vector<u8> & _outBytes) const
	{
		if (_request.data == nullptr || _request.width == 0 || _request.height == 0)
			return false;

		const size_t unpackAlignment = static_cast<size_t>(std::max<s32>(1, _request.unpackAlignment));
		const bool packedType = _isPackedType(_request.type);
		if (packedType) {
			UploadLayout srcLayout{};
			if (!_multiplySize(static_cast<size_t>(_request.width), static_cast<size_t>(2U), srcLayout.rowSizeBytes))
				return false;
			if (!_alignSize(srcLayout.rowSizeBytes, unpackAlignment, srcLayout.srcRowPitch))
				return false;
			if (!_multiplySize(srcLayout.rowSizeBytes, static_cast<size_t>(_request.height), srcLayout.totalBytes))
				return false;

			UploadLayout dstLayout{};
			if (!_multiplySize(static_cast<size_t>(_request.width), static_cast<size_t>(4U), dstLayout.rowSizeBytes))
				return false;
			if (!_multiplySize(dstLayout.rowSizeBytes, static_cast<size_t>(_request.height), dstLayout.totalBytes))
				return false;
			if (dstLayout.totalBytes == 0U)
				return false;
			_outBytes.resize(dstLayout.totalBytes);

			auto expand5To8 = [](u32 _v) -> u8 {
				return static_cast<u8>((_v << 3U) | (_v >> 2U));
			};
			auto expand6To8 = [](u32 _v) -> u8 {
				return static_cast<u8>((_v << 2U) | (_v >> 4U));
			};
			auto expand4To8 = [](u32 _v) -> u8 {
				return static_cast<u8>((_v << 4U) | _v);
			};

			const u8 * src = static_cast<const u8 *>(_request.data);
			for (u32 y = 0; y < _request.height; ++y) {
				const u8 * srcRow = src + srcLayout.srcRowPitch * y;
				u8 * dstRow = _outBytes.data() + dstLayout.rowSizeBytes * y;
				for (u32 x = 0; x < _request.width; ++x) {
					const u16 packed = static_cast<u16>(srcRow[x * 2U]) | (static_cast<u16>(srcRow[x * 2U + 1U]) << 8U);
					u8 r = 0U;
					u8 g = 0U;
					u8 b = 0U;
					u8 a = 255U;
					if (_request.type == graphics::datatype::UNSIGNED_SHORT_5_6_5) {
						r = expand5To8((packed >> 11U) & 0x1FU);
						g = expand6To8((packed >> 5U) & 0x3FU);
						b = expand5To8(packed & 0x1FU);
						a = 255U;
					} else if (_request.type == graphics::datatype::UNSIGNED_SHORT_5_5_5_1) {
						r = expand5To8((packed >> 11U) & 0x1FU);
						g = expand5To8((packed >> 6U) & 0x1FU);
						b = expand5To8((packed >> 1U) & 0x1FU);
						a = (packed & 0x1U) != 0U ? 255U : 0U;
					} else {
						r = expand4To8((packed >> 12U) & 0x0FU);
						g = expand4To8((packed >> 8U) & 0x0FU);
						b = expand4To8((packed >> 4U) & 0x0FU);
						a = expand4To8(packed & 0x0FU);
					}
					const size_t dstIndex = static_cast<size_t>(x) * 4U;
					dstRow[dstIndex + 0U] = r;
					dstRow[dstIndex + 1U] = g;
					dstRow[dstIndex + 2U] = b;
					dstRow[dstIndex + 3U] = a;
				}
			}
			return _repackNormalizedByteUpload(_request, _dstFormat, _outBytes);
		}

		const u32 components = _componentsForFormat(_request.format);
		const u32 pixelSize = components * _bytesPerComponent(_request.type);
		if (pixelSize == 0U)
			return false;

		UploadLayout layout{};
		if (!_multiplySize(static_cast<size_t>(_request.width), static_cast<size_t>(pixelSize), layout.rowSizeBytes))
			return false;
		if (!_alignSize(layout.rowSizeBytes, unpackAlignment, layout.srcRowPitch))
			return false;
		if (!_multiplySize(layout.rowSizeBytes, static_cast<size_t>(_request.height), layout.totalBytes))
			return false;
		if (layout.totalBytes == 0U)
			return false;

		_outBytes.resize(layout.totalBytes);
		const u8 * src = static_cast<const u8 *>(_request.data);
		if (layout.rowSizeBytes == layout.srcRowPitch) {
			std::memcpy(_outBytes.data(), src, layout.totalBytes);
			return _repackNormalizedByteUpload(_request, _dstFormat, _outBytes);
		}
		for (u32 y = 0; y < _request.height; ++y) {
			std::memcpy(
				_outBytes.data() + layout.rowSizeBytes * y,
				src + layout.srcRowPitch * y,
				layout.rowSizeBytes);
		}
		return _repackNormalizedByteUpload(_request, _dstFormat, _outBytes);
	}

	bool _runSingleUseCommands(const std::function<void(VkCommandBuffer)> & _record) const
	{
		if (m_device == VK_NULL_HANDLE || m_commandPool == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE)
			return false;

		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkCommandBufferAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocateInfo.commandPool = m_commandPool;
		allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocateInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_device, &allocateInfo, &commandBuffer) != VK_SUCCESS)
			return false;

		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
			vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
			return false;
		}

		_record(commandBuffer);

		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
			return false;
		}

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		const VkResult submitResult = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
		if (submitResult != VK_SUCCESS) {
			vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
			return false;
		}
		vkQueueWaitIdle(m_graphicsQueue);
		vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
		return true;
	}

		void _transitionImageLayout(
			VkCommandBuffer _commandBuffer,
			VkImage _image,
			VkImageLayout _oldLayout,
		VkImageLayout _newLayout,
		u32 _mipLevelCount,
			VkImageAspectFlags _aspectMask) const
		{
			if (_oldLayout == _newLayout)
				return;
			VkImageMemoryBarrier imageBarrier{};
			imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imageBarrier.oldLayout = _oldLayout;
		imageBarrier.newLayout = _newLayout;
		imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imageBarrier.image = _image;
		imageBarrier.subresourceRange.aspectMask = _aspectMask;
		imageBarrier.subresourceRange.baseMipLevel = 0;
		imageBarrier.subresourceRange.levelCount = _mipLevelCount;
		imageBarrier.subresourceRange.baseArrayLayer = 0;
		imageBarrier.subresourceRange.layerCount = 1;
		imageBarrier.srcAccessMask = 0;
		imageBarrier.dstAccessMask = 0;

		VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

			if (_oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && _newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
				imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && _newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
				imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && _newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
				imageBarrier.srcAccessMask = 0;
				imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && _newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
				imageBarrier.srcAccessMask = 0;
				imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && _newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && _newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && _newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
				imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && _newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
				imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			} else if (_oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && _newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				imageBarrier.srcAccessMask = 0;
				imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			} else {
				imageBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				imageBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			}

		vkCmdPipelineBarrier(
			_commandBuffer,
			sourceStage,
			destinationStage,
			0,
			0,
			nullptr,
			0,
			nullptr,
			1,
			&imageBarrier);
	}

	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VkCommandPool m_commandPool = VK_NULL_HANDLE;
	VkQueue m_graphicsQueue = VK_NULL_HANDLE;
};
#else
class TextureUploader
{
};
#endif

} // namespace vulkan
