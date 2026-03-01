#pragma once

#include <functional>
#include <vector>

#include <Graphics/ColorBufferReader.h>
#include <Graphics/ObjectHandle.h>
#include <Graphics/PixelBuffer.h>

namespace vulkan {
namespace readback {

using PixelReadFn = std::function<bool(
	s32,
	s32,
	u32,
	u32,
	graphics::Parameter,
	graphics::Parameter,
	std::vector<u8> &)>;

using ReadTextureFn = std::function<bool(
	graphics::ObjectHandle,
	s32,
	s32,
	u32,
	u32,
	graphics::Parameter,
	graphics::Parameter,
	std::vector<u8> &,
	u32 &)>;

graphics::PixelReadBuffer * createPixelReadBuffer(size_t _sizeInBytes, const PixelReadFn & _readPixelsFn);

graphics::ColorBufferReader * createColorBufferReader(CachedTexture * _pTexture, const ReadTextureFn & _readTextureFn);

} // namespace readback
} // namespace vulkan
