// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

// External
#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::utilities
{

struct Buffer2TextureNodeContext : NodeContext
{
	Buffer2TextureNodeContext(const nosFbNode* node) : NodeContext(node)
	{
	}
	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nos::NodeExecuteParams execParams(params);
		const auto& inputPinData = *InterpretPinValue<sys::vulkan::Buffer>(execParams[NOS_NAME_STATIC("Input")].Data->Data);
		const nosBuffer* outputPinData = execParams[NOS_NAME_STATIC("Output")].Data;
		const auto& output = *InterpretPinValue<sys::vulkan::Texture>(outputPinData->Data);
		const auto& size = *InterpretPinValue<fb::vec2u>(execParams[NOS_NAME_STATIC("Size")].Data->Data);
		const auto& format = *InterpretPinValue<sys::vulkan::Format>(execParams[NOS_NAME_STATIC("Format")].Data->Data);
		if (size.x() != output.width() ||
			size.y() != output.height() ||
			format != output.format())
		{
			nosResourceShareInfo tex{.Info = {
				.Type = NOS_RESOURCE_TYPE_TEXTURE,
				.Texture = {
					.Width = size.x(),
					.Height = size.y(),
					.Format = nosFormat(format)
				}
			}};
			sys::vulkan::TTexture texDef = vkss::ConvertTextureInfo(tex);
			nosEngine.SetPinValueByName(NodeId, NOS_NAME_STATIC("Output"), Buffer::From(texDef));
		}
		nosResourceShareInfo out = vkss::DeserializeTextureInfo(outputPinData->Data);
		nosResourceShareInfo in = vkss::ConvertToResourceInfo(inputPinData);

		if (!in.Memory.Handle || !out.Memory.Handle)
			return NOS_RESULT_SUCCESS;

		nosCmd cmd;
		nosVulkan->Begin("Buffer2Texture Copy", &cmd);
		nosVulkan->Copy(cmd, &in, &out, 0);
		nosGPUEvent event;
		nosCmdEndParams endParams{.ForceSubmit = true, .OutGPUEventHandle = &event};
		nosVulkan->End(cmd, &endParams);
		nosVulkan->WaitGpuEvent(&event, UINT_MAX);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterBuffer2Texture(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.utilities.Buffer2Texture"), Buffer2TextureNodeContext, funcs);
	return NOS_RESULT_SUCCESS;
}

}