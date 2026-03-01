#include "vulkan_PacketBuilder.h"

#include <algorithm>

#include <gDP.h>

#include "vulkan_CombinerHeuristics.h"

namespace vulkan {
namespace packet_builder {

void initializeTrianglePacket(vulkan::PrimitiveType _primitive, const graphics::CombinerProgram * _combiner, vulkan::DrawPacket & _packet)
{
	_packet = vulkan::DrawPacket{};
	_packet.primitive = _primitive;
	_packet.transformMode = vulkan::VertexTransformMode::Triangle;
	_packet.textureUnit0 = 0U;
	_packet.textureUnit1 = 1U;
	_packet.shaderFlags = vulkan::combiner::resolveShaderFlags(_combiner, true, true);
	_packet.fogColorR = gDP.fogColor.r;
	_packet.fogColorG = gDP.fogColor.g;
	_packet.fogColorB = gDP.fogColor.b;
	_packet.fogColorA = gDP.fogColor.a;
}

void initializeRectPacket(
	vulkan::PrimitiveType _primitive,
	const graphics::CombinerProgram * _combiner,
	bool _texrect,
	vulkan::DrawPacket & _packet)
{
	_packet = vulkan::DrawPacket{};
	_packet.primitive = _primitive;
	_packet.transformMode = vulkan::VertexTransformMode::Rect;
	_packet.textureUnit0 = 0U;
	_packet.textureUnit1 = 1U;
	_packet.shaderFlags = vulkan::combiner::resolveShaderFlags(_combiner, false, _texrect);
	_packet.texrectAlphaTest = 0U;
	_packet.texrectFilterMode = 0U;
	_packet.blendMux1Packed = 0U;
	_packet.blendMux2Packed = 0U;
	_packet.blendParamsPacked = 0U;
	_packet.texrectTextureWidth = 1.0f;
	_packet.texrectTextureHeight = 1.0f;
	_packet.gammaLevel = 2.0f;
	_packet.textColorR = 1.0f;
	_packet.textColorG = 1.0f;
	_packet.textColorB = 1.0f;
	_packet.textColorA = 1.0f;
	_packet.fogColorR = gDP.fogColor.r;
	_packet.fogColorG = gDP.fogColor.g;
	_packet.fogColorB = gDP.fogColor.b;
	_packet.fogColorA = gDP.fogColor.a;
}

void initializeLinePacket(f32 _width, vulkan::DrawPacket & _packet)
{
	_packet = vulkan::DrawPacket{};
	_packet.primitive = vulkan::PrimitiveType::Lines;
	_packet.transformMode = vulkan::VertexTransformMode::Triangle;
	_packet.lineWidth = std::max(1.0f, _width);
	_packet.shaderFlags = vulkan::draw_shader_flags::kShade;
}

void appendTriangleVertex(const SPVertex & _src, bool _flatColors, vulkan::DrawPacket & _packet)
{
	vulkan::DrawVertex dst{};
	dst.x = _src.x;
	dst.y = _src.y;
	dst.z = _src.z;
	dst.w = _src.w;
	if (_flatColors) {
		dst.r = _src.flat_r;
		dst.g = _src.flat_g;
		dst.b = _src.flat_b;
		dst.a = _src.flat_a;
	} else {
		dst.r = _src.r;
		dst.g = _src.g;
		dst.b = _src.b;
		dst.a = _src.a;
	}
	dst.s0 = _src.s;
	dst.t0 = _src.t;
	dst.s1 = _src.s;
	dst.t1 = _src.t;
	dst.bc0 = _src.bc0;
	dst.bc1 = _src.bc1;
	dst.modify = _src.modify;
	_packet.vertices.push_back(dst);
}

void appendRectVertex(const RectVertex & _src, vulkan::DrawPacket & _packet)
{
	vulkan::DrawVertex dst{};
	dst.x = _src.x;
	dst.y = _src.y;
	dst.z = _src.z;
	dst.w = _src.w;
	// Upstream GL path feeds rect shading through uRectColor. Mirror that by
	// stamping per-vertex rect color from gDP.rectColor in Vulkan packets.
	dst.r = gDP.rectColor.r;
	dst.g = gDP.rectColor.g;
	dst.b = gDP.rectColor.b;
	dst.a = gDP.rectColor.a;
	dst.s0 = _src.s0;
	dst.t0 = _src.t0;
	dst.s1 = _src.s1;
	dst.t1 = _src.t1;
	dst.bc0 = _src.bc0;
	dst.bc1 = _src.bc1;
	dst.modify = 0U;
	_packet.vertices.push_back(dst);
}

void appendLineVertex(const SPVertex & _src, vulkan::DrawPacket & _packet)
{
	vulkan::DrawVertex dst{};
	dst.x = _src.x;
	dst.y = _src.y;
	dst.z = _src.z;
	dst.w = _src.w;
	dst.r = _src.r;
	dst.g = _src.g;
	dst.b = _src.b;
	dst.a = _src.a;
	dst.s0 = _src.s;
	dst.t0 = _src.t;
	dst.s1 = _src.s;
	dst.t1 = _src.t;
	dst.bc0 = _src.bc0;
	dst.bc1 = _src.bc1;
	dst.modify = _src.modify;
	_packet.vertices.push_back(dst);
}

void overwritePacketVertexColor(vulkan::DrawPacket & _packet, f32 _r, f32 _g, f32 _b, f32 _a)
{
	for (vulkan::DrawVertex & vertex : _packet.vertices) {
		vertex.r = _r;
		vertex.g = _g;
		vertex.b = _b;
		vertex.a = _a;
	}
}

} // namespace packet_builder
} // namespace vulkan
