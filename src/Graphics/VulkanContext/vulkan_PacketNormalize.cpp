#include "vulkan_PacketNormalize.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <GBI.h>
#include <Log.h>
#include <gDP.h>
#include <gSP.h>

namespace vulkan {
namespace packet_normalize {

namespace {

void applyLegacyScreenTransform(f32 & _x, f32 & _y, f32 _w)
{
	constexpr f32 kHalfScreenSize = 320.0f;
	_x -= kHalfScreenSize * _w;
	_y -= kHalfScreenSize * _w;
	_x /= kHalfScreenSize;
	_y /= kHalfScreenSize;
}

bool normalizeVertexWithRasterViewport(
	const vulkan::RasterState & _raster,
	const vulkan::DrawVertex & _src,
	vulkan::DrawVertex & _dst)
{
	if (!_raster.viewportValid || _raster.viewportWidth == 0 || _raster.viewportHeight == 0)
		return false;

	const f32 viewportX = static_cast<f32>(_raster.viewportX);
	const f32 viewportY = static_cast<f32>(_raster.viewportY);
	const f32 viewportWidth = std::max(1.0f, static_cast<f32>(std::abs(_raster.viewportWidth)));
	const f32 viewportHeight = std::max(1.0f, static_cast<f32>(std::abs(_raster.viewportHeight)));

	_dst.x = ((_src.x - viewportX) / viewportWidth) * 2.0f - 1.0f;
	_dst.y = ((_src.y - viewportY) / viewportHeight) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

bool normalizeRectWithGspViewport(const vulkan::DrawVertex & _src, vulkan::DrawVertex & _dst)
{
	const f32 viewportWidth = std::abs(gSP.viewport.width);
	const f32 viewportHeight = std::abs(gSP.viewport.height);
	if (viewportWidth < 0.5f || viewportHeight < 0.5f)
		return false;

	_dst.x = ((_src.x - gSP.viewport.x) / viewportWidth) * 2.0f - 1.0f;
	_dst.y = ((_src.y - gSP.viewport.y) / viewportHeight) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

bool normalizeRectWithDpScissor(const vulkan::DrawVertex & _src, vulkan::DrawVertex & _dst)
{
	f32 left = gDP.scissor.ulx;
	f32 top = gDP.scissor.uly;
	f32 width = std::abs(gDP.scissor.lrx - gDP.scissor.ulx);
	f32 height = std::abs(gDP.scissor.lry - gDP.scissor.uly);

	// Fill-cycle rect coordinates often live in color-image space (for example 320x240)
	// even when current scissor reports a much smaller UI viewport. Prefer color-image
	// normalization when scissor extents are implausibly small.
	f32 imageWidth = static_cast<f32>(gDP.colorImage.width);
	if (imageWidth < 0.5f)
		imageWidth = 320.0f;
	f32 imageHeight = imageWidth * 0.75f;
	if (imageHeight < 0.5f)
		imageHeight = 240.0f;

	const bool preferImageSpace =
		width < 0.5f
		|| height < 0.5f
		|| width < imageWidth * 0.5f
		|| height < imageHeight * 0.5f;
	if (preferImageSpace) {
		left = 0.0f;
		top = 0.0f;
		width = imageWidth;
		height = imageHeight;
	}

	_dst.x = ((_src.x - left) / width) * 2.0f - 1.0f;
	_dst.y = ((_src.y - top) / height) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

bool normalizeRectWithColorImageSpace(const vulkan::DrawVertex & _src, vulkan::DrawVertex & _dst)
{
	f32 width = static_cast<f32>(gDP.colorImage.width);
	if (width < 0.5f)
		width = 320.0f;
	f32 height = width * 0.75f;
	if (height < 0.5f)
		height = 240.0f;

	_dst.x = (_src.x / width) * 2.0f - 1.0f;
	_dst.y = (_src.y / height) * 2.0f - 1.0f;
	_dst.w = 1.0f;
	return true;
}

} // namespace

vulkan::DrawVertex normalizePacketVertex(const vulkan::DrawPacket & _packet, const vulkan::DrawVertex & _src)
{
	static const bool disablePositionNormalize = std::getenv("REALITYVK_VK_DISABLE_POSITION_NORMALIZE") != nullptr;
	static const bool disableForceRasterRectTransform = std::getenv("REALITYVK_VK_DISABLE_FORCE_RASTER_RECT_TRANSFORM") != nullptr;
	static const bool debugRectNormalize = std::getenv("REALITYVK_VK_DEBUG_RECT_NORMALIZE") != nullptr;
	static const u32 debugRectNormalizeLimit = []() -> u32 {
		const char * env = std::getenv("REALITYVK_VK_DEBUG_RECT_NORMALIZE_LIMIT");
		if (env == nullptr || env[0] == '\0')
			return 96U;
		return static_cast<u32>(std::strtoul(env, nullptr, 10));
	}();
	static u32 debugRectNormalizeCount = 0U;
	if (disablePositionNormalize)
		return _src;

	vulkan::DrawVertex dst = _src;
	const bool forceRasterRectTransform = _packet.forceRasterRectTransform && !disableForceRasterRectTransform;
	const bool clipLikeVertex = std::abs(_src.x) <= 2.0f && std::abs(_src.y) <= 2.0f && std::abs(_src.w) <= 2.0f;
	const bool trustClipLikeVertex = _packet.positionsNormalized || !forceRasterRectTransform;
	const bool fillCycleRectNeedsNormalization =
		_packet.transformMode == vulkan::VertexTransformMode::Rect
		&& !_packet.positionsNormalized
		&& !_packet.debugTexrect
		&& _packet.debugCombinerCycleType == G_CYC_FILL
		&& _packet.textureSlotMask == 0U;
	if (clipLikeVertex && trustClipLikeVertex && !fillCycleRectNeedsNormalization)
		return dst;

	// Some paths feed homogeneous coordinates with large w and clip-space-scaled x/y.
	// Recover NDC first so triangle-mode screen transform does not explode them off-screen.
	const f32 absW = std::abs(_src.w);
	if (absW > 2.0f) {
		const f32 conservativeW = absW * 1.25f;
		if (std::abs(_src.x) <= conservativeW && std::abs(_src.y) <= conservativeW) {
			const f32 invW = 1.0f / _src.w;
			dst.x = _src.x * invW;
			dst.y = _src.y * invW;
			dst.z = _src.z * invW;
			dst.w = 1.0f;
			return dst;
		}
	}

	if (_packet.transformMode == vulkan::VertexTransformMode::Triangle) {
		const bool modifyXY = (_src.modify & MODIFY_XY) != 0U;
		const bool modifyZ = (_src.modify & MODIFY_Z) != 0U;
		if (modifyXY) {
			dst.x = _src.x * _src.w;
			dst.y = _src.y * _src.w;
		} else {
			dst.x = _src.x * gSP.viewport.vscale[0] + gSP.viewport.vtrans[0] * _src.w;
			dst.y = _src.y * (-gSP.viewport.vscale[1]) + gSP.viewport.vtrans[1] * _src.w;
			dst.x = std::floor(dst.x * 4.0f) * 0.25f;
			dst.y = std::floor(dst.y * 4.0f) * 0.25f;
		}
		if (modifyZ)
			dst.z = _src.z * _src.w;
		applyLegacyScreenTransform(dst.x, dst.y, _src.w);
		// Legacy triangle transform can occasionally explode when input vertices
		// are already close to window/clip space; fall back to viewport remap.
		if (std::abs(dst.x) <= 8.0f && std::abs(dst.y) <= 8.0f)
			return dst;
		dst = _src;
	}

	if (_packet.transformMode == vulkan::VertexTransformMode::Rect) {
		const bool fillCycleRect = !_packet.debugTexrect && _packet.debugCombinerCycleType == G_CYC_FILL;
		if (fillCycleRect) {
			f32 imageWidth = static_cast<f32>(gDP.colorImage.width);
			if (imageWidth < 0.5f)
				imageWidth = 320.0f;
			f32 imageHeight = imageWidth * 0.75f;
			if (imageHeight < 0.5f)
				imageHeight = 240.0f;

			if (!_packet.vertices.empty()) {
				f32 minX = _packet.vertices[0].x;
				f32 maxX = minX;
				f32 minY = _packet.vertices[0].y;
				f32 maxY = minY;
				for (const vulkan::DrawVertex & packetVertex : _packet.vertices) {
					minX = std::min(minX, packetVertex.x);
					maxX = std::max(maxX, packetVertex.x);
					minY = std::min(minY, packetVertex.y);
					maxY = std::max(maxY, packetVertex.y);
				}
				const f32 spanX = maxX - minX;
				const f32 spanY = maxY - minY;
				const bool fullWidthThinBand = spanX >= imageWidth * 0.9f && spanY <= imageHeight * 0.25f;
				if (fullWidthThinBand && normalizeRectWithColorImageSpace(_src, dst)) {
					if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
						LOG(LOG_WARNING, "VK rect normalize debug: branch=color-image-fill-band result=[%0.3f,%0.3f]", dst.x, dst.y);
						++debugRectNormalizeCount;
					}
					return dst;
				}
			}

			if (normalizeRectWithDpScissor(_src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=dp-scissor-fill result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
		}

		if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
			LOG(
				LOG_WARNING,
					"VK rect normalize debug: forceRaster=%u src=[%0.3f,%0.3f] rasterViewport=%d,%d,%d,%d valid=%u gspViewport=[x=%0.3f y=%0.3f w=%0.3f h=%0.3f]",
					forceRasterRectTransform ? 1U : 0U,
					_src.x,
					_src.y,
				_packet.state.raster.viewportX,
				_packet.state.raster.viewportY,
				_packet.state.raster.viewportWidth,
				_packet.state.raster.viewportHeight,
				_packet.state.raster.viewportValid ? 1U : 0U,
				gSP.viewport.x,
				gSP.viewport.y,
				gSP.viewport.width,
				gSP.viewport.height);
		}
			if (forceRasterRectTransform) {
			if (normalizeVertexWithRasterViewport(_packet.state.raster, _src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=raster result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
				if (normalizeRectWithGspViewport(_src, dst)) {
					if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
						LOG(LOG_WARNING, "VK rect normalize debug: branch=gsp-secondary result=[%0.3f,%0.3f]", dst.x, dst.y);
						++debugRectNormalizeCount;
					}
					return dst;
				}
				applyLegacyScreenTransform(dst.x, dst.y, _src.w);
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=screen-transform-secondary result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
			// Rect vertices usually arrive in viewport space and should be normalized
			// against the active RSP viewport instead of a fixed 640x640 basis.
			if (normalizeRectWithGspViewport(_src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=gsp result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
			return dst;
			}
			if (normalizeVertexWithRasterViewport(_packet.state.raster, _src, dst)) {
				if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
					LOG(LOG_WARNING, "VK rect normalize debug: branch=raster-secondary result=[%0.3f,%0.3f]", dst.x, dst.y);
					++debugRectNormalizeCount;
				}
				return dst;
			}
			applyLegacyScreenTransform(dst.x, dst.y, _src.w);
			if (debugRectNormalize && debugRectNormalizeCount < debugRectNormalizeLimit) {
				LOG(LOG_WARNING, "VK rect normalize debug: branch=screen-transform result=[%0.3f,%0.3f]", dst.x, dst.y);
				++debugRectNormalizeCount;
			}
			return dst;
		}

	if (!normalizeVertexWithRasterViewport(_packet.state.raster, _src, dst))
		return dst;
	return dst;
}

void normalizePacketPositions(vulkan::DrawPacket & _packet)
{
	static const bool debugPositions = std::getenv("REALITYVK_VK_DEBUG_POSITIONS") != nullptr;
	f32 preMinX = 0.0f;
	f32 preMaxX = 0.0f;
	f32 preMinY = 0.0f;
	f32 preMaxY = 0.0f;
	if (debugPositions && !_packet.vertices.empty()) {
		preMinX = preMaxX = _packet.vertices[0].x;
		preMinY = preMaxY = _packet.vertices[0].y;
		for (const vulkan::DrawVertex & vertex : _packet.vertices) {
			preMinX = std::min(preMinX, vertex.x);
			preMaxX = std::max(preMaxX, vertex.x);
			preMinY = std::min(preMinY, vertex.y);
			preMaxY = std::max(preMaxY, vertex.y);
		}
	}

	for (vulkan::DrawVertex & vertex : _packet.vertices)
		vertex = normalizePacketVertex(_packet, vertex);
	_packet.positionsNormalized = true;

	if (debugPositions && !_packet.vertices.empty()) {
		static const u32 positionLogLimit = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_POSITIONS_LIMIT");
			if (env == nullptr || env[0] == '\0')
				return 128U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		static u32 positionLogCount = 0U;
		if (positionLogCount < positionLogLimit) {
			f32 postMinX = _packet.vertices[0].x;
			f32 postMaxX = postMinX;
			f32 postMinY = _packet.vertices[0].y;
			f32 postMaxY = postMinY;
			for (const vulkan::DrawVertex & vertex : _packet.vertices) {
				postMinX = std::min(postMinX, vertex.x);
				postMaxX = std::max(postMaxX, vertex.x);
				postMinY = std::min(postMinY, vertex.y);
				postMaxY = std::max(postMaxY, vertex.y);
			}
			LOG(
				LOG_WARNING,
				"VK position debug: pre=[x=%0.3f..%0.3f y=%0.3f..%0.3f] post=[x=%0.3f..%0.3f y=%0.3f..%0.3f] viewport=%d,%d,%d,%d valid=%u scissor=%d,%d,%d,%d enabled=%u",
				preMinX,
				preMaxX,
				preMinY,
				preMaxY,
				postMinX,
				postMaxX,
				postMinY,
				postMaxY,
				_packet.state.raster.viewportX,
				_packet.state.raster.viewportY,
				_packet.state.raster.viewportWidth,
				_packet.state.raster.viewportHeight,
				_packet.state.raster.viewportValid ? 1U : 0U,
				_packet.state.raster.scissorX,
				_packet.state.raster.scissorY,
				_packet.state.raster.scissorWidth,
				_packet.state.raster.scissorHeight,
				_packet.state.raster.scissorEnabled ? 1U : 0U);
			++positionLogCount;
		}
	}
}

} // namespace packet_normalize
} // namespace vulkan
