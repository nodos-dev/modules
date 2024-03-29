﻿#include <Nodos/PluginHelpers.hpp>

// External
#include <glm/glm.hpp> // TODO: Ring no longer needs glm::mat4 colormatrix. Remove this
#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::utilities
{
struct UploadBufferNodeContext : NodeContext
{
	UploadBufferNodeContext(const nosFbNode* node) : NodeContext(node)
	{

	}

	nosResult ExecuteNode(const nosNodeExecuteArgs* args) override
	{
		auto execArgs = nos::NodeExecuteArgs(args);
		auto& output = *InterpretPinValue<sys::vulkan::Buffer>(execArgs[NOS_NAME_STATIC("Output")].Data->Data);
		auto& input = *InterpretPinValue<sys::vulkan::Buffer>(execArgs[NOS_NAME_STATIC("Input")].Data->Data);

		if (input.size_in_bytes() != output.size_in_bytes())
		{
			nosResourceShareInfo bufInfo = {
				.Info = {.Type = NOS_RESOURCE_TYPE_BUFFER,
						 .Buffer = nosBufferInfo{.Size = (uint32_t)input.size_in_bytes(),
												 .Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_DST |
																		 NOS_BUFFER_USAGE_TRANSFER_SRC |
																		 NOS_BUFFER_USAGE_STORAGE_BUFFER),
												 .MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_DEVICE_MEMORY)}}};
			auto bufferDesc = vkss::ConvertBufferInfo(bufInfo);
			nosEngine.SetPinValueByName(NodeId, NOS_NAME_STATIC("Output"), Buffer::From(bufferDesc));

			output = *InterpretPinValue<sys::vulkan::Buffer>(execArgs[NOS_NAME_STATIC("Output")].Data->Data);
		}

		if (!output.handle() || !input.handle())
		{
			return NOS_RESULT_SUCCESS;
		}

		auto OutputBuffer = vkss::ConvertToResourceInfo(output);
		auto InputBuffer = vkss::ConvertToResourceInfo(input);

		nosCmd cmd;
		nosVulkan->Begin("UploadBuffer Staging Copy", &cmd);
		nosVulkan->Copy(cmd, &InputBuffer, &OutputBuffer, 0);
		nosVulkan->End(cmd, nullptr);

		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterUploadBuffer(nosNodeFunctions* functions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("UploadBuffer"), UploadBufferNodeContext, functions)
	return NOS_RESULT_SUCCESS;
}

}