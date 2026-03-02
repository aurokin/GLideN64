#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "gSP.h"
#include "Graphics/RealityVK2/rvk2_Runtime.h"

namespace rvk2::synthetic_triangle {

namespace detail {

struct TrianglePoint
{
	f32 x = 0.0f;
	f32 y = 0.0f;
};

inline s32 toYSubpixel14(f32 _y)
{
	return static_cast<s32>(std::lround(static_cast<double>(_y) * 4.0));
}

inline s32 toFixed16(f32 _value)
{
	return static_cast<s32>(std::lround(static_cast<double>(_value) * 65536.0));
}

inline s32 calcSlopeFixed(f32 _x0, f32 _x1, s32 _y0, s32 _y1)
{
	if (_y0 == _y1)
		return 0;
	const double slope = static_cast<double>(_x1 - _x0) / static_cast<double>(_y1 - _y0);
	return static_cast<s32>(std::llround(slope * 65536.0));
}

inline u32 packSigned14(s32 _value)
{
	return static_cast<u32>(_value) & 0x3FFFU;
}

inline u32 packSignedFixedWord(s32 _value, u32 _integerBits)
{
	const u32 rawMask = (1U << (_integerBits + 16U)) - 1U;
	const u32 integerMask = (1U << _integerBits) - 1U;
	const u32 raw = static_cast<u32>(_value) & rawMask;
	const u32 integerPart = (raw >> 16U) & integerMask;
	const u32 fractionalPart = raw & 0xFFFFU;
	return (integerPart << 16U) | fractionalPart;
}

struct AttributePlane
{
	s32 base = 0;
	s32 dx = 0;
	s32 dy = 0;
};

inline s32 clampS32FromDouble(double _value)
{
	if (_value < static_cast<double>(std::numeric_limits<s32>::min()))
		return std::numeric_limits<s32>::min();
	if (_value > static_cast<double>(std::numeric_limits<s32>::max()))
		return std::numeric_limits<s32>::max();
	return static_cast<s32>(std::llround(_value));
}

inline AttributePlane fitPlane(
	const SPVertex & _v0,
	const SPVertex & _v1,
	const SPVertex & _v2,
	double _a0,
	double _a1,
	double _a2)
{
	const double x0 = static_cast<double>(_v0.x);
	const double y0 = static_cast<double>(_v0.y);
	const double x1 = static_cast<double>(_v1.x);
	const double y1 = static_cast<double>(_v1.y);
	const double x2 = static_cast<double>(_v2.x);
	const double y2 = static_cast<double>(_v2.y);

	const double det = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
	if (std::fabs(det) < 1.0e-9)
		return AttributePlane{ clampS32FromDouble(_a0), 0, 0 };

	const double dx = (_a0 * (y1 - y2) + _a1 * (y2 - y0) + _a2 * (y0 - y1)) / det;
	const double dy = (_a0 * (x2 - x1) + _a1 * (x0 - x2) + _a2 * (x1 - x0)) / det;
	const double base = _a0 - dx * x0 - dy * y0;

	return AttributePlane{
		clampS32FromDouble(base),
		clampS32FromDouble(dx),
		clampS32FromDouble(dy)
	};
}

inline u32 encodePairWordA(s32 _first, s32 _second)
{
	const u32 first = static_cast<u32>(_first);
	const u32 second = static_cast<u32>(_second);
	return (first & 0xFFFF0000U) | ((second >> 16U) & 0xFFFFU);
}

inline u32 encodePairWordB(s32 _first, s32 _second)
{
	const u32 first = static_cast<u32>(_first);
	const u32 second = static_cast<u32>(_second);
	return ((first & 0xFFFFU) << 16U) | (second & 0xFFFFU);
}

inline s32 colorToFixed8(f32 _value)
{
	double channel = static_cast<double>(_value);
	if (channel >= -0.5 && channel <= 1.5)
		channel *= 255.0;
	channel = std::max(0.0, std::min(255.0, channel));
	return clampS32FromDouble(channel * 256.0);
}

inline s32 texCoordToCoord5(f32 _value)
{
	return clampS32FromDouble(static_cast<double>(_value) * 32.0);
}

inline s32 textureWToFixed16(f32 _value)
{
	const double absW = std::max(1.0e-6, std::fabs(static_cast<double>(_value)));
	return clampS32FromDouble(65536.0 / absW);
}

inline s32 depthToFixed16(f32 _value)
{
	return clampS32FromDouble(static_cast<double>(_value) * 65536.0);
}

} // namespace detail

struct TriangleWords
{
	u32 w0 = 0U;
	u32 w1 = 0U;
	u32 w2 = 0U;
	u32 w3 = 0U;
	u32 w4 = 0U;
	u32 w5 = 0U;
	u32 w6 = 0U;
	u32 w7 = 0U;
};

struct TriangleCommand
{
	TriangleWords words{};
	u8 payloadWordCount = 0U;
	u16 fullWordCount = 0U;
	std::array<u32, rvk2::kMaxCommandPayloadWords> payloadWords{};
};

inline u8 selectOpcode(bool _shade, bool _textured, bool _depthTest)
{
	if (_shade) {
		if (_textured)
			return _depthTest ? 0x0FU : 0x0EU;
		return _depthTest ? 0x0DU : 0x0CU;
	}
	if (_textured)
		return _depthTest ? 0x0BU : 0x0AU;
	return _depthTest ? 0x09U : 0x08U;
}

inline bool buildWords(
	const SPVertex & _v0,
	const SPVertex & _v1,
	const SPVertex & _v2,
	u8 _opcode,
	u8 _tile,
	u8 _level,
	TriangleWords & _words)
{
	std::array<detail::TrianglePoint, 3> points{
		detail::TrianglePoint{ _v0.x, _v0.y },
		detail::TrianglePoint{ _v1.x, _v1.y },
		detail::TrianglePoint{ _v2.x, _v2.y }
	};
	std::sort(
		points.begin(),
		points.end(),
		[](const detail::TrianglePoint & _lhs, const detail::TrianglePoint & _rhs) -> bool {
			if (_lhs.y < _rhs.y)
				return true;
			if (_lhs.y > _rhs.y)
				return false;
			return _lhs.x < _rhs.x;
		});

	const detail::TrianglePoint & top = points[0];
	const detail::TrianglePoint & middle = points[1];
	const detail::TrianglePoint & bottom = points[2];

	const s32 yh = detail::toYSubpixel14(top.y);
	const s32 ym = detail::toYSubpixel14(middle.y);
	const s32 yl = detail::toYSubpixel14(bottom.y);
	if (yl <= yh)
		return false;

	const s32 xh = detail::toFixed16(top.x);
	const s32 xm = xh;
	const s32 xl = detail::toFixed16(middle.x);
	const s32 dxhdy = detail::calcSlopeFixed(top.x, bottom.x, yh, yl);
	s32 dxmdy = detail::calcSlopeFixed(top.x, middle.x, yh, ym);
	if (ym == yh)
		dxmdy = dxhdy;
	s32 dxldy = detail::calcSlopeFixed(middle.x, bottom.x, ym, yl);
	if (yl == ym)
		dxldy = dxhdy;

	const s64 xLongAtYm = static_cast<s64>(xh) + static_cast<s64>(dxhdy) * static_cast<s64>(ym - yh);
	const bool lmajor = xLongAtYm < static_cast<s64>(xl);

	_words.w0 = (static_cast<u32>(_opcode) << 24)
		| ((lmajor ? 1U : 0U) << 23)
		| ((static_cast<u32>(_level) & 0x7U) << 19)
		| ((static_cast<u32>(_tile) & 0x7U) << 16)
		| detail::packSigned14(yl);
	_words.w1 = (detail::packSigned14(ym) << 16)
		| detail::packSigned14(yh);
	_words.w2 = detail::packSignedFixedWord(xl, 12U);
	_words.w3 = detail::packSignedFixedWord(dxldy, 14U);
	_words.w4 = detail::packSignedFixedWord(xh, 12U);
	_words.w5 = detail::packSignedFixedWord(dxhdy, 14U);
	_words.w6 = detail::packSignedFixedWord(xm, 12U);
	_words.w7 = detail::packSignedFixedWord(dxmdy, 14U);
	return true;
}

inline bool isShadeOpcode(u8 _opcode)
{
	return _opcode == 0x0CU || _opcode == 0x0DU || _opcode == 0x0EU || _opcode == 0x0FU;
}

inline bool isTextureOpcode(u8 _opcode)
{
	return _opcode == 0x0AU || _opcode == 0x0BU || _opcode == 0x0EU || _opcode == 0x0FU;
}

inline bool isDepthOpcode(u8 _opcode)
{
	return _opcode == 0x09U || _opcode == 0x0BU || _opcode == 0x0DU || _opcode == 0x0FU;
}

inline void appendPairWords(
	s32 _first,
	s32 _second,
	u32 _wordAIndex,
	u32 _wordBIndex,
	std::array<u32, 16> & _out)
{
	_out[_wordAIndex] = detail::encodePairWordA(_first, _second);
	_out[_wordBIndex] = detail::encodePairWordB(_first, _second);
}

inline void appendFeatureWords(
	const std::array<u32, 16> & _words,
	u32 & _payloadIndex,
	std::array<u32, rvk2::kMaxCommandPayloadWords> & _payloadWords)
{
	for (u32 i = 0U; i < 16U; ++i)
		_payloadWords[_payloadIndex++] = _words[i];
}

inline bool buildCommand(
	const SPVertex & _v0,
	const SPVertex & _v1,
	const SPVertex & _v2,
	u8 _opcode,
	u8 _tile,
	u8 _level,
	TriangleCommand & _command)
{
	if (!buildWords(_v0, _v1, _v2, _opcode, _tile, _level, _command.words))
		return false;

	auto & payloadWords = _command.payloadWords;
	payloadWords.fill(0U);
	u32 payloadIndex = 0U;
	payloadWords[payloadIndex++] = _command.words.w2;
	payloadWords[payloadIndex++] = _command.words.w3;
	payloadWords[payloadIndex++] = _command.words.w4;
	payloadWords[payloadIndex++] = _command.words.w5;
	payloadWords[payloadIndex++] = _command.words.w6;
	payloadWords[payloadIndex++] = _command.words.w7;

	if (isShadeOpcode(_opcode)) {
		const detail::AttributePlane rPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::colorToFixed8(_v0.r)),
			static_cast<double>(detail::colorToFixed8(_v1.r)),
			static_cast<double>(detail::colorToFixed8(_v2.r)));
		const detail::AttributePlane gPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::colorToFixed8(_v0.g)),
			static_cast<double>(detail::colorToFixed8(_v1.g)),
			static_cast<double>(detail::colorToFixed8(_v2.g)));
		const detail::AttributePlane bPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::colorToFixed8(_v0.b)),
			static_cast<double>(detail::colorToFixed8(_v1.b)),
			static_cast<double>(detail::colorToFixed8(_v2.b)));
		const detail::AttributePlane aPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::colorToFixed8(_v0.a)),
			static_cast<double>(detail::colorToFixed8(_v1.a)),
			static_cast<double>(detail::colorToFixed8(_v2.a)));

		std::array<u32, 16> shadeWords{};
		appendPairWords(rPlane.base, gPlane.base, 0U, 4U, shadeWords);
		appendPairWords(bPlane.base, aPlane.base, 1U, 5U, shadeWords);
		appendPairWords(rPlane.dx, gPlane.dx, 2U, 6U, shadeWords);
		appendPairWords(bPlane.dx, aPlane.dx, 3U, 7U, shadeWords);
		appendPairWords(0, 0, 8U, 12U, shadeWords);
		appendPairWords(0, 0, 9U, 13U, shadeWords);
		appendPairWords(rPlane.dy, gPlane.dy, 10U, 14U, shadeWords);
		appendPairWords(bPlane.dy, aPlane.dy, 11U, 15U, shadeWords);
		appendFeatureWords(shadeWords, payloadIndex, payloadWords);
	}

	if (isTextureOpcode(_opcode)) {
		const detail::AttributePlane sPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::texCoordToCoord5(_v0.s)),
			static_cast<double>(detail::texCoordToCoord5(_v1.s)),
			static_cast<double>(detail::texCoordToCoord5(_v2.s)));
		const detail::AttributePlane tPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::texCoordToCoord5(_v0.t)),
			static_cast<double>(detail::texCoordToCoord5(_v1.t)),
			static_cast<double>(detail::texCoordToCoord5(_v2.t)));
		const detail::AttributePlane wPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::textureWToFixed16(_v0.w)),
			static_cast<double>(detail::textureWToFixed16(_v1.w)),
			static_cast<double>(detail::textureWToFixed16(_v2.w)));

		std::array<u32, 16> textureWords{};
		appendPairWords(sPlane.base, tPlane.base, 0U, 4U, textureWords);
		appendPairWords(wPlane.base, 0, 1U, 5U, textureWords);
		appendPairWords(sPlane.dx, tPlane.dx, 2U, 6U, textureWords);
		appendPairWords(wPlane.dx, 0, 3U, 7U, textureWords);
		appendPairWords(0, 0, 8U, 12U, textureWords);
		appendPairWords(0, 0, 9U, 13U, textureWords);
		appendPairWords(sPlane.dy, tPlane.dy, 10U, 14U, textureWords);
		appendPairWords(wPlane.dy, 0, 11U, 15U, textureWords);
		appendFeatureWords(textureWords, payloadIndex, payloadWords);
	}

	if (isDepthOpcode(_opcode)) {
		const detail::AttributePlane zPlane = detail::fitPlane(
			_v0, _v1, _v2,
			static_cast<double>(detail::depthToFixed16(_v0.z)),
			static_cast<double>(detail::depthToFixed16(_v1.z)),
			static_cast<double>(detail::depthToFixed16(_v2.z)));
		payloadWords[payloadIndex++] = static_cast<u32>(zPlane.base);
		payloadWords[payloadIndex++] = static_cast<u32>(zPlane.dx);
		payloadWords[payloadIndex++] = 0U;
		payloadWords[payloadIndex++] = static_cast<u32>(zPlane.dy);
	}

	if (payloadIndex > rvk2::kMaxCommandPayloadWords)
		return false;

	_command.payloadWordCount = static_cast<u8>(payloadIndex);
	_command.fullWordCount = static_cast<u16>(2U + payloadIndex);
	return true;
}

inline bool submit(
	const SPVertex & _v0,
	const SPVertex & _v1,
	const SPVertex & _v2,
	bool _shade,
	bool _textured,
	bool _depthTest,
	u8 _tile,
	u8 _level,
	u32 _dlistAddress,
	const CommandProvenance & _provenance)
{
	const u8 opcode = selectOpcode(_shade, _textured, _depthTest);
	TriangleCommand command{};
	if (!buildCommand(_v0, _v1, _v2, opcode, _tile, _level, command))
		return false;

	const u8 inlineWordCount = static_cast<u8>(std::min<u32>(command.payloadWordCount, 6U));
	const u32 w2 = command.payloadWordCount > 0U ? command.payloadWords[0] : 0U;
	const u32 w3 = command.payloadWordCount > 1U ? command.payloadWords[1] : 0U;
	const u32 w4 = command.payloadWordCount > 2U ? command.payloadWords[2] : 0U;
	const u32 w5 = command.payloadWordCount > 3U ? command.payloadWords[3] : 0U;
	const u32 w6 = command.payloadWordCount > 4U ? command.payloadWords[4] : 0U;
	const u32 w7 = command.payloadWordCount > 5U ? command.payloadWords[5] : 0U;

	runtime().submitRDPWord(
		_dlistAddress,
		command.words.w0,
		command.words.w1,
		_provenance,
		inlineWordCount,
		w2,
		w3,
		w4,
		w5,
		w6,
		w7,
		command.fullWordCount,
		1469598103934665603ULL,
		command.payloadWordCount,
		command.payloadWordCount > 0U ? command.payloadWords.data() : nullptr);
	return true;
}

} // namespace rvk2::synthetic_triangle
