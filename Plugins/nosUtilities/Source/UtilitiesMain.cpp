// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::utilities
{
enum class Utilities : size_t
{
	Host,
	PrintLog,
	Sink,
	Time,
	Counter,
	TextRender,
	Count,
};

void RegisterHost(nosNodeFunctions*);
void RegisterPrintLog(nosNodeFunctions*);
void RegisterSink(nosNodeFunctions*);
void RegisterTime(nosNodeFunctions*);
nosResult RegisterCounter(nosNodeFunctions*);
nosResult RegisterTextRender(nosNodeFunctions*);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
	if (outSize)
		*outSize = static_cast<size_t>(Utilities::Count);
	if (!outList)
		return NOS_RESULT_SUCCESS;

	RegisterHost(outList[static_cast<size_t>(Utilities::Host)]);
	RegisterPrintLog(outList[static_cast<size_t>(Utilities::PrintLog)]);
	RegisterSink(outList[static_cast<size_t>(Utilities::Sink)]);
	RegisterTime(outList[static_cast<size_t>(Utilities::Time)]);
	RegisterCounter(outList[static_cast<size_t>(Utilities::Counter)]);
	RegisterTextRender(outList[static_cast<size_t>(Utilities::TextRender)]);
	return NOS_RESULT_SUCCESS;
}

void GetRenamedTypes(nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outSize)
{
	static std::vector<std::pair<nos::Name, nos::Name>> renames = {
		{NOS_NAME("nos.fb.ChannelViewerChannels"), NOS_NAME("nos.compositing.ChannelViewerChannels")},
		{NOS_NAME("nos.fb.ChannelViewerFormats"), NOS_NAME("nos.compositing.ChannelViewerFormats")},
		{NOS_NAME("nos.fb.GradientKind"), NOS_NAME("nos.utilities.GradientKind")},
		{NOS_NAME("nos.fb.BlendMode"), NOS_NAME("nos.compositing.BlendMode")},
		{NOS_NAME("nos.fb.ResizeMethod"), NOS_NAME("nos.imageprocessing.ResizeMethod")},
		{NOS_NAME("nos.fb.Source"), NOS_NAME("nos.compositing.Source")},
		{NOS_NAME("nos.fb.Channel"), NOS_NAME("nos.compositing.Channel")},
		{NOS_NAME("nos.plugin.switcher.TextureSwitcherChannel"), NOS_NAME("nos.compositing.TextureSwitcherChannel")},
		{NOS_NAME("nos.utilities.ChannelViewerChannels"), NOS_NAME("nos.compositing.ChannelViewerChannels")},
		{NOS_NAME("nos.utilities.ChannelViewerFormats"), NOS_NAME("nos.compositing.ChannelViewerFormats")},
		{NOS_NAME("nos.utilities.BoxFitMode"), NOS_NAME("nos.compositing.BoxFitMode")},
		{NOS_NAME("nos.utilities.BlendMode"), NOS_NAME("nos.compositing.BlendMode")},
		{NOS_NAME("nos.utilities.Source"), NOS_NAME("nos.compositing.Source")},
		{NOS_NAME("nos.utilities.Channel"), NOS_NAME("nos.compositing.Channel")},
		{NOS_NAME("nos.utilities.TextureSwitcherChannel"), NOS_NAME("nos.compositing.TextureSwitcherChannel")},
		{NOS_NAME("nos.utilities.ResizeMethod"), NOS_NAME("nos.imageprocessing.ResizeMethod")},
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
		{NOS_NAME("nos.utilities.AutoResize"), NOS_NAME("nos.imageprocessing.AutoResize")},
		{NOS_NAME("nos.utilities.Resize"), NOS_NAME("nos.imageprocessing.Resize")},
		{NOS_NAME("nos.utilities.YADIF"), NOS_NAME("nos.filters.YADIF")},
		{NOS_NAME("nos.utilities.YADIFWithAutoDispatchSize"), NOS_NAME("nos.filters.YADIFWithAutoDispatchSize")},
		{NOS_NAME("nos.utilities.ReduceTexture"), NOS_NAME("nos.imageprocessing.ReduceTexture")},
		{NOS_NAME("nos.utilities.MeanSquaredError"), NOS_NAME("nos.noise.MeanSquaredError")},
		{NOS_NAME("nos.utilities.PSNR"), NOS_NAME("nos.noise.PSNR")},
		{NOS_NAME("nos.utilities.StbiLoad"), NOS_NAME("nos.mediaio.StbiLoad")},
		{NOS_NAME("nos.utilities.WriteImage"), NOS_NAME("nos.mediaio.WriteImage")},
		{NOS_NAME("nos.utilities.LoadCubeLUT"), NOS_NAME("nos.mediaio.LoadCubeLUT")},
		{NOS_NAME("nos.utilities.ReadImage"), NOS_NAME("nos.mediaio.ReadImage")},
		{NOS_NAME("nos.utilities.AlwaysDirty"), NOS_NAME("nos.flow.AlwaysDirty")},
		{NOS_NAME("nos.utilities.ConditionalTrigger"), NOS_NAME("nos.flow.ConditionalTrigger")},
		{NOS_NAME("nos.utilities.ExecDepend"), NOS_NAME("nos.flow.ExecDepend")},
		{NOS_NAME("nos.utilities.CPUSleep"), NOS_NAME("nos.flow.CPUSleep")},
		{NOS_NAME("nos.utilities.MultiLiveOut"), NOS_NAME("nos.flow.MultiLiveOut")},
		{NOS_NAME("nos.utilities.PropagateExecution"), NOS_NAME("nos.flow.PropagateExecution")},
		{NOS_NAME("nos.utilities.RepeatingJunction"), NOS_NAME("nos.flow.RepeatingJunction")},
		{NOS_NAME("nos.utilities.ScheduleOnRequest"), NOS_NAME("nos.flow.ScheduleOnRequest")},
		{NOS_NAME("nos.utilities.ScheduleRequest"), NOS_NAME("nos.flow.ScheduleRequest")},
		{NOS_NAME("nos.utilities.ShowStatus"), NOS_NAME("nos.flow.ShowStatus")},
		{NOS_NAME("nos.utilities.ShowStatusNode"), NOS_NAME("nos.flow.ShowStatus")},
		{NOS_NAME("nos.utilities.SwitchTrigger"), NOS_NAME("nos.flow.SwitchTrigger")},
		{NOS_NAME("nos.utilities.SyncMultiOutlet"), NOS_NAME("nos.flow.SyncMultiOutlet")},
		{NOS_NAME("nos.utilities.ThreadedSyncMultiOutlet"), NOS_NAME("nos.flow.ThreadedSyncMultiOutlet")},
		{NOS_NAME("nos.utilities.TimedFunctionSignaller"), NOS_NAME("nos.flow.TimedFunctionSignaller")},
		{NOS_NAME("nos.utilities.TriggerOnAnyInput"), NOS_NAME("nos.flow.TriggerOnAnyInput")},
		{NOS_NAME("nos.utilities.AsyncDownloadBuffer"), NOS_NAME("nos.resource.AsyncDownloadBuffer")},
		{NOS_NAME("nos.utilities.BoundedQueue"), NOS_NAME("nos.resource.BoundedQueue")},
		{NOS_NAME("nos.utilities.BoundedTextureQueue"), NOS_NAME("nos.resource.BoundedTextureQueue")},
		{NOS_NAME("nos.utilities.Buffer2Texture"), NOS_NAME("nos.resource.Buffer2Texture")},
		{NOS_NAME("nos.utilities.BufferProvider"), NOS_NAME("nos.resource.BufferProvider")},
		{NOS_NAME("nos.utilities.CalculateDispatchSize"), NOS_NAME("nos.resource.CalculateDispatchSize")},
		{NOS_NAME("nos.utilities.CopyResource"), NOS_NAME("nos.resource.CopyResource")},
		{NOS_NAME("nos.utilities.DeinterlacedBoundedTextureQueue"), NOS_NAME("nos.resource.DeinterlacedBoundedTextureQueue")},
		{NOS_NAME("nos.utilities.DeinterlacedBufferRing"), NOS_NAME("nos.resource.DeinterlacedBufferRing")},
		{NOS_NAME("nos.utilities.HostVisibleBufferCopy"), NOS_NAME("nos.resource.HostVisibleBufferCopy")},
		{NOS_NAME("nos.utilities.RingBuffer"), NOS_NAME("nos.resource.RingBuffer")},
		{NOS_NAME("nos.utilities.Texture2Buffer"), NOS_NAME("nos.resource.Texture2Buffer")},
		{NOS_NAME("nos.utilities.TextureProvider"), NOS_NAME("nos.resource.TextureProvider")},
		{NOS_NAME("nos.utilities.UploadBuffer"), NOS_NAME("nos.resource.UploadBuffer")},
		{NOS_NAME("nos.utilities.UploadBufferProvider"), NOS_NAME("nos.resource.UploadBufferProvider")},
		{NOS_NAME("nos.utilities.WaitGPUEvent"), NOS_NAME("nos.sync.WaitGPUEvent")},
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

} // namespace nos::utilities
