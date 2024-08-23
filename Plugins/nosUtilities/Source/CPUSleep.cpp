// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <thread>
namespace nos::utilities
{
NOS_REGISTER_NAME(BusyWait);
NOS_REGISTER_NAME(WaitTimeMS);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_CPUSleep, "nos.utilities.CPUSleep")
struct CPUSleepNode : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto pins = GetPinValues(params);
		bool busyWait = *GetPinValue<bool>(pins, NSN_BusyWait);
		auto milliseconds = *GetPinValue<double>(pins, NSN_WaitTimeMS);
		if (busyWait)
		{
			auto end = std::chrono::high_resolution_clock::now() + std::chrono::nanoseconds(uint64_t(milliseconds * 1.0e6));
			bool isPreempted = false;
			while (nosEngine.IsRunnerThreadPreempted(&isPreempted) == NOS_RESULT_SUCCESS && !isPreempted && 
				std::chrono::high_resolution_clock::now() < end)
				;
		}
		else
			nosEngine.WaitFor(milliseconds);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterCPUSleep(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_CPUSleep, CPUSleepNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities