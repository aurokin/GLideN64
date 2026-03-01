#pragma once

#include <unordered_map>
#include <Graphics/Context.h>
#include <Graphics/ObjectHandle.h>
#include <Graphics/Parameters.h>
#include <Types.h>

namespace vulkan {

class BindingState
{
public:
	struct TextureBinding {
		graphics::ObjectHandle texture = graphics::ObjectHandle::null;
		graphics::TextureTargetParam target = graphics::textureTarget::TEXTURE_2D;
	};

	struct ImageBinding {
		graphics::ObjectHandle texture = graphics::ObjectHandle::null;
		graphics::ImageAccessModeParam accessMode = graphics::textureImageAccessMode::READ_ONLY;
		graphics::InternalColorFormatParam textureFormat = graphics::internalcolorFormat::NOCOLOR;
	};

	void clear()
	{
		m_boundTextures.clear();
		m_imageBindings.clear();
	}

	void bindTexture(const graphics::Context::BindTextureParameters & _params)
	{
		const u32 unit = static_cast<u32>(_params.textureUnitIndex);
		if (!_params.texture.isNotNull()) {
			TextureBinding & binding = m_boundTextures[unit];
			binding.texture = graphics::ObjectHandle::null;
			binding.target = _params.target;
			return;
		}
		TextureBinding & binding = m_boundTextures[unit];
		binding.texture = _params.texture;
		binding.target = _params.target;
	}

	void clearTextureBindings(graphics::ObjectHandle _texture)
	{
		if (!_texture.isNotNull())
			return;
		for (auto & bound : m_boundTextures) {
			if (bound.second.texture == _texture)
				bound.second.texture = graphics::ObjectHandle::null;
		}
		for (auto & imageBinding : m_imageBindings) {
			if (imageBinding.second.texture == _texture)
				imageBinding.second.texture = graphics::ObjectHandle::null;
		}
	}

	void bindImageTexture(const graphics::Context::BindImageTextureParameters & _params)
	{
		ImageBinding & binding = m_imageBindings[static_cast<u32>(_params.imageUnit)];
		binding.texture = _params.texture;
		binding.accessMode = _params.accessMode;
		binding.textureFormat = _params.textureFormat;
	}

	template <typename Fn>
	void forEachTextureBinding(Fn && _fn) const
	{
		for (const auto & binding : m_boundTextures) {
			if (!binding.second.texture.isNotNull())
				continue;
			_fn(binding.first, binding.second);
		}
	}

	template <typename Fn>
	void forEachImageBinding(Fn && _fn) const
	{
		for (const auto & binding : m_imageBindings) {
			if (!binding.second.texture.isNotNull())
				continue;
			_fn(binding.first, binding.second);
		}
	}

private:
	std::unordered_map<u32, TextureBinding> m_boundTextures;
	std::unordered_map<u32, ImageBinding> m_imageBindings;
};

} // namespace vulkan
