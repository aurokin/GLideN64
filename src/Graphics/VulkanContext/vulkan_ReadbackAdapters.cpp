#include "vulkan_ReadbackAdapters.h"

#include <algorithm>
#include <cstring>

namespace vulkan {
namespace readback {

namespace {

class VulkanPixelReadBuffer final : public graphics::PixelReadBuffer
{
public:
	VulkanPixelReadBuffer(size_t _sizeInBytes, const PixelReadFn & _readPixelsFn)
		: m_readPixelsFn(_readPixelsFn)
		, m_data(_sizeInBytes, 0U)
	{
	}

	void readPixels(s32 _x, s32 _y, u32 _width, u32 _height, graphics::Parameter _format, graphics::Parameter _type) override
	{
		std::vector<u8> readbackBytes;
		if (m_readPixelsFn == nullptr || !m_readPixelsFn(_x, _y, _width, _height, _format, _type, readbackBytes)) {
			std::fill(m_data.begin(), m_data.end(), 0U);
			return;
		}
		const size_t copyBytes = std::min(m_data.size(), readbackBytes.size());
		if (copyBytes > 0U)
			std::memcpy(m_data.data(), readbackBytes.data(), copyBytes);
		if (copyBytes < m_data.size())
			std::fill_n(m_data.data() + copyBytes, m_data.size() - copyBytes, 0U);
	}

	void * getDataRange(u32 _offset, u32 _range) override
	{
		if (_offset >= m_data.size())
			return nullptr;
		const size_t maxRange = m_data.size() - _offset;
		if (_range > maxRange)
			_range = static_cast<u32>(maxRange);
		return m_data.data() + _offset;
	}

	void closeReadBuffer() override {}
	void bind() override {}
	void unbind() override {}

private:
	PixelReadFn m_readPixelsFn;
	std::vector<u8> m_data;
};

class VulkanColorBufferReader final : public graphics::ColorBufferReader
{
public:
	VulkanColorBufferReader(CachedTexture * _pTexture, const ReadTextureFn & _readTextureFn)
		: ColorBufferReader(_pTexture)
		, m_readTextureFn(_readTextureFn)
	{
	}

	void cleanUp() override
	{
		m_readbackData.clear();
	}

private:
	const u8 * _readPixels(const ReadColorBufferParams & _params, u32 & _heightOffset, u32 & _stride) override
	{
		_heightOffset = 0;
		_stride = _params.width;
		m_readbackData.clear();
		if (m_readTextureFn == nullptr)
			return nullptr;

		u32 stridePixels = 0U;
		const bool readOk = m_readTextureFn(
			m_pTexture->name,
			_params.x0,
			_params.y0,
			_params.width,
			_params.height,
			_params.colorFormat,
			_params.colorType,
			m_readbackData,
			stridePixels);
		if (!readOk || m_readbackData.empty())
			return nullptr;

		if (stridePixels != 0U)
			_stride = stridePixels;
		return m_readbackData.data();
	}

	ReadTextureFn m_readTextureFn;
	std::vector<u8> m_readbackData;
};

} // namespace

graphics::PixelReadBuffer * createPixelReadBuffer(size_t _sizeInBytes, const PixelReadFn & _readPixelsFn)
{
	return new VulkanPixelReadBuffer(_sizeInBytes, _readPixelsFn);
}

graphics::ColorBufferReader * createColorBufferReader(CachedTexture * _pTexture, const ReadTextureFn & _readTextureFn)
{
	if (_pTexture == nullptr)
		return nullptr;
	return new VulkanColorBufferReader(_pTexture, _readTextureFn);
}

} // namespace readback
} // namespace vulkan
