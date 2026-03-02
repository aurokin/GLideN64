#pragma once

#include <algorithm>
#include <array>
#include <cmath>

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
	TriangleWords words{};
	if (!buildWords(_v0, _v1, _v2, opcode, _tile, _level, words))
		return false;

	runtime().submitRDPWord(
		_dlistAddress,
		words.w0,
		words.w1,
		_provenance,
		6U,
		words.w2,
		words.w3,
		words.w4,
		words.w5,
		words.w6,
		words.w7,
		8U);
	return true;
}

} // namespace rvk2::synthetic_triangle
