// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>

namespace nos::compositing
{
// The quadrant compositing itself is done declaratively by Shaders/QuadMerge.frag. This node exists only to size the
// output texture from the Resolution pin, since the output is marked unscaled.
struct QuadMergeNode : NodeContext
{
	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		auto outputTex = params.GetPinObject<sys::vulkan::Texture>(NOS_NAME("Output"));
		auto outputTexInfo = sys::vulkan::GetResourceInfo(outputTex);
		const nos::fb::vec2u& resolution = *params.GetPinValue<fb::vec2u>(NOS_NAME("Resolution"));

		if (!outputTexInfo || resolution.x() != outputTexInfo->Width || resolution.y() != outputTexInfo->Height)
		{
			SetPinObject(NOS_NAME("Output"),
						 sys::vulkan::CreateTexture(
							 {
								 .Width = resolution.x(),
								 .Height = resolution.y(),
								 .Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_DST | NOS_IMAGE_USAGE_TRANSFER_SRC |
														NOS_IMAGE_USAGE_SAMPLED),
							 },
							 "QuadMergeResult"));
		}

		return nosVulkan->ExecuteGPUNode(this, params.RawParams);
	}
};

void RegisterQuadMerge(nosNodeFunctions* funcs)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("QuadMerge"), QuadMergeNode, funcs);
}
} // namespace nos::compositing
