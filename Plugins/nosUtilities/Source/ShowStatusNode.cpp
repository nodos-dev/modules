// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

namespace nos::utilities
{
	NOS_REGISTER_NAME_SPACED(Nos_Utilities_ShowStatus, "nos.utilities.ShowStatus")
	struct ShowStatus : NodeContext
	{
		using NodeContext::NodeContext;

		nosResult ExecuteNode(nosNodeExecuteArgs const* args) override
		{
			NodeExecuteArgs execArgs(args);
			std::string statusMessage = "";
			if(execArgs[NOS_NAME("Status")].Data->Size > 0)
				statusMessage = std::string(reinterpret_cast<const char*>(execArgs[NOS_NAME("Status")].Data->Data));
			fb::NodeStatusMessageType statusType = *execArgs.GetPinData<fb::NodeStatusMessageType>(NOS_NAME("StatusType"));
			if (StatusMessage == statusMessage && StatusType == statusType)
			{
				return NOS_RESULT_SUCCESS;
			}
			StatusMessage = statusMessage;
			StatusType = statusType;
			if (statusMessage.empty())
				ClearNodeStatusMessages();
			else
				SetNodeStatusMessage(std::move(statusMessage), statusType);
			return NOS_RESULT_SUCCESS;
		}

		std::string StatusMessage;
		fb::NodeStatusMessageType StatusType;
	};


	nosResult RegisterShowStatusNode(nosNodeFunctions* fn)
	{
		NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_ShowStatus, ShowStatus, fn);
		return NOS_RESULT_SUCCESS;
	}

} // namespace nos