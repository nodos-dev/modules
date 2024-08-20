// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>

#include "Names.h"

namespace nos::utilities
{

NOS_REGISTER_NAME(RESIZE_PASS);
NOS_REGISTER_NAME(Method);
NOS_REGISTER_NAME(Size);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_Resize, "nos.utilities.Resize")

static nosResult ExecuteNode(void* ctx, nosNodeExecuteParams* params)
{
	nos::NodeExecuteParams nodeParams(params);
	auto inputTex = vkss::DeserializeTextureInfo(nodeParams[NSN_Input].Data->Data);
	auto method = *reinterpret_cast<uint32_t*>(nodeParams[NSN_Method].Data->Data);

	auto tex = vkss::DeserializeTextureInfo(nodeParams[NSN_Output].Data->Data);
	auto& size = *reinterpret_cast<nosVec2u*>(nodeParams[NSN_Size].Data->Data);
		
	if(size.x != tex.Info.Texture.Width ||
		size.y != tex.Info.Texture.Height)
	{
		auto prevTex = tex;
		prevTex.Memory = {};
		prevTex.Info.Texture.Width = size.x;
		prevTex.Info.Texture.Height = size.y;
		auto texFb = vkss::ConvertTextureInfo(prevTex);
		texFb.unscaled = true;
		auto texFbBuf = nos::Buffer::From(texFb);
		nosEngine.SetPinValue(params->Pins[1].Id, {.Data = texFbBuf.Data(), .Size = texFbBuf.Size()});
	}
    
	std::vector bindings = {vkss::ShaderBinding(NSN_Input, inputTex), vkss::ShaderBinding(NSN_Method, method)};
	
	tex = vkss::DeserializeTextureInfo(nodeParams[NSN_Output].Data->Data);
	
	nosRunPassParams resizeParam {
		.Key = NSN_RESIZE_PASS,
		.Bindings = bindings.data(),
		.BindingCount = 2,
		.Output = tex,
		.Wireframe = 0,
		.Benchmark = 0,
	};

	nosCmd cmd;
	nosCmdBeginParams beginParams {.Name = NOS_NAME("Resize"), .AssociatedNodeId = params->NodeId, .OutCmdHandle = &cmd};
	nosVulkan->Begin2(&beginParams);
	nosVulkan->RunPass(cmd, &resizeParam);
	nosVulkan->End(cmd, nullptr);

	return NOS_RESULT_SUCCESS;
}

nosResult RegisterResize(nosNodeFunctions* out)
{
	out->ClassName = NSN_Nos_Utilities_Resize;
	out->ExecuteNode = ExecuteNode;
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities


