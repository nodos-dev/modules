// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

namespace nos::graphics
{
NOS_REGISTER_NAME(SphereProject)
NOS_REGISTER_NAME(Resolution)
NOS_REGISTER_NAME(Output)

// The shader and pass are registered from the node's GPUNode contents block, and every
// scalar pin binds into the shader UBO by name. All this context does is size the render
// target to the Resolution pin before handing execution back to the vulkan subsystem, so
// the output resolution and the aspect ratio the shader derives from it can never diverge.
struct SphereProject : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		NodeExecuteParams pins(params);

		nosVec2u resolution = *pins.GetPinData<nosVec2u>(NSN_Resolution);
		if (resolution.x == 0 || resolution.y == 0)
		{
			nosEngine.LogW("SphereProject: Resolution must be non-zero on both axes.");
			return NOS_RESULT_FAILED;
		}

		auto output = vkss::DeserializeTextureInfo(pins[NSN_Output].Data->Data);
		if (output.Info.Texture.Width != resolution.x || output.Info.Texture.Height != resolution.y)
		{
			auto desc = output;
			desc.Memory = {};
			desc.Info.Texture.Width = resolution.x;
			desc.Info.Texture.Height = resolution.y;
			auto textureFb = vkss::ConvertTextureInfo(desc);
			textureFb.unscaled = true;
			nosEngine.SetPinValueByName(NodeId, NSN_Output, Buffer::From(textureFb));
		}

		return nosVulkan->ExecuteGPUNode(this, params);
	}
};

nosResult RegisterSphereProject(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_SphereProject, SphereProject, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::graphics
