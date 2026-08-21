// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/nosVulkanSubsystem.h>

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::flow
{
void RegisterAlwaysDirty(nosNodeFunctions*);
void RegisterConditionalTrigger(nosNodeFunctions*);
void RegisterCPUSleep(nosNodeFunctions*);
void RegisterMultiLiveOut(nosNodeFunctions*);
void RegisterPropagateExecution(nosNodeFunctions*);
void RegisterRepeatingJunction(nosNodeFunctions*);
void RegisterScheduleOnRequest(nosNodeFunctions*);
void RegisterScheduleRequest(nosNodeFunctions*);
void RegisterShowStatusNode(nosNodeFunctions*);
void RegisterSwitchTrigger(nosNodeFunctions*);
void RegisterSyncMultiOutlet(nosNodeFunctions*);
void RegisterTriggerOnAnyInput(nosNodeFunctions*);
void RegisterToggleNode(nosNodeFunctions*);
}

namespace nos::flow
{
static void RegisterShowStatus(nosNodeFunctions* nodeFunctions)
{
	RegisterShowStatusNode(nodeFunctions);
}

static void RegisterToggle(nosNodeFunctions* nodeFunctions)
{
	RegisterToggleNode(nodeFunctions);
}

enum class Nodes : size_t
{
	AlwaysDirty,
	ConditionalTrigger,
	CPUSleep,
	MultiLiveOut,
	PropagateExecution,
	RepeatingJunction,
	ScheduleOnRequest,
	ScheduleRequest,
	ShowStatus,
	SwitchTrigger,
	SyncMultiOutlet,
	TriggerOnAnyInput,
	Toggle,
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

	for (size_t i = 0; i < static_cast<size_t>(Nodes::Count); ++i)
	{
		auto* node = outList[i];
		switch (static_cast<Nodes>(i))
		{
		default:
			break;
			GEN_CASE_NODE(AlwaysDirty)
			GEN_CASE_NODE(ConditionalTrigger)
			GEN_CASE_NODE(CPUSleep)
			GEN_CASE_NODE(MultiLiveOut)
			GEN_CASE_NODE(PropagateExecution)
			GEN_CASE_NODE(RepeatingJunction)
			GEN_CASE_NODE(ScheduleOnRequest)
			GEN_CASE_NODE(ScheduleRequest)
			GEN_CASE_NODE(ShowStatus)
			GEN_CASE_NODE(SwitchTrigger)
			GEN_CASE_NODE(SyncMultiOutlet)
			GEN_CASE_NODE(TriggerOnAnyInput)
			GEN_CASE_NODE(Toggle)
		}
	}

#undef GEN_CASE_NODE

	return NOS_RESULT_SUCCESS;
}

void GetRenamedTypes(nosName* outFrom, nosName* outTo, size_t* outSize)
{
static std::vector<std::pair<nos::Name, nos::Name>> renames = {
		{NOS_NAME("nos.utilities.ScheduleRequestMode"), NOS_NAME("nos.flow.ScheduleRequestMode")},
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
		{NOS_NAME("zd.utilities.Toggle"), NOS_NAME("nos.flow.Toggle")},
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

} // namespace nos::flow
