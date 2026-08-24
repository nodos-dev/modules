// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::compositing
{
void RegisterBoxFit(nosNodeFunctions*);
nosResult RegisterChannelViewer(nosNodeFunctions*);
nosResult RegisterMerge(nosNodeFunctions*);
void RegisterQuadMerge(nosNodeFunctions*);
nosResult RegisterLayoutDrawer(nosNodeFunctions*);
void RegisterFreeLayout(nosNodeFunctions*);
void RegisterGridLayout(nosNodeFunctions*);
void RegisterFreeOutputLayout(nosNodeFunctions*);
void RegisterGridOutputLayout(nosNodeFunctions*);
void RegisterTextureTransition(nosNodeFunctions*);
void RegisterTextureMapper(nosNodeFunctions*);
void RegisterMixer(nosNodeFunctions*);
void RegisterGrid3D(nosNodeFunctions*);
void RegisterCanvasMapper(nosNodeFunctions*);

enum class Nodes : size_t
{
	BoxFit,
	ChannelViewer,
	Merge,
	QuadMerge,
	LayoutDrawer,
	FreeLayout,
	GridLayout,
	FreeOutputLayout,
	GridOutputLayout,
	TextureTransition,
	TextureMapper,
	Mixer,
	Grid3D,
	CanvasMapper,
	Count,
};

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outCount, nosNodeFunctions** outList)
{
	if (outCount)
		*outCount = static_cast<size_t>(Nodes::Count);
	if (!outList)
		return NOS_RESULT_SUCCESS;

#define GEN_CASE_NODE(name)     \
	case Nodes::name: {         \
		Register##name(node);   \
		break;                  \
	}

#define GEN_CASE_NODE_RESULT(name)     \
	case Nodes::name: {                \
		auto ret = Register##name(node); \
		if (ret != NOS_RESULT_SUCCESS)  \
			return ret;                 \
		break;                          \
	}

	for (size_t i = 0; i < static_cast<size_t>(Nodes::Count); ++i)
	{
		auto* node = outList[i];
		switch (static_cast<Nodes>(i))
		{
		default:
			break;
			GEN_CASE_NODE(BoxFit)
			GEN_CASE_NODE_RESULT(ChannelViewer)
			GEN_CASE_NODE_RESULT(Merge)
			GEN_CASE_NODE(QuadMerge)
			GEN_CASE_NODE_RESULT(LayoutDrawer)
			GEN_CASE_NODE(FreeLayout)
			GEN_CASE_NODE(GridLayout)
			GEN_CASE_NODE(FreeOutputLayout)
			GEN_CASE_NODE(GridOutputLayout)
			GEN_CASE_NODE(TextureTransition)
			GEN_CASE_NODE(TextureMapper)
			GEN_CASE_NODE(Mixer)
			GEN_CASE_NODE(Grid3D)
			GEN_CASE_NODE(CanvasMapper)
		}
	}

#undef GEN_CASE_NODE
#undef GEN_CASE_NODE_RESULT

	return NOS_RESULT_SUCCESS;
}

void GetRenamedTypes(nosName* outFrom, nosName* outTo, size_t* outSize)
{
	static std::vector<std::pair<nos::Name, nos::Name>> renames = {
		{NOS_NAME("nos.utilities.BoxFitMode"), NOS_NAME("nos.compositing.BoxFitMode")},
		{NOS_NAME("nos.utilities.BlendMode"), NOS_NAME("nos.compositing.BlendMode")},
		{NOS_NAME("nos.utilities.Source"), NOS_NAME("nos.compositing.Source")},
		{NOS_NAME("nos.utilities.Channel"), NOS_NAME("nos.compositing.Channel")},
		{NOS_NAME("nos.utilities.ChannelViewerChannels"), NOS_NAME("nos.compositing.ChannelViewerChannels")},
		{NOS_NAME("nos.utilities.ChannelViewerFormats"), NOS_NAME("nos.compositing.ChannelViewerFormats")},
		{NOS_NAME("nos.utilities.TextureSwitcherChannel"), NOS_NAME("nos.compositing.TextureSwitcherChannel")},
		{NOS_NAME("nos.plugin.switcher.TextureSwitcherChannel"), NOS_NAME("nos.compositing.TextureSwitcherChannel")},
		{NOS_NAME("nos.utilities.LayoutType"), NOS_NAME("nos.compositing.LayoutType")},
		{NOS_NAME("nos.utilities.FreeLayoutItem"), NOS_NAME("nos.compositing.FreeLayoutItem")},
		{NOS_NAME("nos.utilities.GridLayoutItem"), NOS_NAME("nos.compositing.GridLayoutItem")},
		{NOS_NAME("nos.utilities.FreeOutputItem"), NOS_NAME("nos.compositing.FreeOutputItem")},
		{NOS_NAME("nos.utilities.GridOutputItem"), NOS_NAME("nos.compositing.GridOutputItem")},
		{NOS_NAME("nos.utilities.LayoutDrawItem"), NOS_NAME("nos.compositing.LayoutDrawItem")},
		{NOS_NAME("nos.utilities.LayoutOutputInfo"), NOS_NAME("nos.compositing.LayoutOutputInfo")},
		{NOS_NAME("nos.utilities.layout.LayoutDrawItem"), NOS_NAME("nos.compositing.LayoutDrawItem")},
		{NOS_NAME("nos.utilities.layout.LayoutOutputInfo"), NOS_NAME("nos.compositing.LayoutOutputInfo")},
		{NOS_NAME("zd.utilities.BlendMode"), NOS_NAME("nos.compositing.BlendMode")},
		{NOS_NAME("zd.utilities.CanvasLayer"), NOS_NAME("nos.compositing.CanvasLayer")},
		{NOS_NAME("zd.utilities.MixerChannelType"), NOS_NAME("nos.compositing.MixerChannelType")},
		{NOS_NAME("zd.utilities.MixerTransitionType"), NOS_NAME("nos.compositing.MixerTransitionType")},
		{NOS_NAME("zd.utilities.MixerTransitionTarget"), NOS_NAME("nos.compositing.MixerTransitionTarget")},
		{NOS_NAME("zd.utilities.TransitionType"), NOS_NAME("nos.compositing.TransitionType")},
		{NOS_NAME("zd.utilities.TransitionInterpolation"), NOS_NAME("nos.compositing.TransitionInterpolation")},
		{NOS_NAME("zd.utilities.TransitionTarget"), NOS_NAME("nos.compositing.TransitionTarget")},
	};

	if (!outFrom)
	{
		*outSize = renames.size();
		return;
	}

	for (size_t i = 0; i < renames.size(); ++i)
	{
		outFrom[i] = renames[i].first;
		outTo[i] = renames[i].second;
	}
}

void GetRenamedNodeClasses(nosName* outFrom, nosName* outTo, size_t* outSize)
{
	static std::vector<std::pair<nos::Name, nos::Name>> renames = {
		{NOS_NAME("nos.utilities.BoxFit"), NOS_NAME("nos.compositing.BoxFit")},
		{NOS_NAME("nos.utilities.ChannelViewer"), NOS_NAME("nos.compositing.ChannelViewer")},
		{NOS_NAME("nos.utilities.Merge"), NOS_NAME("nos.compositing.Merge")},
		{NOS_NAME("nos.utilities.QuadMerge"), NOS_NAME("nos.compositing.QuadMerge")},
		{NOS_NAME("nos.utilities.Swizzle"), NOS_NAME("nos.compositing.Swizzle")},
		{NOS_NAME("nos.utilities.Layout"), NOS_NAME("nos.compositing.Layout")},
		{NOS_NAME("nos.utilities.LayoutDrawer"), NOS_NAME("nos.compositing.LayoutDrawer")},
		{NOS_NAME("nos.utilities.FreeLayout"), NOS_NAME("nos.compositing.FreeLayout")},
		{NOS_NAME("nos.utilities.GridLayout"), NOS_NAME("nos.compositing.GridLayout")},
		{NOS_NAME("nos.utilities.FreeOutputLayout"), NOS_NAME("nos.compositing.FreeOutputLayout")},
		{NOS_NAME("nos.utilities.GridOutputLayout"), NOS_NAME("nos.compositing.GridOutputLayout")},
		{NOS_NAME("nos.utilities.LayoutQuadCanvas"), NOS_NAME("nos.compositing.LayoutQuadCanvas")},
		{NOS_NAME("nos.utilities.TextureSwitcher"), NOS_NAME("nos.compositing.TextureSwitcher")},
		{NOS_NAME("nos.utilities.Offset"), NOS_NAME("nos.compositing.Offset")},
		{NOS_NAME("nos.utilities.SplitWipe"), NOS_NAME("nos.compositing.SplitWipe")},
		{NOS_NAME("zd.utilities.CanvasMapper"), NOS_NAME("nos.compositing.CanvasMapper")},
		{NOS_NAME("zd.utilities.Grid3D"), NOS_NAME("nos.compositing.Grid3D")},
		{NOS_NAME("zd.utilities.Mask"), NOS_NAME("nos.compositing.Mask")},
		{NOS_NAME("zd.utilities.Mixer"), NOS_NAME("nos.compositing.Mixer")},
		{NOS_NAME("zd.utilities.TextureMapper"), NOS_NAME("nos.compositing.TextureMapper")},
		{NOS_NAME("zd.utilities.TextureTransition"), NOS_NAME("nos.compositing.TextureTransition")},
	};

	if (!outFrom)
	{
		*outSize = renames.size();
		return;
	}

	for (size_t i = 0; i < renames.size(); ++i)
	{
		outFrom[i] = renames[i].first;
		outTo[i] = renames[i].second;
	}
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* out)
{
	out->ExportNodeFunctions = ExportNodeFunctions;
	out->GetRenamedTypes = GetRenamedTypes;
	out->GetRenamedNodeClasses = GetRenamedNodeClasses;
	return NOS_RESULT_SUCCESS;
}
}

} // namespace nos::compositing
