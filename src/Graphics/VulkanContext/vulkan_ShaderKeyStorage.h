#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <Combiner.h>
#include <Graphics/CombinerProgram.h>
#include <Graphics/Context.h>
#include <Log.h>
#include <PluginAPI.h>
#include <RSP.h>
#include <osal_files.h>

namespace vulkan {

class ShaderKeyStorage
{
public:
	typedef std::function<graphics::CombinerProgram *(const CombinerKey &)> ProgramFactory;

	static bool save(const graphics::Combiners & _combiners)
	{
		std::string keysFileName = _getStorageFileName("keys");
		if (keysFileName.empty())
			return false;

#if defined(OS_WINDOWS) && !defined(MINGW)
		std::ofstream keysOut(keysFileName, std::ofstream::trunc);
#else
		std::ofstream keysOut(keysFileName.c_str(), std::ofstream::trunc);
#endif
		if (!keysOut)
			return false;

		std::vector<u64> keysData;
		keysData.reserve(_combiners.size());
		for (auto cur = _combiners.begin(); cur != _combiners.end(); ++cur)
			keysData.push_back(cur->first.getMux());
		std::sort(keysData.begin(), keysData.end());

		constexpr u32 kKeysVersion = 1U;
		keysOut << "0x" << std::hex << std::setfill('0') << std::setw(8) << kKeysVersion << "\n";
		keysOut << "0x" << std::hex << std::setfill('0') << std::setw(8) << static_cast<u32>(keysData.size()) << "\n";
		for (u64 key : keysData)
			keysOut << "0x" << std::hex << std::setfill('0') << std::setw(16) << key << "\n";

		keysOut.flush();
		keysOut.close();
		return true;
	}

	static bool load(graphics::Combiners & _combiners, const ProgramFactory & _programFactory)
	{
		if (!_programFactory)
			return false;

		std::string keysFileName = _getStorageFileName("keys");
		if (keysFileName.empty())
			return false;

#if defined(OS_WINDOWS) && !defined(MINGW)
		std::ifstream keysIn(keysFileName);
#else
		std::ifstream keysIn(keysFileName.c_str());
#endif
		if (!keysIn)
			return false;

		u32 version = 0;
		keysIn >> std::hex >> version;
		if (!keysIn || version == 0)
			return false;

		u32 keyCount = 0;
		keysIn >> std::hex >> keyCount;
		if (!keysIn)
			return false;

		for (u32 i = 0; i < keyCount; ++i) {
			u64 mux = 0;
			keysIn >> std::hex >> mux;
			if (!keysIn)
				break;
			const CombinerKey key(mux);
			if (_combiners.find(key) != _combiners.end())
				continue;
			graphics::CombinerProgram * program = _programFactory(key);
			if (program == nullptr)
				continue;
			_combiners[key] = program;
		}
		return true;
	}

private:
	static std::string _getStorageFileName(const char * _fileExtension)
	{
		class SetLocale
		{
		public:
			SetLocale()
				: m_locale(setlocale(LC_CTYPE, nullptr))
			{
				setlocale(LC_CTYPE, "");
			}
			~SetLocale()
			{
				setlocale(LC_CTYPE, m_locale.c_str());
			}

		private:
			std::string m_locale;
		} setLocale;

		wchar_t cacheFolderPathW[PLUGIN_PATH_SIZE];
		api().GetUserCachePath(cacheFolderPathW);

		char cacheFolderPath[PLUGIN_PATH_SIZE * 4];
		std::wcstombs(cacheFolderPath, cacheFolderPathW, sizeof(cacheFolderPath));

		std::stringstream path;
		path << cacheFolderPath << "/shaders";

		wchar_t shaderFolderPathW[PLUGIN_PATH_SIZE];
		std::mbstowcs(shaderFolderPathW, path.str().c_str(), PLUGIN_PATH_SIZE);
		if (!osal_path_existsW(shaderFolderPathW) || !osal_is_directory(shaderFolderPathW)) {
			if (osal_mkdirp(shaderFolderPathW) != 0) {
				path.str("");
				path << cacheFolderPath;
			}
		}

		path << "/RealityVK." << std::hex << static_cast<u32>(std::hash<std::string>()(RSP.romname))
			<< ".Vulkan." << _fileExtension;
		return path.str();
	}
};

} // namespace vulkan
