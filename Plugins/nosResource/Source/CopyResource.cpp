// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>

#include <nosSysVulkan/Helpers.hpp>

namespace nos::resource
{
struct CopyResourceNode : NodeContext
{
	using NodeContext::NodeContext;

	std::optional<nos::TypeInfo> TypeInfo;

	nosResult OnCreate(nosFbNodePtr node) override
	{
		for (auto pin : *node->pins())
		{
			if (pin->name()->string_view() == NOS_NAME("Source"))
				if (pin->type_name()->string_view() != NOS_NAME(nos::Generic::GetFullyQualifiedName()))
					TypeInfo = nos::TypeInfo(nos::Name(pin->type_name()->string_view()));
		}
		return NOS_RESULT_SUCCESS;
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		if (!(params->IncomingTypeName == NOS_NAME(nos::sys::vulkan::Texture::GetFullyQualifiedName()) ||
			  params->IncomingTypeName == NOS_NAME(nos::sys::vulkan::Buffer::GetFullyQualifiedName())))
		{
			strncpy(params->OutErrorMessage, "CopyResource only supports Vulkan Texture or Buffer types", 58);
			return NOS_RESULT_FAILED;
		}

		TypeInfo = nos::TypeInfo(params->IncomingTypeName);

		for (size_t i = 0; i < params->PinCount; i++)
		{
			auto& pinInfo = params->Pins[i];
			if (pinInfo.Name == NOS_NAME("Source")
				|| pinInfo.Name == NOS_NAME("Destination")
				|| pinInfo.Name == NOS_NAME("OutDestination"))
				pinInfo.OutResolvedTypeName = TypeInfo->TypeName;
		}

		return NOS_RESULT_SUCCESS;
	}


	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		auto source = params.GetPinObject(NOS_NAME("Source"));
		auto destination = params.GetPinObject(NOS_NAME("Destination"));
		auto inEventHolder = params.GetPinObject<sys::vulkan::GPUEventHolder>(NOS_NAME("InGPUEventHolder"));
		bool preferTransferQueue = *params.GetPinValue<bool>(NOS_NAME("PreferTransferQueue"));
		bool forceSubmit = *params.GetPinValue<bool>(NOS_NAME("ForceSubmit"));
		nosCmd cmd{};
		nosCmdBeginParams beginParams{
			.Name = NOS_NAME("Resource Copy"),
			.AssociatedNodeId = NodeId,
			.OutCmdHandle = &cmd,
			.PreferredQueueType = preferTransferQueue ? NOS_CMD_QUEUE_TYPE_TRANSFER : NOS_CMD_QUEUE_TYPE_MAIN,
		};
		nosVulkan->Begin(&beginParams);
		nosVulkan->Copy(cmd, source, destination, nullptr);
		nosCmdEndParams endParams{.ForceSubmit = forceSubmit};
		if (inEventHolder)
		{
			nosGPUEvent* inEvent = nullptr;
			nosVulkan->GetGPUEventFromHolder(inEventHolder, &inEvent);
			if (inEvent)
			{
				endParams.ForceSubmit = NOS_TRUE;
				endParams.OutGPUEventHandle = inEvent;
			}
		}
		nosVulkan->End(cmd, &endParams);
		SetPinObject(NOS_NAME("OutGPUEventHolder"), inEventHolder);
		SetPinObject(NOS_NAME("OutDestination"), destination);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterCopyResource(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("CopyResource"), CopyResourceNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::resource
