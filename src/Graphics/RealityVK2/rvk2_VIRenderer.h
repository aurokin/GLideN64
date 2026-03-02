#pragma once

#include <vector>

#include "rvk2_Types.h"

namespace rvk2 {

enum VIRejectReason : u8
{
	kVIRejectNone = 0U,
	kVIRejectMissingSource = 1U,
	kVIRejectSourcePixelCount = 2U,
	kVIRejectInvalidRegisterState = 3U,
	kVIRejectInvalidResolvedOutput = 4U,
};

struct VIRendererConfig {
	u8 aspectX = 4U;
	u8 aspectY = 3U;
	u16 maxOutputWidth = 4096U;
	u16 maxOutputHeight = 4096U;
};

struct VIRegisterState {
	bool valid = false;
	u32 status = 0U;
	u32 origin = 0U;
	u32 width = 0U;
	u32 vCurrentLine = 0U;
	u32 vSync = 0U;
	u32 hStart = 0U;
	u32 vStart = 0U;
	u32 xScale = 0U;
	u32 yScale = 0U;
};

struct VIFrameInput {
	bool sourceAddressValid = false;
	u32 sourceAddress = 0U;
	u16 sourceWidth = 0U;
	u16 sourceHeight = 0U;
	const std::vector<u32> * sourcePixels = nullptr;
	VIRegisterState registers{};
};

struct VIFrameSummary {
	u64 presentHash = 1469598103934665603ULL;
	u32 presentWidth = 0U;
	u32 presentHeight = 0U;
	u32 contentX = 0U;
	u32 contentY = 0U;
	u32 contentWidth = 0U;
	u32 contentHeight = 0U;
	u32 resolvedSourceWidth = 0U;
	u32 resolvedSourceHeight = 0U;
	u32 resolvedOutputWidth = 0U;
	u32 resolvedOutputHeight = 0U;
	u32 resolvedLineStride = 0U;
	u8 resolvedType = 0U;
	u8 rejectReason = kVIRejectNone;
	u8 usesRegisters = 0U;
	u8 reserved0 = 0U;
	u8 aspectX = 4U;
	u8 aspectY = 3U;
};

class VIRenderer
{
public:
	explicit VIRenderer(const VIRendererConfig & _config = VIRendererConfig{});

	VIFrameSummary present(
		const VIFrameInput & _input,
		std::vector<u32> * _outputPixels = nullptr) const;

private:
	VIRendererConfig m_config;
};

VIRendererConfig loadVIRendererConfigFromEnv();

} // namespace rvk2
