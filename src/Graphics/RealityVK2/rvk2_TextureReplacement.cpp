#include "rvk2_TextureReplacement.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace {

constexpr u64 kFnvOffset = 1469598103934665603ULL;
constexpr u64 kFnvPrime = 1099511628211ULL;
constexpr u8 kHTCMagic[8] = {'R', 'K', 'V', 'H', 'T', 'C', '1', '\0'};
constexpr u32 kHTCVersion = 1U;

inline void hashByte(u64 & _hash, u8 _value)
{
	_hash ^= static_cast<u64>(_value);
	_hash *= kFnvPrime;
}

inline void hashU16(u64 & _hash, u16 _value)
{
	hashByte(_hash, static_cast<u8>(_value & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 8U) & 0xFFU));
}

inline void hashU32(u64 & _hash, u32 _value)
{
	hashByte(_hash, static_cast<u8>(_value & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 8U) & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 16U) & 0xFFU));
	hashByte(_hash, static_cast<u8>((_value >> 24U) & 0xFFU));
}

inline void hashU64(u64 & _hash, u64 _value)
{
	hashU32(_hash, static_cast<u32>(_value & 0xFFFFFFFFULL));
	hashU32(_hash, static_cast<u32>((_value >> 32U) & 0xFFFFFFFFULL));
}

inline void hashS32(u64 & _hash, s32 _value)
{
	hashU32(_hash, static_cast<u32>(_value));
}

inline bool writeBytes(std::FILE * _file, const void * _data, size_t _size)
{
	return std::fwrite(_data, 1U, _size, _file) == _size;
}

inline bool readBytes(std::FILE * _file, void * _data, size_t _size)
{
	return std::fread(_data, 1U, _size, _file) == _size;
}

inline bool writeU16LE(std::FILE * _file, u16 _value)
{
	const u8 bytes[2]{
		static_cast<u8>(_value & 0xFFU),
		static_cast<u8>((_value >> 8U) & 0xFFU)
	};
	return writeBytes(_file, bytes, sizeof(bytes));
}

inline bool writeU32LE(std::FILE * _file, u32 _value)
{
	const u8 bytes[4]{
		static_cast<u8>(_value & 0xFFU),
		static_cast<u8>((_value >> 8U) & 0xFFU),
		static_cast<u8>((_value >> 16U) & 0xFFU),
		static_cast<u8>((_value >> 24U) & 0xFFU)
	};
	return writeBytes(_file, bytes, sizeof(bytes));
}

inline bool writeU64LE(std::FILE * _file, u64 _value)
{
	return writeU32LE(_file, static_cast<u32>(_value & 0xFFFFFFFFULL))
		&& writeU32LE(_file, static_cast<u32>((_value >> 32U) & 0xFFFFFFFFULL));
}

inline bool readU16LE(std::FILE * _file, u16 & _value)
{
	u8 bytes[2]{};
	if (!readBytes(_file, bytes, sizeof(bytes)))
		return false;
	_value = static_cast<u16>(
		static_cast<u16>(bytes[0])
		| (static_cast<u16>(bytes[1]) << 8U));
	return true;
}

inline bool readU32LE(std::FILE * _file, u32 & _value)
{
	u8 bytes[4]{};
	if (!readBytes(_file, bytes, sizeof(bytes)))
		return false;
	_value =
		static_cast<u32>(bytes[0])
		| (static_cast<u32>(bytes[1]) << 8U)
		| (static_cast<u32>(bytes[2]) << 16U)
		| (static_cast<u32>(bytes[3]) << 24U);
	return true;
}

inline bool readU64LE(std::FILE * _file, u64 & _value)
{
	u32 lo = 0U;
	u32 hi = 0U;
	if (!readU32LE(_file, lo) || !readU32LE(_file, hi))
		return false;
	_value = (static_cast<u64>(hi) << 32U) | static_cast<u64>(lo);
	return true;
}

inline bool isImageValid(const rvk2::TextureReplacementImage & _image)
{
	if (_image.width == 0U || _image.height == 0U)
		return false;
	const size_t pixelCount = static_cast<size_t>(_image.width) * static_cast<size_t>(_image.height);
	return _image.pixels.size() == pixelCount;
}

inline s32 wrapCoordPositive(s32 _value, s32 _period)
{
	if (_period <= 0)
		return 0;
	s32 wrapped = _value % _period;
	if (wrapped < 0)
		wrapped += _period;
	return wrapped;
}

inline std::string trimAsciiWhitespace(const std::string & _input)
{
	size_t begin = 0U;
	while (begin < _input.size()
		&& std::isspace(static_cast<unsigned char>(_input[begin])) != 0)
		++begin;
	size_t end = _input.size();
	while (end > begin
		&& std::isspace(static_cast<unsigned char>(_input[end - 1U])) != 0)
		--end;
	return _input.substr(begin, end - begin);
}

bool parseU64Token(const std::string & _token, u64 & _out)
{
	const std::string token = trimAsciiWhitespace(_token);
	if (token.empty())
		return false;
	char * end = nullptr;
	errno = 0;
	const unsigned long long value = std::strtoull(token.c_str(), &end, 0);
	if (errno != 0 || end == nullptr || *end != '\0')
		return false;
	_out = static_cast<u64>(value);
	return true;
}

bool parseU16Token(const std::string & _token, u16 & _out)
{
	u64 value = 0ULL;
	if (!parseU64Token(_token, value))
		return false;
	if (value > static_cast<u64>(std::numeric_limits<u16>::max()))
		return false;
	_out = static_cast<u16>(value);
	return true;
}

std::string joinPath(const std::string & _base, const std::string & _leaf)
{
	if (_leaf.empty())
		return _base;
	if (!_leaf.empty() && (_leaf[0] == '/' || _leaf[0] == '\\'))
		return _leaf;
	if (_leaf.size() > 1U && _leaf[1U] == ':')
		return _leaf;
	if (_base.empty())
		return _leaf;
	const char tail = _base[_base.size() - 1U];
	if (tail == '/' || tail == '\\')
		return _base + _leaf;
	return _base + "/" + _leaf;
}

bool splitTSVLine(
	const std::string & _line,
	std::vector<std::string> & _outFields)
{
	_outFields.clear();
	size_t start = 0U;
	while (start <= _line.size()) {
		size_t end = _line.find('\t', start);
		if (end == std::string::npos) {
			_outFields.push_back(_line.substr(start));
			break;
		}
		_outFields.push_back(_line.substr(start, end - start));
		start = end + 1U;
	}
	return !_outFields.empty();
}

bool loadRawRGBAFile(
	const std::string & _path,
	u16 _width,
	u16 _height,
	rvk2::TextureReplacementImage & _outImage)
{
	std::FILE * file = std::fopen(_path.c_str(), "rb");
	if (file == nullptr)
		return false;
	const u32 pixelCount = static_cast<u32>(_width) * static_cast<u32>(_height);
	rvk2::TextureReplacementImage image{};
	image.width = _width;
	image.height = _height;
	image.pixels.resize(pixelCount, 0U);
	bool ok = true;
	for (u32 i = 0U; ok && i < pixelCount; ++i)
		ok = readU32LE(file, image.pixels[i]);
	std::fclose(file);
	if (!ok)
		return false;
	_outImage = std::move(image);
	return true;
}

inline u16 decodeTileSpanTexels(u16 _a, u16 _b)
{
	const u32 low = std::min<u32>(_a, _b);
	const u32 high = std::max<u32>(_a, _b);
	if (high <= low)
		return 1U;
	const u32 spanSubTexel = high - low;
	const u32 spanTexel = (spanSubTexel >> 2U) + 1U;
	return static_cast<u16>(std::max<u32>(1U, std::min<u32>(spanTexel, 0xFFFFU)));
}

inline bool isCITexture(const rvk2::RenderWorkPacket & _work)
{
	return _work.textureImageFormat == 2U || _work.tileFormat == 2U;
}

u64 buildNativeTextureHash(const rvk2::RenderWorkPacket & _work)
{
	u64 hash = kFnvOffset;
	hashByte(hash, _work.textureImageFormat);
	hashByte(hash, _work.textureImageSize);
	hashU16(hash, _work.textureImageWidth);
	hashU32(hash, _work.textureImageAddress);
	hashByte(hash, _work.tileFormat);
	hashByte(hash, _work.tileSize);
	hashU16(hash, _work.tileLine);
	hashU16(hash, _work.tileTmem);
	hashByte(hash, _work.tilePalette);
	hashByte(hash, _work.tileCms);
	hashByte(hash, _work.tileCmt);
	hashByte(hash, _work.tileMasks);
	hashByte(hash, _work.tileMaskt);
	hashByte(hash, _work.tileShifts);
	hashByte(hash, _work.tileShiftt);
	hashU16(hash, _work.tileULS);
	hashU16(hash, _work.tileULT);
	hashU16(hash, _work.tileLRS);
	hashU16(hash, _work.tileLRT);
	hashByte(hash, _work.tmemLoadKind);
	hashByte(hash, _work.tmemLoadTile);
	hashU16(hash, _work.tmemLoadULS);
	hashU16(hash, _work.tmemLoadULT);
	hashU16(hash, _work.tmemLoadLRS);
	hashU16(hash, _work.tmemLoadLRT);
	hashU16(hash, _work.tmemLoadDXT);
	return hash;
}

u64 buildPaletteHash(const rvk2::RenderWorkPacket & _work, bool _ciTexture)
{
	u64 hash = kFnvOffset;
	hashByte(hash, _work.tilePalette);
	hashU64(hash, _work.keyState);
	hashU64(hash, _work.convertState);
	if (_ciTexture) {
		hashByte(hash, _work.textureImageFormat);
		hashByte(hash, _work.textureImageSize);
		hashU32(hash, _work.textureImageAddress);
	}
	return hash;
}

} // namespace

namespace rvk2 {

TextureReplacementKey buildTextureReplacementKey(const TextureReplacementRequest & _request)
{
	TextureReplacementKey key{};
	if (_request.work == nullptr)
		return key;

	const RenderWorkPacket & work = *_request.work;
	key.ciTexture = isCITexture(work);
	key.format = work.textureImageFormat;
	key.size = work.textureImageSize;
	key.tile = work.tile;
	key.palette = work.tilePalette;
	key.nativeWidth = decodeTileSpanTexels(work.tileULS, work.tileLRS);
	key.nativeHeight = decodeTileSpanTexels(work.tileULT, work.tileLRT);
	key.nativeTextureHash = buildNativeTextureHash(work);
	key.paletteHash = buildPaletteHash(work, key.ciTexture);

	u64 deterministic = kFnvOffset;
	hashU64(deterministic, key.nativeTextureHash);
	hashU64(deterministic, key.paletteHash);
	hashU16(deterministic, key.nativeWidth);
	hashU16(deterministic, key.nativeHeight);
	hashByte(deterministic, key.format);
	hashByte(deterministic, key.size);
	hashByte(deterministic, key.tile);
	hashByte(deterministic, key.palette);
	hashByte(deterministic, key.ciTexture ? 1U : 0U);
	hashS32(deterministic, _request.s);
	hashS32(deterministic, _request.t);
	hashS32(deterministic, _request.w);
	hashByte(deterministic, _request.perspective ? 1U : 0U);
	key.deterministicKey = deterministic;
	return key;
}

TextureReplacementCacheKey buildTextureReplacementCacheKey(const TextureReplacementKey & _key)
{
	TextureReplacementCacheKey cacheKey{};
	u64 hi = kFnvOffset;
	u64 lo = kFnvOffset;
	hashU64(hi, _key.nativeTextureHash);
	hashU64(hi, _key.paletteHash);
	hashU16(hi, _key.nativeWidth);
	hashU16(hi, _key.nativeHeight);
	hashU64(lo, _key.deterministicKey);
	hashByte(lo, _key.format);
	hashByte(lo, _key.size);
	hashByte(lo, _key.tile);
	hashByte(lo, _key.palette);
	hashByte(lo, _key.ciTexture ? 1U : 0U);
	cacheKey.hi = hi;
	cacheKey.lo = lo;
	return cacheKey;
}

size_t TextureReplacementStore::KeyHash::operator()(const TextureReplacementCacheKey & _key) const
{
	u64 hash = kFnvOffset;
	hashU64(hash, _key.hi);
	hashU64(hash, _key.lo);
	return static_cast<size_t>(hash);
}

bool TextureReplacementStore::KeyEq::operator()(
	const TextureReplacementCacheKey & _a,
	const TextureReplacementCacheKey & _b) const
{
	return _a.hi == _b.hi && _a.lo == _b.lo;
}

void TextureReplacementStore::clear()
{
	m_entries.clear();
}

bool TextureReplacementStore::empty() const
{
	return m_entries.empty();
}

size_t TextureReplacementStore::entryCount() const
{
	return m_entries.size();
}

u64 TextureReplacementStore::totalPixels() const
{
	u64 pixels = 0ULL;
	for (const auto & it : m_entries) {
		const TextureReplacementImage & image = it.second;
		pixels += static_cast<u64>(image.width) * static_cast<u64>(image.height);
	}
	return pixels;
}

bool TextureReplacementStore::insert(
	const TextureReplacementCacheKey & _cacheKey,
	const TextureReplacementImage & _image)
{
	if (!isImageValid(_image))
		return false;
	m_entries[_cacheKey] = _image;
	return true;
}

const TextureReplacementImage * TextureReplacementStore::find(
	const TextureReplacementCacheKey & _cacheKey) const
{
	const auto it = m_entries.find(_cacheKey);
	return it == m_entries.end() ? nullptr : &it->second;
}

std::vector<TextureReplacementCacheKey> TextureReplacementStore::keys() const
{
	std::vector<TextureReplacementCacheKey> keys;
	keys.reserve(m_entries.size());
	for (const auto & it : m_entries)
		keys.push_back(it.first);
	std::sort(keys.begin(), keys.end(), [](const TextureReplacementCacheKey & _a, const TextureReplacementCacheKey & _b) {
		if (_a.hi != _b.hi)
			return _a.hi < _b.hi;
		return _a.lo < _b.lo;
	});
	return keys;
}

void TextureReplacementStore::applyLimits(size_t _maxEntries, u64 _maxPixels)
{
	if (m_entries.empty())
		return;
	if (_maxEntries == 0U && _maxPixels == 0ULL)
		return;

	const std::vector<TextureReplacementCacheKey> orderedKeys = keys();
	size_t keptEntries = 0U;
	u64 keptPixels = 0ULL;
	for (const TextureReplacementCacheKey & key : orderedKeys) {
		const auto it = m_entries.find(key);
		if (it == m_entries.end())
			continue;
		const TextureReplacementImage & image = it->second;
		const u64 imagePixels =
			static_cast<u64>(image.width) * static_cast<u64>(image.height);
		const bool overEntryLimit =
			(_maxEntries > 0U) && (keptEntries >= _maxEntries);
		const bool overPixelLimit =
			(_maxPixels > 0ULL)
			&& (keptPixels + imagePixels > _maxPixels);
		if (overEntryLimit || overPixelLimit) {
			m_entries.erase(it);
			continue;
		}
		++keptEntries;
		keptPixels += imagePixels;
	}
}

u32 sampleTextureReplacementImage(const TextureReplacementImage & _image, s32 _s, s32 _t)
{
	if (!isImageValid(_image))
		return 0U;
	const s32 texelS = _s >> 5U;
	const s32 texelT = _t >> 5U;
	const s32 x = wrapCoordPositive(texelS, static_cast<s32>(_image.width));
	const s32 y = wrapCoordPositive(texelT, static_cast<s32>(_image.height));
	const size_t idx =
		static_cast<size_t>(y) * static_cast<size_t>(_image.width)
		+ static_cast<size_t>(x);
	if (idx >= _image.pixels.size())
		return 0U;
	return _image.pixels[idx];
}

bool writeTextureReplacementHTC(const char * _path, const TextureReplacementStore & _store)
{
	if (_path == nullptr || _path[0] == '\0')
		return false;

	std::FILE * file = std::fopen(_path, "wb");
	if (file == nullptr)
		return false;

	bool ok = true;
	const std::vector<TextureReplacementCacheKey> keys = _store.keys();
	ok = ok && writeBytes(file, kHTCMagic, sizeof(kHTCMagic));
	ok = ok && writeU32LE(file, kHTCVersion);
	ok = ok && writeU32LE(file, static_cast<u32>(keys.size()));
	for (const TextureReplacementCacheKey & key : keys) {
		const TextureReplacementImage * image = _store.find(key);
		if (image == nullptr || !isImageValid(*image)) {
			ok = false;
			break;
		}
		const u32 pixelCount = static_cast<u32>(
			static_cast<u32>(image->width) * static_cast<u32>(image->height));
		ok = ok && writeU64LE(file, key.hi);
		ok = ok && writeU64LE(file, key.lo);
		ok = ok && writeU16LE(file, image->width);
		ok = ok && writeU16LE(file, image->height);
		ok = ok && writeU32LE(file, pixelCount);
		for (u32 i = 0U; ok && i < pixelCount; ++i)
			ok = writeU32LE(file, image->pixels[i]);
		if (!ok)
			break;
	}

	std::fclose(file);
	return ok;
}

bool loadTextureReplacementHTC(const char * _path, TextureReplacementStore & _store)
{
	if (_path == nullptr || _path[0] == '\0')
		return false;

	std::FILE * file = std::fopen(_path, "rb");
	if (file == nullptr)
		return false;

	bool ok = true;
	u8 magic[8]{};
	u32 version = 0U;
	u32 entryCount = 0U;
	ok = ok && readBytes(file, magic, sizeof(magic));
	ok = ok && readU32LE(file, version);
	ok = ok && readU32LE(file, entryCount);
	if (!ok || std::equal(std::begin(magic), std::end(magic), std::begin(kHTCMagic)) == false || version != kHTCVersion) {
		std::fclose(file);
		return false;
	}

	TextureReplacementStore loaded;
	for (u32 entryIndex = 0U; ok && entryIndex < entryCount; ++entryIndex) {
		TextureReplacementCacheKey key{};
		TextureReplacementImage image{};
		u32 pixelCount = 0U;
		ok = ok && readU64LE(file, key.hi);
		ok = ok && readU64LE(file, key.lo);
		ok = ok && readU16LE(file, image.width);
		ok = ok && readU16LE(file, image.height);
		ok = ok && readU32LE(file, pixelCount);
		const u32 expectedPixels =
			static_cast<u32>(image.width) * static_cast<u32>(image.height);
		if (!ok || image.width == 0U || image.height == 0U || pixelCount != expectedPixels) {
			ok = false;
			break;
		}
		image.pixels.resize(pixelCount, 0U);
		for (u32 i = 0U; ok && i < pixelCount; ++i)
			ok = readU32LE(file, image.pixels[i]);
		if (!ok || !loaded.insert(key, image)) {
			ok = false;
			break;
		}
	}

	std::fclose(file);
	if (!ok)
		return false;
	_store = std::move(loaded);
	return true;
}

bool loadTextureReplacementPack(const char * _packPath, TextureReplacementStore & _store)
{
	if (_packPath == nullptr || _packPath[0] == '\0')
		return false;

	const std::string packPath = _packPath;
	const std::string indexPath = joinPath(packPath, "rkv2_pack_index_v1.tsv");
	std::FILE * file = std::fopen(indexPath.c_str(), "rb");
	if (file == nullptr)
		return false;

	std::vector<std::pair<TextureReplacementCacheKey, TextureReplacementImage>> pendingEntries;
	char lineBuffer[4096]{};
	bool ok = true;
	while (std::fgets(lineBuffer, sizeof(lineBuffer), file) != nullptr) {
		std::string line = trimAsciiWhitespace(lineBuffer);
		if (line.empty() || line[0] == '#')
			continue;

		std::vector<std::string> fields;
		if (!splitTSVLine(line, fields) || fields.size() != 5U) {
			ok = false;
			break;
		}

		TextureReplacementCacheKey key{};
		u16 width = 0U;
		u16 height = 0U;
		if (!parseU64Token(fields[0], key.hi)
			|| !parseU64Token(fields[1], key.lo)
			|| !parseU16Token(fields[2], width)
			|| !parseU16Token(fields[3], height)
			|| width == 0U
			|| height == 0U) {
			ok = false;
			break;
		}

		const std::string imageLeaf = trimAsciiWhitespace(fields[4]);
		if (imageLeaf.empty()) {
			ok = false;
			break;
		}

		TextureReplacementImage image{};
		if (!loadRawRGBAFile(joinPath(packPath, imageLeaf), width, height, image)) {
			ok = false;
			break;
		}
		pendingEntries.push_back(std::make_pair(key, std::move(image)));
	}

	std::fclose(file);
	if (!ok)
		return false;

	for (const auto & entry : pendingEntries) {
		if (!_store.insert(entry.first, entry.second))
			return false;
	}
	return true;
}

} // namespace rvk2
