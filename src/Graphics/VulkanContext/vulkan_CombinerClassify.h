#pragma once

#include <Types.h>
#include <CombinerKey.h>

namespace vulkan {
namespace combiner {

enum class CombinerDirectOutputMode : u8 {
	kNone = 0U,
	kZero,
	kOne,
	kShade,
	kTexel0,
	kTexel1,
	kPrimitive,
	kEnvironment
};

struct CombinerActiveCycleTerms {
	u32 colorA = 0U;
	u32 colorB = 0U;
	u32 colorM = 0U;
	u32 colorD = 0U;
	u32 alphaA = 0U;
	u32 alphaB = 0U;
	u32 alphaM = 0U;
	u32 alphaD = 0U;
};

enum class CombinerConstantModulateMode : u8 {
	kNone = 0U,
	kPrimitiveTexel0,
	kPrimitiveTexel1,
	kEnvironmentTexel0,
	kEnvironmentTexel1,
	kPrimitiveShade,
	kEnvironmentShade
};

enum class CombinerConstantAddMode : u8 {
	kNone = 0U,
	kPrimitiveTexel0,
	kPrimitiveTexel1,
	kPrimitiveShade,
	kEnvironmentTexel0,
	kEnvironmentTexel1,
	kEnvironmentShade
};

enum class CombinerDirectAlphaMode : u8 {
	kNone = 0U,
	kZero,
	kOne,
	kShade,
	kTexel0,
	kTexel1,
	kPrimitive,
	kEnvironment
};

struct CombinerTexturedAlphaScaleInfo {
	bool valid = false;
	bool primaryIsTexel0 = true;
	CombinerDirectAlphaMode alphaScaleMode = CombinerDirectAlphaMode::kNone;
};

enum class CombinerPostModulateConstantSource : u8 {
	kNone = 0U,
	kPrimitive,
	kEnvironment
};

enum class CombinerTwoCyclePostModulateBase : u8 {
	kNone = 0U,
	kTexel0ShadeAlphaTexel0Shade,
	kTexel0ShadeAlphaTexel0,
	kShadeAlphaShade
};

struct CombinerTwoCyclePostModulateInfo {
	CombinerPostModulateConstantSource source = CombinerPostModulateConstantSource::kNone;
	CombinerTwoCyclePostModulateBase base = CombinerTwoCyclePostModulateBase::kNone;

	bool valid() const
	{
		return source != CombinerPostModulateConstantSource::kNone
			&& base != CombinerTwoCyclePostModulateBase::kNone;
	}
};

CombinerDirectOutputMode detectCombinerDirectOutputMode(const CombinerKey & _key);
CombinerConstantModulateMode detectCombinerConstantModulateMode(const CombinerKey & _key);
CombinerConstantAddMode detectCombinerConstantAddMode(const CombinerKey & _key);
CombinerDirectAlphaMode detectCombinerDirectAlphaMode(const CombinerKey & _key);
CombinerTexturedAlphaScaleInfo detectCombinerTexturedAlphaScaleInfo(const CombinerKey & _key);
CombinerTwoCyclePostModulateInfo detectCombinerTwoCyclePostModulateInfo(const CombinerKey & _key);
bool decodeCombinerActiveCycleTerms(const CombinerKey & _key, CombinerActiveCycleTerms & _terms);

} // namespace combiner
} // namespace vulkan
