#include "vulkan_CombinerClassify.h"

#include <Combiner.h>
#include <GBI.h>
#include <gDP.h>

#include "vulkan_CombinerDecode.h"

namespace vulkan {
namespace combiner {

bool decodeCombinerActiveCycleTerms(const CombinerKey & _key, CombinerActiveCycleTerms & _terms)
{
	if (_key == CombinerKey::getEmpty())
		return false;

	const u32 cycleType = _key.getCycleType();
	if (cycleType == G_CYC_COPY || cycleType == G_CYC_FILL)
		return false;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();
	const bool secondCycle = cycleType == G_CYC_2CYCLE;
	_terms.colorA = secondCycle
		? expandCombinerColorAForSolidCheck(decoded.saRGB1)
		: expandCombinerColorAForSolidCheck(decoded.saRGB0);
	_terms.colorB = secondCycle
		? expandCombinerColorBForSolidCheck(decoded.sbRGB1)
		: expandCombinerColorBForSolidCheck(decoded.sbRGB0);
	_terms.colorM = secondCycle
		? expandCombinerColorMForSolidCheck(decoded.mRGB1)
		: expandCombinerColorMForSolidCheck(decoded.mRGB0);
	_terms.colorD = secondCycle
		? expandCombinerColorDForSolidCheck(decoded.aRGB1)
		: expandCombinerColorDForSolidCheck(decoded.aRGB0);
	_terms.alphaA = secondCycle
		? expandCombinerAlphaAForSolidCheck(decoded.saA1)
		: expandCombinerAlphaAForSolidCheck(decoded.saA0);
	_terms.alphaB = secondCycle
		? expandCombinerAlphaBForSolidCheck(decoded.sbA1)
		: expandCombinerAlphaBForSolidCheck(decoded.sbA0);
	_terms.alphaM = secondCycle
		? expandCombinerAlphaMForSolidCheck(decoded.mA1)
		: expandCombinerAlphaMForSolidCheck(decoded.mA0);
	_terms.alphaD = secondCycle
		? expandCombinerAlphaDForSolidCheck(decoded.aA1)
		: expandCombinerAlphaDForSolidCheck(decoded.aA0);
	return true;
}

CombinerDirectOutputMode detectCombinerDirectOutputMode(const CombinerKey & _key)
{
	CombinerActiveCycleTerms terms{};
	if (!decodeCombinerActiveCycleTerms(_key, terms))
		return CombinerDirectOutputMode::kNone;

	if (terms.colorM != G_GCI_ZERO || terms.alphaM != G_GCI_ZERO)
		return CombinerDirectOutputMode::kNone;

	if (terms.colorD == G_GCI_ZERO && terms.alphaD == G_GCI_ZERO)
		return CombinerDirectOutputMode::kZero;
	if (terms.colorD == G_GCI_ONE && terms.alphaD == G_GCI_ONE)
		return CombinerDirectOutputMode::kOne;
	if (terms.colorD == G_GCI_SHADE && terms.alphaD == G_GCI_SHADE_ALPHA)
		return CombinerDirectOutputMode::kShade;
	if (terms.colorD == G_GCI_TEXEL0 && terms.alphaD == G_GCI_TEXEL0_ALPHA)
		return CombinerDirectOutputMode::kTexel0;
	if (terms.colorD == G_GCI_TEXEL1 && terms.alphaD == G_GCI_TEXEL1_ALPHA)
		return CombinerDirectOutputMode::kTexel1;
	if (terms.colorD == G_GCI_PRIMITIVE && terms.alphaD == G_GCI_PRIMITIVE_ALPHA)
		return CombinerDirectOutputMode::kPrimitive;
	if (terms.colorD == G_GCI_ENVIRONMENT && terms.alphaD == G_GCI_ENV_ALPHA)
		return CombinerDirectOutputMode::kEnvironment;
	return CombinerDirectOutputMode::kNone;
}

CombinerConstantModulateMode detectCombinerConstantModulateMode(const CombinerKey & _key)
{
	CombinerActiveCycleTerms terms{};
	if (!decodeCombinerActiveCycleTerms(_key, terms))
		return CombinerConstantModulateMode::kNone;

	if (terms.colorB != G_GCI_ZERO
		|| terms.colorD != G_GCI_ZERO
		|| terms.alphaB != G_GCI_ZERO
		|| terms.alphaD != G_GCI_ZERO) {
		return CombinerConstantModulateMode::kNone;
	}

	auto isModulatePattern = [&](u32 _texColor, u32 _constColor, u32 _texAlpha, u32 _constAlpha) -> bool {
		const bool colorMatch =
			((terms.colorA == _texColor && terms.colorM == _constColor)
				|| (terms.colorA == _constColor && terms.colorM == _texColor));
		const bool alphaMatch =
			((terms.alphaA == _texAlpha && terms.alphaM == _constAlpha)
				|| (terms.alphaA == _constAlpha && terms.alphaM == _texAlpha));
		return colorMatch && alphaMatch;
	};

	if (isModulatePattern(G_GCI_TEXEL0, G_GCI_PRIMITIVE, G_GCI_TEXEL0_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantModulateMode::kPrimitiveTexel0;
	if (isModulatePattern(G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantModulateMode::kPrimitiveTexel1;
	if (isModulatePattern(G_GCI_TEXEL0, G_GCI_ENVIRONMENT, G_GCI_TEXEL0_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantModulateMode::kEnvironmentTexel0;
	if (isModulatePattern(G_GCI_TEXEL1, G_GCI_ENVIRONMENT, G_GCI_TEXEL1_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantModulateMode::kEnvironmentTexel1;
	if (isModulatePattern(G_GCI_SHADE, G_GCI_PRIMITIVE, G_GCI_SHADE_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantModulateMode::kPrimitiveShade;
	if (isModulatePattern(G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantModulateMode::kEnvironmentShade;
	return CombinerConstantModulateMode::kNone;
}

CombinerConstantAddMode detectCombinerConstantAddMode(const CombinerKey & _key)
{
	CombinerActiveCycleTerms terms{};
	if (!decodeCombinerActiveCycleTerms(_key, terms))
		return CombinerConstantAddMode::kNone;

	if (terms.colorB != G_GCI_ZERO
		|| terms.alphaB != G_GCI_ZERO
		|| terms.colorM != G_GCI_ONE
		|| terms.alphaM != G_GCI_ONE) {
		return CombinerConstantAddMode::kNone;
	}

	auto matchesAddPattern = [&](u32 _sourceColor, u32 _constColor, u32 _sourceAlpha, u32 _constAlpha) -> bool {
		return terms.colorA == _sourceColor
			&& terms.colorD == _constColor
			&& terms.alphaA == _sourceAlpha
			&& terms.alphaD == _constAlpha;
	};

	if (matchesAddPattern(G_GCI_TEXEL0, G_GCI_PRIMITIVE, G_GCI_TEXEL0_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantAddMode::kPrimitiveTexel0;
	if (matchesAddPattern(G_GCI_TEXEL1, G_GCI_PRIMITIVE, G_GCI_TEXEL1_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantAddMode::kPrimitiveTexel1;
	if (matchesAddPattern(G_GCI_SHADE, G_GCI_PRIMITIVE, G_GCI_SHADE_ALPHA, G_GCI_PRIMITIVE_ALPHA))
		return CombinerConstantAddMode::kPrimitiveShade;
	if (matchesAddPattern(G_GCI_TEXEL0, G_GCI_ENVIRONMENT, G_GCI_TEXEL0_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantAddMode::kEnvironmentTexel0;
	if (matchesAddPattern(G_GCI_TEXEL1, G_GCI_ENVIRONMENT, G_GCI_TEXEL1_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantAddMode::kEnvironmentTexel1;
	if (matchesAddPattern(G_GCI_SHADE, G_GCI_ENVIRONMENT, G_GCI_SHADE_ALPHA, G_GCI_ENV_ALPHA))
		return CombinerConstantAddMode::kEnvironmentShade;
	return CombinerConstantAddMode::kNone;
}

CombinerDirectAlphaMode detectCombinerDirectAlphaMode(const CombinerKey & _key)
{
	CombinerActiveCycleTerms terms{};
	if (!decodeCombinerActiveCycleTerms(_key, terms))
		return CombinerDirectAlphaMode::kNone;

	if (terms.alphaM != G_GCI_ZERO)
		return CombinerDirectAlphaMode::kNone;

	switch (terms.alphaD) {
	case G_GCI_ZERO:
		return CombinerDirectAlphaMode::kZero;
	case G_GCI_ONE:
		return CombinerDirectAlphaMode::kOne;
	case G_GCI_SHADE_ALPHA:
		return CombinerDirectAlphaMode::kShade;
	case G_GCI_TEXEL0_ALPHA:
		return CombinerDirectAlphaMode::kTexel0;
	case G_GCI_TEXEL1_ALPHA:
		return CombinerDirectAlphaMode::kTexel1;
	case G_GCI_PRIMITIVE_ALPHA:
		return CombinerDirectAlphaMode::kPrimitive;
	case G_GCI_ENV_ALPHA:
		return CombinerDirectAlphaMode::kEnvironment;
	default:
		return CombinerDirectAlphaMode::kNone;
	}
}

CombinerTexturedAlphaScaleInfo detectCombinerTexturedAlphaScaleInfo(const CombinerKey & _key)
{
	CombinerTexturedAlphaScaleInfo info{};
	CombinerActiveCycleTerms terms{};
	if (!decodeCombinerActiveCycleTerms(_key, terms))
		return info;

	if (terms.colorB != G_GCI_ZERO || terms.colorM != G_GCI_ZERO)
		return info;
	if (terms.alphaB != G_GCI_ZERO || terms.alphaD != G_GCI_ZERO)
		return info;

	if (terms.colorD == G_GCI_TEXEL0) {
		if (terms.alphaA != G_GCI_TEXEL0_ALPHA)
			return info;
		info.primaryIsTexel0 = true;
	} else if (terms.colorD == G_GCI_TEXEL1) {
		if (terms.alphaA != G_GCI_TEXEL1_ALPHA)
			return info;
		info.primaryIsTexel0 = false;
	} else {
		return info;
	}

	switch (terms.alphaM) {
	case G_GCI_PRIMITIVE_ALPHA:
		info.alphaScaleMode = CombinerDirectAlphaMode::kPrimitive;
		break;
	case G_GCI_ENV_ALPHA:
		info.alphaScaleMode = CombinerDirectAlphaMode::kEnvironment;
		break;
	case G_GCI_SHADE_ALPHA:
		info.alphaScaleMode = CombinerDirectAlphaMode::kShade;
		break;
	case G_GCI_ONE:
		info.alphaScaleMode = CombinerDirectAlphaMode::kOne;
		break;
	default:
		info.alphaScaleMode = CombinerDirectAlphaMode::kNone;
		break;
	}

	info.valid = info.alphaScaleMode != CombinerDirectAlphaMode::kNone;
	return info;
}

CombinerTwoCyclePostModulateInfo detectCombinerTwoCyclePostModulateInfo(const CombinerKey & _key)
{
	CombinerTwoCyclePostModulateInfo info{};
	if (_key == CombinerKey::getEmpty())
		return info;
	if (_key.getCycleType() != G_CYC_2CYCLE)
		return info;

	gDPCombine decoded{};
	decoded.mux = _key.getMux();

	const u32 colorA0 = expandCombinerColorAForSolidCheck(decoded.saRGB0);
	const u32 colorB0 = expandCombinerColorBForSolidCheck(decoded.sbRGB0);
	const u32 colorM0 = expandCombinerColorMForSolidCheck(decoded.mRGB0);
	const u32 colorD0 = expandCombinerColorDForSolidCheck(decoded.aRGB0);
	const u32 alphaA0 = expandCombinerAlphaAForSolidCheck(decoded.saA0);
	const u32 alphaB0 = expandCombinerAlphaBForSolidCheck(decoded.sbA0);
	const u32 alphaM0 = expandCombinerAlphaMForSolidCheck(decoded.mA0);
	const u32 alphaD0 = expandCombinerAlphaDForSolidCheck(decoded.aA0);

	const u32 colorA1 = expandCombinerColorAForSolidCheck(decoded.saRGB1);
	const u32 colorB1 = expandCombinerColorBForSolidCheck(decoded.sbRGB1);
	const u32 colorM1 = expandCombinerColorMForSolidCheck(decoded.mRGB1);
	const u32 colorD1 = expandCombinerColorDForSolidCheck(decoded.aRGB1);
	const u32 alphaA1 = expandCombinerAlphaAForSolidCheck(decoded.saA1);
	const u32 alphaB1 = expandCombinerAlphaBForSolidCheck(decoded.sbA1);
	const u32 alphaM1 = expandCombinerAlphaMForSolidCheck(decoded.mA1);
	const u32 alphaD1 = expandCombinerAlphaDForSolidCheck(decoded.aA1);

	if (colorA1 != G_GCI_COMBINED
		|| colorB1 != G_GCI_ZERO
		|| colorD1 != G_GCI_ZERO
		|| alphaA1 != G_GCI_ZERO
		|| alphaB1 != G_GCI_ZERO
		|| alphaM1 != G_GCI_ZERO
		|| alphaD1 != G_GCI_COMBINED) {
		return info;
	}

	if (colorM1 == G_GCI_PRIMITIVE) {
		info.source = CombinerPostModulateConstantSource::kPrimitive;
	} else if (colorM1 == G_GCI_ENVIRONMENT) {
		info.source = CombinerPostModulateConstantSource::kEnvironment;
	} else {
		return info;
	}

	if (colorA0 == G_GCI_TEXEL0
		&& colorB0 == G_GCI_ZERO
		&& colorM0 == G_GCI_SHADE
		&& colorD0 == G_GCI_ZERO) {
		if (alphaA0 == G_GCI_TEXEL0_ALPHA
			&& alphaB0 == G_GCI_ZERO
			&& alphaM0 == G_GCI_SHADE_ALPHA
			&& alphaD0 == G_GCI_ZERO) {
			info.base = CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0Shade;
			return info;
		}
		if (alphaA0 == G_GCI_ZERO
			&& alphaB0 == G_GCI_ZERO
			&& alphaM0 == G_GCI_ZERO
			&& alphaD0 == G_GCI_TEXEL0_ALPHA) {
			info.base = CombinerTwoCyclePostModulateBase::kTexel0ShadeAlphaTexel0;
			return info;
		}
	}

	if (colorA0 == G_GCI_ZERO
		&& colorB0 == G_GCI_ZERO
		&& colorM0 == G_GCI_ZERO
		&& colorD0 == G_GCI_SHADE
		&& alphaA0 == G_GCI_ZERO
		&& alphaB0 == G_GCI_ZERO
		&& alphaM0 == G_GCI_ZERO
		&& alphaD0 == G_GCI_SHADE_ALPHA) {
		info.base = CombinerTwoCyclePostModulateBase::kShadeAlphaShade;
		return info;
	}

	return info;
}

} // namespace combiner
} // namespace vulkan
