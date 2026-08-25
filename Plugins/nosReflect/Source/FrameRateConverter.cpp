#include "RingBuffer.hpp"

namespace nos::reflect
{

struct FrameRateConverterNode : NodeContext
{
	struct Slot
	{
		ObjectRef Object;
		uint64_t FrameNumber = 0;
	};

	nos::Name TypeName = NSN_TypeNameGeneric;

	RingBufferServeMode Mode = RingBufferServeMode::WaitUntilFull;
	RingBuffer<Slot> Ring;
	uint32_t Capacity = 1;
	uint32_t EffectiveCapacity = 1;
	uint32_t RemainingRepeatCount = 0;
	fb::vec2u Ratio = {1, 1};
	uint32_t PendingScheduleRemainder = 0;

	enum class StatusType
	{
		Ratio,
		Capacity,
		Mode,
		Repeating
	};

	std::unordered_map<StatusType, fb::TNodeStatusMessage> StatusMessages;

	bool CapacityUpdatedViaPathCommand = false;

	FrameRateConverterNode() : Ring(1, Mode)
	{
		Ring.Reset(EffectiveCapacity, Mode);
		AddPinValueWatcher<uint32_t>(NOS_NAME("Capacity"), std::bind(&FrameRateConverterNode::OnCapacityPinValueChanged, this, std::placeholders::_1, std::placeholders::_2));
		AddPinValueWatcher<fb::vec2u>(NOS_NAME("Ratio"), std::bind(&FrameRateConverterNode::OnRatioPinValueChanged, this, std::placeholders::_1, std::placeholders::_2));
		AddPinValueWatcher<RingBufferServeMode>(NOS_NAME("Mode"), std::bind(&FrameRateConverterNode::OnModePinValueChanged, this, std::placeholders::_1, std::placeholders::_2));
	}

	void OnRatioPinValueChanged(fb::vec2u const* newRatio, std::optional<fb::vec2u const*> oldRatio)
	{
		if (*newRatio == Ratio)
			return;
		if (newRatio->x() == 0 || newRatio->y() == 0)
		{
			nosEngine.LogW("%s: Ratio components cannot be 0.", GetItemPath(NodeId).value_or("<unknown>").c_str());
			SetPinValue(NOS_NAME("Ratio"), Ratio);
			return;
		}
		Ratio = *newRatio;
		auto lcm = std::lcm(Ratio.x(), Ratio.y());
		auto requiredEffectiveCapacity = lcm * Capacity;
		if (requiredEffectiveCapacity != EffectiveCapacity)
		{
			EffectiveCapacity = requiredEffectiveCapacity;
		}
		SendPathRestart(NodeId);
	}

	void OnCapacityPinValueChanged(uint32_t const* newCapacity, std::optional<uint32_t const*> oldCapacity)
	{
		const bool updatedViaPathCommand = CapacityUpdatedViaPathCommand;
		CapacityUpdatedViaPathCommand = false;
		if (*newCapacity == Capacity)
			return;
		if (*newCapacity == 0)
		{
			nosEngine.LogW("%s: Capacity cannot be 0.", GetItemPath(NodeId).value_or("<unknown>").c_str());
			SetPinValue(NOS_NAME("Capacity"), 1u);
			return;
		}
		Capacity = *newCapacity;
		EffectiveCapacity = std::lcm(Ratio.x(), Ratio.y()) * Capacity;
		if (!updatedViaPathCommand)
		{
			nosPathCommand ringSizeChange{.Event = NOS_RING_SIZE_CHANGE, .RingSize = Capacity};
			nosEngine.SendPathCommand(*GetPinId(NSN_Input), ringSizeChange);
		}
		SendPathRestart(NSN_Input);
	}

	void OnModePinValueChanged(RingBufferServeMode const* newMode, std::optional<RingBufferServeMode const*> oldMode)
	{
		if (*newMode != Mode)
		{
			Mode = *newMode;
			SendPathRestart(NodeId);
		}
	}

	void SendRingStats(const char* state) const
	{
		auto nodeName = NodeName.AsString();
		nosEngine.WatchLog((nodeName + " Size").c_str(), std::to_string(Ring.GetCurrentSize()).c_str());
		nosEngine.WatchLog((nodeName + " Capacity").c_str(), std::to_string(Ring.GetCapacity()).c_str());
		nosEngine.WatchLog((nodeName + " State").c_str(), state);
	}

	void OnPathStart() override
	{
		nosEngine.LogD("%s: Effective Capacity set to %u", GetItemPath(NodeId).value_or("<unknown>").c_str(), EffectiveCapacity);
		SetStatus(StatusType::Capacity,
				  "Capacity: " + std::to_string(Capacity) + " (Effective: " + std::to_string(EffectiveCapacity) + ")",
				  fb::NodeStatusMessageType::INFO);
		SetStatus(StatusType::Mode, "Starting Mode: " + std::string(Mode == RingBufferServeMode::WaitUntilFull ? "Wait Until Full" : "Serve Immediately"), fb::NodeStatusMessageType::INFO);
		Ring.Reset(EffectiveCapacity, Mode);
		auto producerExecCount = EffectiveCapacity / Ratio.x();
		auto consumerExecCount = EffectiveCapacity / Ratio.y();
		RemainingRepeatCount = consumerExecCount - 1;
		PendingScheduleRemainder = 0;
		SendScheduleRequest(producerExecCount);
	}

	void OverrideConsumerDeltaSeconds(nosVec2u& inoutDeltaSeconds) override
	{
		auto consumerDeltaSeconds = inoutDeltaSeconds;
		// My producer arm will run Y frames producing X objects and consumer arm will run X frames consuming Y objects.
		// Calculating the delta-seconds of my producer arm:
		auto producerDeltaSeconds = nosVec2u{consumerDeltaSeconds.x * Ratio.x(), consumerDeltaSeconds.y * Ratio.y()};
		// Override it:
		inoutDeltaSeconds = producerDeltaSeconds;
	}

	void OnPathStop() override { Ring.Shutdown(); }

	nosResult OnCreate(nosFbNodePtr node) override
	{
		for (auto pin : *node->pins())
		{
			auto name = nos::Name(pin->name()->c_str());
			if (NSN_Output == name)
			{
				if (pin->type_name()->c_str() == NSN_TypeNameGeneric.AsString())
					continue;
				SetType(nos::Name(pin->type_name()->c_str()));
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	nosResult CopyFrom(nosCopyFromInfo* cpy) override
	{
		SendRingStats("Pre Begin Pop");
		if (Ring.GetMode() == RingBufferServeMode::WaitUntilFull)
		{
			if (RemainingRepeatCount > 0)
			{
				SetStatus(StatusType::Repeating,
						  "Repeating: " + std::to_string(RemainingRepeatCount) + " repeats remaining",
						  fb::NodeStatusMessageType::WARNING);
				--RemainingRepeatCount;
				return NOS_RESULT_SUCCESS;
			}
		}
		ClearStatus(StatusType::Repeating);
		std::vector<ObjectRef> outputObjectRefs;
		uint64_t frameNumber;
		uint32_t popCount = Ratio.y();
		std::optional<std::vector<Slot*>> maybeSrcSlots;
		{
			ScopedProfilerEvent _({.Name = "Wait For Read"});
			maybeSrcSlots = Ring.BeginPop(popCount, 100);
		}
		if (maybeSrcSlots)
		{
			auto& srcSlots = *maybeSrcSlots;
			for (auto& srcSlot : srcSlots)
				outputObjectRefs.push_back(std::move(srcSlot->Object));
			frameNumber = srcSlots[0]->FrameNumber;
			Ring.EndPop(popCount);
			SendRingStats("Post Begin Pop");
		}
		else if (Ring.IsShuttingDown())
		{
			return NOS_RESULT_FAILED;
		}
		else
		{
			// Timeout
			return NOS_RESULT_PENDING;
		}
		if (!outputObjectRefs.empty())
		{
			// Convert ObjectRefs to IDs for the API call
			std::vector<nosObjectId> outputObjects;
			outputObjects.reserve(outputObjectRefs.size());
			for (const auto& ref : outputObjectRefs)
				outputObjects.push_back(ref.GetObjectId());

			ObjectRef outputArrayObject;
			auto res = nosEngine.ObjectAPI->CreateArrayObject(
				TypeName, outputObjects.data(), outputObjects.size(), &outputArrayObject.GetStorage());
			if (res != NOS_RESULT_SUCCESS)
				return res;
			SetPinObject(NSN_Output, outputArrayObject);
			cpy->ShouldSetSourceFrameNumber = true;
			cpy->FrameNumber = frameNumber;

			// Maintain the ratio by distributing extra frames across multiple schedules
			PendingScheduleRemainder += Ratio.y() % Ratio.x();
			uint32_t newCount = Ratio.y() / Ratio.x();
			newCount += PendingScheduleRemainder / Ratio.x();
			PendingScheduleRemainder = PendingScheduleRemainder % Ratio.x();
			if (newCount)
				SendScheduleRequest(newCount);
			return NOS_RESULT_SUCCESS;
		}
		return NOS_RESULT_PENDING;
	}

	void OnPathCommand(const nosPathCommand* command) override
	{
		if (command->Event != NOS_RING_SIZE_CHANGE)
			return;
		if (command->RingSize == 0)
		{
			nosEngine.LogW((GetDisplayName() + " capacity cannot be 0.").c_str());
			return;
		}
		if (command->RingSize == Capacity)
			return;
		CapacityUpdatedViaPathCommand = true;
		SetPinValue(NOS_NAME("Capacity"), command->RingSize);
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		TypeInfo typeInfo(params->IncomingTypeName);
		if (typeInfo->BaseType != NOS_BASE_TYPE_ARRAY)
		{
			strncpy(params->OutErrorMessage, "Connected pin type must be an array type!", 42);
			return NOS_RESULT_FAILED;
		}
		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(nosPinUpdate const* update) override
	{
		if (TypeName != NSN_TypeNameGeneric)
			return;
		if (update->UpdatedField == NOS_PIN_FIELD_TYPE_NAME)
		{
			if (update->PinName != NSN_Input)
				return;
			SetType(update->TypeName);
		}
	}

	void SetType(nos::Name typeName)
	{
		TypeName = typeName;
		SendPathRestart(NodeId);
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		if (NSN_TypeNameGeneric == TypeName)
			return NOS_RESULT_FAILED;
		ArrayObjectRef inputArrayObject = params.GetPinObject(NSN_Input);
		if (!inputArrayObject)
			return NOS_RESULT_FAILURE;

		SendRingStats("Pre Push");
		uint32_t pushCount = Ratio.x();
		std::optional<std::vector<Slot*>> maybeDstSlots;
		{
			ScopedProfilerEvent _({.Name = "Wait For Empty Slot"});
			maybeDstSlots = Ring.BeginPush(pushCount, 100);
		}
		if (maybeDstSlots)
		{
			auto& dstSlots = *maybeDstSlots;
			auto inSize = inputArrayObject.GetSize();
			if (inSize != pushCount)
			{
				SetStatus(StatusType::Ratio,
						  "Input array size (" + std::to_string(inSize) + ") does not match required input size (" +
							  std::to_string(pushCount) + ")!",
						  fb::NodeStatusMessageType::FAILURE);
				Ring.EndPush(pushCount);
				SendRingStats("Post Push");
				return NOS_RESULT_FAILED;
			}
			// TODO: Maybe a more understandable message here?
			SetStatus(StatusType::Ratio, "In " + std::to_string(Ratio.x()) + ":" + std::to_string(Ratio.y()) + " Out", fb::NodeStatusMessageType::INFO);
			uint32_t i = 0;
			for (auto& elem : inputArrayObject)
			{
				auto& dstSlot = dstSlots[i++];
				dstSlot->Object = elem;
				dstSlot->FrameNumber = params.FrameNumber;
			}
			Ring.EndPush(pushCount);
			SendRingStats("Post Push");
			return NOS_RESULT_SUCCESS;
		}
		if (Ring.IsShuttingDown())
			return NOS_RESULT_FAILED;
		// Timeout
		return NOS_RESULT_PENDING;
	}

	void SetStatus(StatusType statusType, std::string const& message, fb::NodeStatusMessageType messageType)
	{
		auto msg = fb::TNodeStatusMessage{{}, message, messageType};
		if (StatusMessages[statusType] != msg)
		{
			StatusMessages[statusType] = msg;
			UpdateStatus();
		}
	}

	void ClearStatus(StatusType statusType)
	{
		auto it = StatusMessages.find(statusType);
		if (it != StatusMessages.end())
		{
			StatusMessages.erase(it);
			UpdateStatus();
		}
	}

	void UpdateStatus()
	{
		std::vector<fb::TNodeStatusMessage> messages;
		for (auto const& [_, msg] : StatusMessages)
			messages.push_back(msg);
		SetNodeStatusMessages(messages);
	}
};

nosResult RegisterFrameRateConverter(nosNodeFunctions* node)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("FrameRateConverter"), FrameRateConverterNode, node)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::reflect