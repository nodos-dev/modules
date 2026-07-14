// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>

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

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		auto const& resolution = *params.GetPinValue<fb::vec2u>(NSN_Resolution);
		if (resolution.x() == 0 || resolution.y() == 0)
		{
			nosEngine.LogW("SphereProject: Resolution must be non-zero on both axes.");
			return NOS_RESULT_FAILED;
		}

		auto output = params.GetPinObject<sys::vulkan::Texture>(NSN_Output);
		auto outputInfo = sys::vulkan::GetResourceInfo(output);
		if (!outputInfo || outputInfo->Width != resolution.x() || outputInfo->Height != resolution.y())
		{
			// Usage must be spelled out for the first creation, when the pin holds no texture
			// to inherit it from; a zero-usage image is unusable as a render target.
			nosTextureInfo info = outputInfo.value_or(nosTextureInfo{
				.Format = NOS_FORMAT_R16G16B16A16_SFLOAT,
				.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_DST | NOS_IMAGE_USAGE_TRANSFER_SRC |
									   NOS_IMAGE_USAGE_SAMPLED),
			});
			info.Width = resolution.x();
			info.Height = resolution.y();
			SetPinObject(NSN_Output, sys::vulkan::CreateTexture(info, "SphereProjectResult"));
		}

		return nosVulkan->ExecuteGPUNode(this, params.RawParams);
	}
};

nosResult RegisterSphereProject(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_SphereProject, SphereProject, fn);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::graphics
