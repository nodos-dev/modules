// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::filesystem
{
enum class Filesystem : size_t
{
	ListDirectory,
	ReadFile,
	Count
};

nosResult RegisterListDirectory(nosNodeFunctions*);
nosResult RegisterReadFile(nosNodeFunctions*);

extern "C" nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
	*outSize = static_cast<size_t>(Filesystem::Count);
	if (!outList)
		return NOS_RESULT_SUCCESS;

	RegisterListDirectory(outList[static_cast<size_t>(Filesystem::ListDirectory)]);
	RegisterReadFile(outList[static_cast<size_t>(Filesystem::ReadFile)]);
	return NOS_RESULT_SUCCESS;
}

// Migrate graphs saved while these nodes still lived in nos.utilities.
void GetRenamedNodeClasses(nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize)
{
	static std::vector<std::pair<nos::Name, nos::Name>> renames = {
		{NOS_NAME("nos.utilities.ListDirectory"), NOS_NAME("nos.filesystem.ListDirectory")},
		{NOS_NAME("nos.utilities.ReadFile"), NOS_NAME("nos.filesystem.ReadFile")},
	};

	if (!outRenamedFrom)
	{
		*outSize = renames.size();
		return;
	}

	for (size_t i = 0; i < renames.size(); ++i)
	{
		outRenamedFrom[i] = renames[i].first;
		outRenamedTo[i] = renames[i].second;
	}
}

void GetRenamedTypes(nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize)
{
	static std::vector<std::pair<nos::Name, nos::Name>> renames = {
		{NOS_NAME("nos.utilities.ListDirectorySortBy"), NOS_NAME("nos.filesystem.ListDirectorySortBy")},
	};

	if (!outRenamedFrom)
	{
		*outSize = renames.size();
		return;
	}

	for (size_t i = 0; i < renames.size(); ++i)
	{
		outRenamedFrom[i] = renames[i].first;
		outRenamedTo[i] = renames[i].second;
	}
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outFunctions)
{
	outFunctions->ExportNodeFunctions = ExportNodeFunctions;
	outFunctions->GetRenamedNodeClasses = GetRenamedNodeClasses;
	outFunctions->GetRenamedTypes = GetRenamedTypes;
	return NOS_RESULT_SUCCESS;
}
}
} // namespace nos::filesystem
