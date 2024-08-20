// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

// External
#include <nosVulkanSubsystem/Helpers.hpp>

#include "Names.h"

namespace nos::utilities
{
NOS_REGISTER_NAME(Texture2Buffer)
NOS_REGISTER_NAME(OutputBuffer)

sys::vulkan::BufferElementType GetBufferElementTypeFromVulkanFormat(nosFormat format);

typedef struct BufferPin
{
	uint64_t Size;
	uint64_t Address;
	uint64_t Offset;
} BufferPin;

struct Texture2BufferNodeContext : nos::NodeContext
{
	BufferPin BufferPinProxy = {};
	nosResourceShareInfo Buffer = {};
	nosUUID NodeUUID = {}, InputUUID = {}, OutputBufferUUID = {};
	Texture2BufferNodeContext(nosFbNode const* node) : NodeContext(node)
	{
		NodeUUID = *node->id();

		for (const auto& pin : *node->pins()) {
			const char* currentPinName = pin->name()->c_str();
			if (NSN_Input.Compare(pin->name()->c_str()) == 0) {
				InputUUID = *pin->id();
			}
			else if (NSN_OutputBuffer.Compare(pin->name()->c_str()) == 0) {
				OutputBufferUUID = *pin->id();
			}
		}
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto pinIds = nos::GetPinIds(params);
		auto pinValues = nos::GetPinValues(params);
		nosResourceShareInfo inputTextureInfo = nos::vkss::DeserializeTextureInfo(pinValues[NSN_Input]);
		PrepareResources(inputTextureInfo);
		return NOS_RESULT_SUCCESS;
	}

	void PrepareResources(nosResourceShareInfo& in) {
		uint64_t currentSize = in.Memory.Size;
		
		if (currentSize == Buffer.Memory.Size) {
			nosCmd cmd = {};
			nosCmdBeginParams beginParams = {.Name = NOS_NAME("Texture2Buffer Copy"), .AssociatedNodeId = NodeId, .OutCmdHandle = &cmd};
			nosVulkan->Begin2(&beginParams);
			nosVulkan->Copy(cmd, &in, &Buffer, 0);
			nosVulkan->End(cmd, nullptr);
			return;
		}

		if (Buffer.Memory.Handle != NULL) {
			nosVulkan->DestroyResource(&Buffer);
		}
		Buffer.Info.Type = NOS_RESOURCE_TYPE_BUFFER;
		Buffer.Info.Buffer.Size = currentSize;
		Buffer.Info.Buffer.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_SRC);
		nosVulkan->CreateResource(&Buffer);

		nos::sys::vulkan::Buffer buffer;
		buffer.mutate_element_type(GetBufferElementTypeFromVulkanFormat(in.Info.Texture.Format));
		buffer.mutate_handle(Buffer.Memory.Handle);
		buffer.mutate_size_in_bytes(Buffer.Memory.Size);
		buffer.mutate_offset(Buffer.Memory.ExternalMemory.Offset);

		buffer.mutable_external_memory().mutate_allocation_size(Buffer.Memory.ExternalMemory.AllocationSize);
		buffer.mutable_external_memory().mutate_handle(Buffer.Memory.ExternalMemory.Handle);
		buffer.mutable_external_memory().mutate_handle_type(Buffer.Memory.ExternalMemory.HandleType);
		buffer.mutable_external_memory().mutate_pid(Buffer.Memory.ExternalMemory.PID);

		nos::Buffer bufPin = nos::Buffer::From(buffer);

		nosEngine.SetPinValueDirect(OutputBufferUUID, bufPin);
	}

};

nosResult RegisterTexture2Buffer(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Texture2Buffer, Texture2BufferNodeContext, fn);
	return NOS_RESULT_SUCCESS;
}

// TODO: Maybe move it to nos.sys.vulkan subsystem
sys::vulkan::BufferElementType GetBufferElementTypeFromVulkanFormat(nosFormat format) {
	switch (format) {
		/*
		* VK_FORMAT_R8G8B8A8_SRGB specifies a four-component, 32-bit unsigned normalized format that
		has an 8-bit R component stored with sRGB nonlinear encoding in byte 0, an 8-bit G component stored with sRGB nonlinear
		encoding in byte 1, an 8-bit B component stored with sRGB nonlinear encoding in byte 2, and an 8-bit A component in byte 3.

		VK_FORMAT_B8G8R8A8_UNORM specifies a four-component, 32-bit unsigned normalized format that has an 8-bit B component in byte 0,
		an 8-bit G component in byte 1, an 8-bit R component in byte 2, and an 8-bit A component in byte 3.
		*/
	case NOS_FORMAT_R8_UNORM:
	case NOS_FORMAT_R8G8_UNORM:
	case NOS_FORMAT_R8G8B8_UNORM:
	case NOS_FORMAT_B8G8R8_UNORM:
	case NOS_FORMAT_R8G8B8A8_UNORM:
	case NOS_FORMAT_B8G8R8A8_UNORM:
	case NOS_FORMAT_G8B8G8R8_422_UNORM:
	case NOS_FORMAT_B8G8R8G8_422_UNORM:
	case NOS_FORMAT_R8_UINT:
	case NOS_FORMAT_R8G8_UINT:
	case NOS_FORMAT_B8G8R8_UINT:
	case NOS_FORMAT_R8G8B8A8_UINT:
	case NOS_FORMAT_R8_SRGB:
	case NOS_FORMAT_R8G8_SRGB:
	case NOS_FORMAT_R8G8B8_SRGB:
	case NOS_FORMAT_B8G8R8_SRGB:
	case NOS_FORMAT_R8G8B8A8_SRGB:
	case NOS_FORMAT_B8G8R8A8_SRGB:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_UINT8;

	case NOS_FORMAT_R16_UNORM:
	case NOS_FORMAT_R16G16_UNORM:
	case NOS_FORMAT_R16G16B16_UNORM:
	case NOS_FORMAT_R16G16B16A16_UNORM:
	case NOS_FORMAT_D16_UNORM:
	case NOS_FORMAT_R16_UINT:
	case NOS_FORMAT_R16G16B16_UINT:
	case NOS_FORMAT_R16G16_UINT:
	case NOS_FORMAT_R16G16B16A16_UINT:
	case NOS_FORMAT_R16_USCALED:
	case NOS_FORMAT_R16G16_USCALED:
	case NOS_FORMAT_R16G16B16_USCALED:
	case NOS_FORMAT_R16G16B16A16_USCALED:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_UINT16;

	case NOS_FORMAT_R16_SINT:
	case NOS_FORMAT_R16G16_SINT:
	case NOS_FORMAT_R16G16B16_SINT:
	case NOS_FORMAT_R16G16B16A16_SINT:
	case NOS_FORMAT_R16_SNORM:
	case NOS_FORMAT_R16G16_SNORM:
	case NOS_FORMAT_R16G16B16_SNORM:
	case NOS_FORMAT_R16G16B16A16_SNORM:
	case NOS_FORMAT_R16_SSCALED:
	case NOS_FORMAT_R16G16_SSCALED:
	case NOS_FORMAT_R16G16B16_SSCALED:
	case NOS_FORMAT_R16G16B16A16_SSCALED:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_INT16;

	case NOS_FORMAT_R16_SFLOAT:
	case NOS_FORMAT_R16G16_SFLOAT:
	case NOS_FORMAT_R16G16B16_SFLOAT:
	case NOS_FORMAT_R16G16B16A16_SFLOAT:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_FLOAT16;

	case NOS_FORMAT_R32_UINT:
	case NOS_FORMAT_R32G32_UINT:
	case NOS_FORMAT_R32G32B32_UINT:
	case NOS_FORMAT_R32G32B32A32_UINT:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_UINT32;

	case NOS_FORMAT_R32_SINT:
	case NOS_FORMAT_R32G32_SINT:
	case NOS_FORMAT_R32G32B32_SINT:
	case NOS_FORMAT_R32G32B32A32_SINT:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_INT32;

	case NOS_FORMAT_R32_SFLOAT:
	case NOS_FORMAT_R32G32_SFLOAT:
	case NOS_FORMAT_R32G32B32_SFLOAT:
	case NOS_FORMAT_R32G32B32A32_SFLOAT:
	case NOS_FORMAT_D32_SFLOAT:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_FLOAT;

	default:
		return sys::vulkan::BufferElementType::ELEMENT_TYPE_UNDEFINED;
	}
}
}
