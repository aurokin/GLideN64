#pragma once

#include <Graphics/CombinerProgram.h>
#include <GraphicsDrawer.h>
#include <gSP.h>

#include "vulkan_DrawRecorder.h"

namespace vulkan {
namespace packet_builder {

void initializeTrianglePacket(vulkan::PrimitiveType _primitive, const graphics::CombinerProgram * _combiner, vulkan::DrawPacket & _packet);
void initializeRectPacket(
	vulkan::PrimitiveType _primitive,
	const graphics::CombinerProgram * _combiner,
	bool _texrect,
	vulkan::DrawPacket & _packet);
void initializeLinePacket(f32 _width, vulkan::DrawPacket & _packet);

void appendTriangleVertex(const SPVertex & _src, bool _flatColors, vulkan::DrawPacket & _packet);
void appendRectVertex(const RectVertex & _src, vulkan::DrawPacket & _packet);
void appendLineVertex(const SPVertex & _src, vulkan::DrawPacket & _packet);

void overwritePacketVertexColor(vulkan::DrawPacket & _packet, f32 _r, f32 _g, f32 _b, f32 _a);

} // namespace packet_builder
} // namespace vulkan
