#pragma once

#include <Graphics/CombinerProgram.h>
#include <Graphics/ShaderProgram.h>
#include "vulkan_DrawRecorder.h"

namespace vulkan {
namespace special_programs {

graphics::ShaderProgram * createDepthFogShader();
graphics::TexrectDrawerShaderProgram * createTexrectDrawerDrawShader();
graphics::ShaderProgram * createTexrectDrawerClearShader();
graphics::ShaderProgram * createTexrectUpscaleCopyShader();
graphics::ShaderProgram * createTexrectColorAndDepthUpscaleCopyShader();
graphics::ShaderProgram * createTexrectDownscaleCopyShader();
graphics::ShaderProgram * createTexrectColorAndDepthDownscaleCopyShader();
graphics::ShaderProgram * createGammaCorrectionShader();
graphics::ShaderProgram * createFXAAShader();
graphics::TextDrawerShaderProgram * createTextDrawerShader();

bool isTexrectClearProgram(const graphics::CombinerProgram * _combiner);

void applyRectSpecialProgramState(const graphics::CombinerProgram * _combiner, vulkan::DrawPacket & _packet);

} // namespace special_programs
} // namespace vulkan
