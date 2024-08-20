// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/Helpers.hpp>
#include <glm/glm.hpp>
#include "Names.h"

NOS_REGISTER_NAME(Channel);
NOS_REGISTER_NAME(Format);
NOS_REGISTER_NAME(Channel_Viewer_Pass);
NOS_REGISTER_NAME(Channel_Viewer_Shader);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_ChannelViewer, "nos.utilities.ChannelViewer")

namespace nos::utilities
{
static nosResult ExecuteNode(void* ctx, nosNodeExecuteParams* pins)
{
	auto values = GetPinValues(pins);
	const nosResourceShareInfo input = vkss::DeserializeTextureInfo(values[NSN_Input]);
	const nosResourceShareInfo output = vkss::DeserializeTextureInfo(values[NSN_Output]);

	auto channel = *(u32*)values[NSN_Channel];
	auto format = *(u32*)values[NSN_Format];

	glm::vec4 val{};
	val[channel & 3] = 1;

	constexpr glm::vec3 coeffs[3] = {{.299f, .587f, .114f}, {.2126f, .7152f, .0722f}, {.2627f, .678f, .0593f}};

	glm::vec4 multipliers = glm::vec4(coeffs[format], channel > 3);
	std::vector bindings = {
		vkss::ShaderBinding(NSN_Input, input),
		vkss::ShaderBinding(NSN_Channel, val), 
		vkss::ShaderBinding(NSN_Format, multipliers)
	};

	nosRunPassParams pass = {
		.Key = NSN_Channel_Viewer_Pass,
		.Bindings = bindings.data(),
		.BindingCount = (u32)bindings.size(),
		.Output = output,
		.Wireframe = false,
	};
	nosVulkan->RunPass(0, &pass);
	return NOS_RESULT_SUCCESS;
}

nosResult RegisterChannelViewer(nosNodeFunctions* out)
{
	out->ClassName = NSN_Nos_Utilities_ChannelViewer;
	out->ExecuteNode = ExecuteNode;

	fs::path root = nosEngine.Module->RootFolderPath;
	auto chViewerPath = (root / "Shaders" / "ChannelViewer.frag").generic_string();

	nosShaderInfo2 shader = {
		.Key = NSN_Channel_Viewer_Shader, .Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = chViewerPath.c_str()},
		.AssociatedNodeClassName = NSN_Nos_Utilities_ChannelViewer
	};
	auto ret = nosVulkan->RegisterShaders2(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	nosPassInfo pass = {
		.Key = NSN_Channel_Viewer_Pass,
		.Shader = NSN_Channel_Viewer_Shader,
		.MultiSample = 1
	};
	return nosVulkan->RegisterPasses(1, &pass);
}
} 

