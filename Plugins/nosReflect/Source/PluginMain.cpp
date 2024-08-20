#include <Nodos/PluginHelpers.hpp>
#include <Nodos/Helpers.hpp>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::reflect
{

enum Nodes : size_t
{	// CPU nodes
	Make = 0,
	MakeDynamic,
	Break,
	Indexer,
	Array,
	Delay,
	Arithmetic,
	Count
};

nosResult RegisterMake(nosNodeFunctions* node);
nosResult RegisterBreak(nosNodeFunctions* node);
nosResult RegisterIndexer(nosNodeFunctions* node);
nosResult RegisterArray(nosNodeFunctions* node);
nosResult RegisterDelay(nosNodeFunctions* node);
nosResult RegisterArithmetic(nosNodeFunctions* node);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outCount, nosNodeFunctions** outFunctions)
{
	*outCount = (size_t)(Nodes::Count);
	if (!outFunctions)
		return NOS_RESULT_SUCCESS;
	
#define GEN_CASE_NODE(name)					\
	case Nodes::name: {						\
		auto ret = Register##name(node);	\
		if (NOS_RESULT_SUCCESS != ret)		\
			return ret;						\
		break;								\
	}

	for (size_t i = 0; i < (size_t)Nodes::Count; ++i)
	{
		auto node = outFunctions[i];
		switch ((Nodes)i)
		{
			GEN_CASE_NODE(Make)
			GEN_CASE_NODE(Break)
			GEN_CASE_NODE(Indexer)
			GEN_CASE_NODE(Array)
			GEN_CASE_NODE(Delay)
			GEN_CASE_NODE(Arithmetic)
		}
	}

#undef GEN_CASE_NODE
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
/// Nodos calls this function to initialize the plugin & retrieve the plugin's functions.
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outPluginFunctions)
{
	outPluginFunctions->ExportNodeFunctions = ExportNodeFunctions;
	return NOS_RESULT_SUCCESS;
}
}

}