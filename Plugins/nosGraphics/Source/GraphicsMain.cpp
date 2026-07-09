// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::graphics
{
enum Nodes : int
{ // CPU nodes
	TrackToView,
	BillboardMask,
	BillboardPositions,
	NormalizedDepthToDepthBuffer,
	HomographySolver,
	ConvertCoordinateFrame,
	CameraGuide,
	TrackToTransformQ,
	SphereProject,
	Count
};

nosResult RegisterTrackToView(nosNodeFunctions*);
nosResult RegisterBillboardMask(nosNodeFunctions*);
nosResult RegisterBillboardPositions(nosNodeFunctions*);
nosResult RegisterNormalizedDepthToDepthBuffer(nosNodeFunctions*);
nosResult RegisterHomographySolver(nosNodeFunctions*);
nosResult RegisterConvertCoordinateFrame(nosNodeFunctions*);
nosResult RegisterCameraGuide(nosNodeFunctions*);
nosResult RegisterTrackToTransformQ(nosNodeFunctions*);
nosResult RegisterSphereProject(nosNodeFunctions*);

struct PluginFunctions : nos::PluginFunctions
{
	nosResult NOSAPI_CALL ExportNodeFunctions(size_t& outSize, nosNodeFunctions** outFunctions)
	{
		outSize = Nodes::Count;
		if (!outFunctions)
			return NOS_RESULT_SUCCESS;
#define GEN_CASE_NODE(name)                                                                                            \
	case Nodes::name: {                                                                                                \
		auto ret = Register##name(node);                                                                               \
		if (NOS_RESULT_SUCCESS != ret)                                                                                 \
			return ret;                                                                                                \
		break;                                                                                                         \
	}
		for (int i = 0; i < Nodes::Count; ++i)
		{
			auto node = outFunctions[i];
			switch ((Nodes)i)
			{
				GEN_CASE_NODE(TrackToView)
				GEN_CASE_NODE(BillboardMask)
				GEN_CASE_NODE(BillboardPositions)
				GEN_CASE_NODE(NormalizedDepthToDepthBuffer)
				GEN_CASE_NODE(HomographySolver)
				GEN_CASE_NODE(ConvertCoordinateFrame)
				GEN_CASE_NODE(CameraGuide)
				GEN_CASE_NODE(TrackToTransformQ)
				GEN_CASE_NODE(SphereProject)
			default: break;
			}
		}
		return NOS_RESULT_SUCCESS;
	}
};
NOS_EXPORT_PLUGIN_FUNCTIONS(PluginFunctions)
} // namespace nos::graphics
