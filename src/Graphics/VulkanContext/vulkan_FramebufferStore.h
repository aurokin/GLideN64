#pragma once

#include <algorithm>
#include <unordered_map>
#include <Graphics/Context.h>
#include <Graphics/ObjectHandle.h>
#include <Graphics/Parameters.h>
#include <Types.h>

namespace vulkan {

class FramebufferStore
{
public:
	struct RenderbufferResource {
		graphics::ObjectHandle handle = graphics::ObjectHandle::null;
		graphics::InternalColorFormatParam format = graphics::internalcolorFormat::NOCOLOR;
		u32 width = 0;
		u32 height = 0;
		bool initialized = false;
	};

	struct FramebufferAttachment {
		graphics::BufferAttachmentParam attachment;
		graphics::Parameter textureTarget;
		graphics::ObjectHandle textureHandle;
	};

	struct FramebufferResource {
		graphics::ObjectHandle handle = graphics::ObjectHandle::null;
		std::unordered_map<u32, FramebufferAttachment> attachments;
		u32 drawBufferCount = 1;
	};

	void clear()
	{
		m_framebuffers.clear();
		m_renderbuffers.clear();
		m_currentDrawBufferCount = 1;
	}

	void createFramebuffer(graphics::ObjectHandle _handle)
	{
		if (!_handle.isNotNull())
			return;
		FramebufferResource & framebuffer = m_framebuffers[static_cast<u32>(_handle)];
		framebuffer.handle = _handle;
		framebuffer.drawBufferCount = m_currentDrawBufferCount;
	}

	void deleteFramebuffer(graphics::ObjectHandle _handle)
	{
		if (!_handle.isNotNull())
			return;
		m_framebuffers.erase(static_cast<u32>(_handle));
	}

	void addFramebufferRenderTarget(const graphics::Context::FrameBufferRenderTarget & _params)
	{
		if (!_params.bufferHandle.isNotNull())
			return;
		FramebufferResource & framebuffer = m_framebuffers[static_cast<u32>(_params.bufferHandle)];
		framebuffer.handle = _params.bufferHandle;
		FramebufferAttachment attachment{};
		attachment.attachment = _params.attachment;
		attachment.textureTarget = _params.textureTarget;
		attachment.textureHandle = _params.textureHandle;
		framebuffer.attachments[static_cast<u32>(_params.attachment)] = attachment;
	}

	void createRenderbuffer(graphics::ObjectHandle _handle)
	{
		if (!_handle.isNotNull())
			return;
		RenderbufferResource & renderbuffer = m_renderbuffers[static_cast<u32>(_handle)];
		renderbuffer.handle = _handle;
	}

	void initRenderbuffer(const graphics::Context::InitRenderbufferParams & _params)
	{
		if (!_params.handle.isNotNull())
			return;
		RenderbufferResource & renderbuffer = m_renderbuffers[static_cast<u32>(_params.handle)];
		renderbuffer.handle = _params.handle;
		renderbuffer.format = _params.format;
		renderbuffer.width = _params.width;
		renderbuffer.height = _params.height;
		renderbuffer.initialized = true;
	}

	void setDrawBuffers(graphics::ObjectHandle _handle, u32 _num)
	{
		m_currentDrawBufferCount = std::max<u32>(1U, _num);
		if (!_handle.isNotNull())
			return;
		FramebufferResource & framebuffer = m_framebuffers[static_cast<u32>(_handle)];
		framebuffer.handle = _handle;
		framebuffer.drawBufferCount = m_currentDrawBufferCount;
	}

	const FramebufferResource * getFramebuffer(graphics::ObjectHandle _handle) const
	{
		if (!_handle.isNotNull())
			return nullptr;
		const auto it = m_framebuffers.find(static_cast<u32>(_handle));
		if (it == m_framebuffers.end())
			return nullptr;
		return &it->second;
	}

	const FramebufferAttachment * getAttachment(graphics::ObjectHandle _bufferHandle, graphics::BufferAttachmentParam _attachment) const
	{
		const FramebufferResource * framebuffer = getFramebuffer(_bufferHandle);
		if (framebuffer == nullptr)
			return nullptr;
		const auto it = framebuffer->attachments.find(static_cast<u32>(_attachment));
		if (it == framebuffer->attachments.end())
			return nullptr;
		return &it->second;
	}

	u32 getDrawBufferCount(graphics::ObjectHandle _handle) const
	{
		if (!_handle.isNotNull())
			return std::max<u32>(1U, m_currentDrawBufferCount);
		const FramebufferResource * framebuffer = getFramebuffer(_handle);
		if (framebuffer == nullptr)
			return 1U;
		return std::max<u32>(1U, framebuffer->drawBufferCount);
	}

private:
	u32 m_currentDrawBufferCount = 1;
	std::unordered_map<u32, FramebufferResource> m_framebuffers;
	std::unordered_map<u32, RenderbufferResource> m_renderbuffers;
};

} // namespace vulkan
