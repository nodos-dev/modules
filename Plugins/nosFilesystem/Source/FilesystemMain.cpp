// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT_WITH_MIN_REQUIRED_MINOR(4)
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::filesystem
{

enum Filesystem : int
{
	ListDirectory = 0,
	ReadFile,
	Count
};

nosResult RegisterListDirectory(nosNodeFunctions*);
nosResult RegisterReadFile(nosNodeFunctions*);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
	*outSize = Filesystem::Count;
	if (!outList)
		return NOS_RESULT_SUCCESS;

#define GEN_CASE_NODE(name)					\
	case Filesystem::name: {				\
		auto ret = Register##name(node);	\
		if (NOS_RESULT_SUCCESS != ret)		\
			return ret;						\
		break;								\
	}

	for (int i = 0; i < Filesystem::Count; ++i)
	{
		auto node = outList[i];
		switch ((Filesystem)i) {
		default:
			break;
			GEN_CASE_NODE(ListDirectory)
			GEN_CASE_NODE(ReadFile)
		}
	}
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* out)
{
	out->ExportNodeFunctions = ExportNodeFunctions;
	// Migrate graphs saved when these nodes lived in nos.utilities.
	out->GetRenamedNodeClasses = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize) {
		if (!outRenamedFrom)
		{
			*outSize = 2;
			return;
		}
		// clang-format off
		outRenamedFrom[0] = NOS_NAME("nos.utilities.ListDirectory"); outRenamedTo[0] = NOS_NAME("nos.filesystem.ListDirectory");
		outRenamedFrom[1] = NOS_NAME("nos.utilities.ReadFile"); outRenamedTo[1] = NOS_NAME("nos.filesystem.ReadFile");
		// clang-format on
	};
	out->GetRenamedTypes = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize) {
		if (!outRenamedFrom)
		{
			*outSize = 1;
			return;
		}
		// clang-format off
		outRenamedFrom[0] = NOS_NAME("nos.utilities.ListDirectorySortBy"); outRenamedTo[0] = NOS_NAME("nos.filesystem.ListDirectorySortBy");
		// clang-format on
	};
	return NOS_RESULT_SUCCESS;
}
}
}
