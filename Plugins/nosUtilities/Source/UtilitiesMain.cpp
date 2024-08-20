// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/PluginHelpers.hpp>
#include <glm/glm.hpp>
#include <Builtins_generated.h>

#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

NOS_REGISTER_NAME(Input);
NOS_REGISTER_NAME(Output);
NOS_REGISTER_NAME(In);
NOS_REGISTER_NAME(Out);
NOS_REGISTER_NAME(Path);
NOS_REGISTER_NAME(sRGB);

namespace nos::utilities
{

enum Utilities : int
{	// CPU nodes
	Resize = 0,
	ChannelViewer,
	Merge,
	Time,
	ReadImage,
	WriteImage,
	CPUSleep,
	UploadBuffer,
	Buffer2Texture,
	Texture2Buffer,
	IsSameStringNode,
	ShowStatusNode,
	Sink,
	Count
};

nosResult RegisterMerge(nosNodeFunctions*);
nosResult RegisterTime(nosNodeFunctions*);
nosResult RegisterReadImage(nosNodeFunctions*);
nosResult RegisterWriteImage(nosNodeFunctions*);
nosResult RegisterChannelViewer(nosNodeFunctions*);
nosResult RegisterResize(nosNodeFunctions*);
nosResult RegisterCPUSleep(nosNodeFunctions*);
nosResult RegisterUploadBuffer(nosNodeFunctions*);
nosResult RegisterBuffer2Texture(nosNodeFunctions*);
nosResult RegisterTexture2Buffer(nosNodeFunctions*);
nosResult RegisterIsSameStringNode(nosNodeFunctions*);
nosResult RegisterShowStatusNode(nosNodeFunctions*);
nosResult RegisterSink(nosNodeFunctions*);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
	*outSize = Utilities::Count;
	if (!outList)
		return NOS_RESULT_SUCCESS;
	//
	// auto ret = RequestVulkanSubsystem();
	// if (ret != NOS_RESULT_SUCCESS)
	// 	return ret;

#define GEN_CASE_NODE(name)				\
	case Utilities::name: {					\
		auto ret = Register##name(node);	\
		if (NOS_RESULT_SUCCESS != ret)		\
			return ret;						\
		break;								\
	}

	for (int i = 0; i < Utilities::Count; ++i)
	{
		auto node = outList[i];
		switch ((Utilities)i) {
		default:
			break;
			GEN_CASE_NODE(Merge)
			GEN_CASE_NODE(Time)
			GEN_CASE_NODE(ReadImage)
			GEN_CASE_NODE(WriteImage)
			GEN_CASE_NODE(ChannelViewer)
			GEN_CASE_NODE(Resize)
			GEN_CASE_NODE(CPUSleep)
			GEN_CASE_NODE(UploadBuffer)
			GEN_CASE_NODE(Buffer2Texture)
			GEN_CASE_NODE(Texture2Buffer)
			GEN_CASE_NODE(IsSameStringNode)
			GEN_CASE_NODE(ShowStatusNode)
			GEN_CASE_NODE(Sink)
		}
	}
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* out)
{
	out->ExportNodeFunctions = ExportNodeFunctions;
	return NOS_RESULT_SUCCESS;
}
}
}	
