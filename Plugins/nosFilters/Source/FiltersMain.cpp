// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/PluginAPI.h>
#include <glm/glm.hpp>

// Nodes
#include "GaussianBlur.hpp"

// Dependencies
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::filters
{

enum Filters : int
{
	GaussianBlur = 0,
	Count
};

NOSAPI_ATTR nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
	if (!outList)
	{
		*outSize = Filters::Count;
		return NOS_RESULT_SUCCESS;
	}
	RegisterGaussianBlur(outList[GaussianBlur]);
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outFunctions)
{
	outFunctions->ExportNodeFunctions = ExportNodeFunctions;
	return NOS_RESULT_SUCCESS;
}
}
}
