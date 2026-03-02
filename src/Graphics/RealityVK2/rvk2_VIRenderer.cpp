#include "rvk2_VIRenderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;

inline u32 clampU32(u32 _value, u32 _minimum, u32 _maximum)
{
	if (_value < _minimum)
		return _minimum;
	if (_value > _maximum)
		return _maximum;
	return _value;
}

inline void updateHashByte(u64 & _hash, u8 _value)
{
	_hash ^= static_cast<u64>(_value);
	_hash *= kFnvPrime;
}

inline bool parseAspect(const char * _text, u8 & _x, u8 & _y)
{
	if (_text == nullptr || _text[0] == '\0')
		return false;
	unsigned long x = 0UL;
	unsigned long y = 0UL;
	if (std::sscanf(_text, "%lu:%lu", &x, &y) != 2)
		return false;
	if (x == 0UL || y == 0UL || x > 255UL || y > 255UL)
		return false;
	_x = static_cast<u8>(x);
	_y = static_cast<u8>(y);
	return true;
}

inline size_t pixelIndex(u32 _width, u32 _x, u32 _y)
{
	return static_cast<size_t>(_y) * static_cast<size_t>(_width) + static_cast<size_t>(_x);
}

} // namespace

namespace rvk2 {

VIRendererConfig loadVIRendererConfigFromEnv()
{
	VIRendererConfig config{};
	u8 aspectX = config.aspectX;
	u8 aspectY = config.aspectY;
	if (parseAspect(std::getenv("REALITYVK2_VI_ASPECT"), aspectX, aspectY)) {
		config.aspectX = aspectX;
		config.aspectY = aspectY;
	}
	return config;
}

VIRenderer::VIRenderer(const VIRendererConfig & _config)
	: m_config(_config)
{
}

VIFrameSummary VIRenderer::present(
	const VIFrameInput & _input,
	std::vector<u32> * _outputPixels) const
{
	VIFrameSummary summary{};
	const u32 aspectX = m_config.aspectX == 0U ? 1U : m_config.aspectX;
	const u32 aspectY = m_config.aspectY == 0U ? 1U : m_config.aspectY;
	summary.aspectX = static_cast<u8>(aspectX);
	summary.aspectY = static_cast<u8>(aspectY);

	if (_input.sourceWidth == 0U
		|| _input.sourceHeight == 0U
		|| _input.sourcePixels == nullptr
		|| _input.sourcePixels->empty()) {
		return summary;
	}

	const size_t requiredPixels =
		static_cast<size_t>(_input.sourceWidth) * static_cast<size_t>(_input.sourceHeight);
	if (_input.sourcePixels->size() < requiredPixels)
		return summary;

	u32 outputWidth = _input.sourceWidth;
	u32 outputHeight = _input.sourceHeight;
	const u64 sourceScaled = static_cast<u64>(_input.sourceWidth) * static_cast<u64>(aspectY);
	const u64 targetScaled = static_cast<u64>(_input.sourceHeight) * static_cast<u64>(aspectX);
	if (sourceScaled > targetScaled) {
		outputHeight = static_cast<u32>(
			(static_cast<u64>(_input.sourceWidth) * static_cast<u64>(aspectY)
				+ static_cast<u64>(aspectX) - 1ULL)
			/ static_cast<u64>(aspectX));
	}
	else if (sourceScaled < targetScaled) {
		outputWidth = static_cast<u32>(
			(static_cast<u64>(_input.sourceHeight) * static_cast<u64>(aspectX)
				+ static_cast<u64>(aspectY) - 1ULL)
			/ static_cast<u64>(aspectY));
	}

	outputWidth = clampU32(outputWidth, 1U, std::max<u32>(1U, m_config.maxOutputWidth));
	outputHeight = clampU32(outputHeight, 1U, std::max<u32>(1U, m_config.maxOutputHeight));

	u32 contentWidth = outputWidth;
	u32 contentHeight = outputHeight;
	const u64 contentSourceScaled = static_cast<u64>(_input.sourceWidth) * static_cast<u64>(outputHeight);
	const u64 contentOutputScaled = static_cast<u64>(_input.sourceHeight) * static_cast<u64>(outputWidth);
	if (contentSourceScaled > contentOutputScaled) {
		contentHeight = static_cast<u32>(
			(std::max<u64>(1ULL,
				static_cast<u64>(outputWidth) * static_cast<u64>(_input.sourceHeight)))
			/ static_cast<u64>(_input.sourceWidth));
		contentHeight = std::max<u32>(1U, std::min<u32>(contentHeight, outputHeight));
	}
	else if (contentSourceScaled < contentOutputScaled) {
		contentWidth = static_cast<u32>(
			(std::max<u64>(1ULL,
				static_cast<u64>(outputHeight) * static_cast<u64>(_input.sourceWidth)))
			/ static_cast<u64>(_input.sourceHeight));
		contentWidth = std::max<u32>(1U, std::min<u32>(contentWidth, outputWidth));
	}

	const u32 contentX = (outputWidth - contentWidth) / 2U;
	const u32 contentY = (outputHeight - contentHeight) / 2U;

	summary.presentWidth = outputWidth;
	summary.presentHeight = outputHeight;
	summary.contentX = contentX;
	summary.contentY = contentY;
	summary.contentWidth = contentWidth;
	summary.contentHeight = contentHeight;
	if (_outputPixels != nullptr)
		_outputPixels->assign(static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight), 0U);

	u64 hash = kFnvOffset;
	for (u32 y = 0U; y < outputHeight; ++y) {
		for (u32 x = 0U; x < outputWidth; ++x) {
			u32 pixel = 0U;
				if (x >= contentX
					&& x < contentX + contentWidth
					&& y >= contentY
					&& y < contentY + contentHeight) {
					const u32 contentXLocal = x - contentX;
					const u32 contentYLocal = y - contentY;
					const u32 sourceX = std::min<u32>(
						_input.sourceWidth - 1U,
						(contentXLocal * static_cast<u32>(_input.sourceWidth)) / contentWidth);
					const u32 sourceY = std::min<u32>(
						_input.sourceHeight - 1U,
						(contentYLocal * static_cast<u32>(_input.sourceHeight)) / contentHeight);
					pixel = (*_input.sourcePixels)[pixelIndex(_input.sourceWidth, sourceX, sourceY)];
				}
				if (_outputPixels != nullptr)
					(*_outputPixels)[pixelIndex(outputWidth, x, y)] = pixel;
				updateHashByte(hash, static_cast<u8>((pixel >> 0U) & 0xFFU));
				updateHashByte(hash, static_cast<u8>((pixel >> 8U) & 0xFFU));
				updateHashByte(hash, static_cast<u8>((pixel >> 16U) & 0xFFU));
				updateHashByte(hash, static_cast<u8>((pixel >> 24U) & 0xFFU));
			}
		}
	summary.presentHash = hash;
	return summary;
}

} // namespace rvk2
