﻿#include <Nodos/PluginHelpers.hpp>

// External
#include <glm/glm.hpp> // TODO: Ring no longer needs glm::mat4 colormatrix. Remove this
#include <nosVulkanSubsystem/Helpers.hpp>

#include "Ring.h"

NOS_REGISTER_NAME(Size)
NOS_REGISTER_NAME(Spare)

namespace nos::utilities
{
struct BufferRingNodeContext : NodeContext
{
	enum class RingMode
	{
		CONSUME,
		FILL,
	};

	std::unique_ptr<TRing<nosBufferInfo>> Ring = nullptr;
	std::atomic_uint32_t SpareCount = 0;
	std::atomic<RingMode> Mode = RingMode::FILL;

	BufferRingNodeContext(const nosFbNode* node) : NodeContext(node)
	{
		Ring = std::make_unique<TRing<nosBufferInfo>>(
			1, 1, nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST));
		Ring->Stop();
		AddPinValueWatcher(NSN_Size, [this](nos::Buffer const& newSize, nos::Buffer const& oldSize) {
			uint32_t size = *newSize.As<uint32_t>();
			if (Ring->Size != size)
			{
				Ring->Resize(size);
				nosPathCommand ringSizeChange{.Event = NOS_RING_SIZE_CHANGE, .RingSize = size};
				nosEngine.SendPathCommand(PinName2Id[NOS_NAME_STATIC("Input")], ringSizeChange);
				SendPathRestart();
			}
		});
		AddPinValueWatcher(NOS_NAME_STATIC("Input"), [this](nos::Buffer const& newBuf, nos::Buffer const& oldBuf) {
			auto info = vkss::ConvertToResourceInfo(*InterpretPinValue<sys::vulkan::Buffer>(newBuf.Data()));
			if (Ring->Sample.Size != info.Info.Buffer.Size)
			{
				Ring = std::make_unique<TRing<nosBufferInfo>>(
					Ring->Size,
					info.Info.Buffer.Size,
					nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST));
				Ring->Stop();
				SendPathRestart();
			}
		});
	}

	void SendRingStats() const
	{
		nosEngine.WatchLog(("Ring: " + NodeName.AsString() + " Read Size").c_str(), std::to_string(Ring->Read.Pool.size()).c_str());
		nosEngine.WatchLog(("Ring: " + NodeName.AsString() + " Write Size").c_str(), std::to_string(Ring->Write.Pool.size()).c_str());
		nosEngine.WatchLog(("Ring: " + NodeName.AsString() + " Total Frame Count").c_str(), std::to_string(Ring->TotalFrameCount()).c_str());
	}

	nosResult ExecuteNode(const nosNodeExecuteArgs* args) override
	{
		if (Ring->Exit)
			return NOS_RESULT_FAILED;
		NodeExecuteArgs pins(args);
		auto ringSize = *pins.GetPinData<uint32_t>(NSN_Size);
		ringSize = std::max(1u, ringSize);
		SpareCount = *pins.GetPinData<uint32_t>(NSN_Spare);
		if (SpareCount >= ringSize)
		{
			uint32_t newSpareCount = ringSize - 1; 
			SpareCount = newSpareCount;
			nosEngine.LogW("Spare count must be less than ring size! Capping spare count at %u.", newSpareCount);
			nosEngine.SetPinValueByName(NodeId, NSN_Spare, nosBuffer{.Data = &newSpareCount, .Size = sizeof(newSpareCount)});
		}
		auto* output = pins.GetPinData<sys::vulkan::Buffer>(NOS_NAME_STATIC("Output"));
		nosResourceShareInfo input = vkss::ConvertToResourceInfo(*pins.GetPinData<sys::vulkan::Buffer>(NOS_NAME_STATIC("Input")));
		if (!input.Memory.Handle)
			return NOS_RESULT_SUCCESS;

		if (Ring->IsFull())
		{
			nosEngine.LogI("Trying to push while ring is full");
		}

		auto slot = Ring->BeginPush();
		// TODO: FieldType
		slot->FrameNumber = args->FrameNumber;
		nosCmd cmd;
		nosVulkan->Begin("nos.aja.BufferRing: Input Buffer Copy To Ring Slot", &cmd);
		nosVulkan->Copy(cmd, &input, &slot->Res, 0);
		nosCmdEndParams end{.ForceSubmit = NOS_TRUE, .OutGPUEventHandle = &slot->Params.WaitEvent};
		nosVulkan->End(cmd, &end);
		nosVulkan->WaitGpuEvent(&slot->Params.WaitEvent, UINT64_MAX);
		Ring->EndPush(slot);
		if (Mode == RingMode::FILL && Ring->IsFull())
			Mode = RingMode::CONSUME;
		return NOS_RESULT_SUCCESS;
	}

	// Called from a different thread.
	nosResult CopyFrom(nosCopyInfo* cpy) override
	{
		if (!Ring || Ring->Exit)
			return NOS_RESULT_FAILED;
		SendRingStats();
		if (Mode == RingMode::FILL && Ring->HasEmptySlots())
			return NOS_RESULT_PENDING;

		auto outputBufferDesc = static_cast<sys::vulkan::Buffer*>(cpy->PinData->Data);
		auto output = vkss::ConvertToResourceInfo(*outputBufferDesc);
		auto effectiveSpareCount = SpareCount.load(); // TODO: * (1 + u32(th->Interlaced()));
		auto* slot = Ring->TryPop(cpy->FrameNumber, effectiveSpareCount);
		if (!slot)
			return Ring->Exit ? NOS_RESULT_FAILED : NOS_RESULT_PENDING;
		if (slot->Res.Info.Buffer.Size != output.Info.Buffer.Size)
		{
			output.Info.Type = NOS_RESOURCE_TYPE_BUFFER;
			output.Info.Buffer = slot->Res.Info.Buffer;
			nosEngine.SetPinValueByName(NodeId, NOS_NAME_STATIC("Output"), Buffer::From(vkss::ConvertBufferInfo(output)));
			outputBufferDesc = static_cast<sys::vulkan::Buffer*>(cpy->PinData->Data);
			output = vkss::ConvertToResourceInfo(*outputBufferDesc);
		}
		nosCmd cmd;

		nosVulkan->Begin("nos.aja.BufferRing: Ring Slot Copy To Output Buffer", &cmd);
		nosVulkan->Copy(cmd, &slot->Res, &output, 0);
		nosCmdEndParams end{ .ForceSubmit = NOS_TRUE, .OutGPUEventHandle = &slot->Params.WaitEvent };
		nosVulkan->End(cmd, &end);
		nosVulkan->WaitGpuEvent(&slot->Params.WaitEvent, UINT64_MAX);
		Ring->EndPop(slot);
		SendScheduleRequest(1);
		return NOS_RESULT_SUCCESS;
	}

	void OnPathCommand(const nosPathCommand* command) override
	{
		switch (command->Event)
		{
		case NOS_RING_SIZE_CHANGE: {
			nosEngine.SetPinValue(*GetPinId(NSN_Size), nos::Buffer::From(command->RingSize));
			break;
		}
		default: return;
		}
	}

	void SendScheduleRequest(uint32_t count, bool reset = false) const
	{
		nosScheduleNodeParams schedule {
			.NodeId = NodeId,
			.AddScheduleCount = count,
			.Reset = reset
		};
		nosEngine.ScheduleNode(&schedule);
	}

	void OnPathStop() override
	{
		Mode = RingMode::FILL;
		if (Ring)
			Ring->Stop();
	}

	void OnPathStart() override
	{
		uint32_t ringSize = *InterpretPinValue<uint32_t>(*GetWatchedPinValue(NSN_Size));
		if (Ring)
			ringSize = Ring->Write.Pool.size();
		nosScheduleNodeParams schedule{.NodeId = NodeId, .AddScheduleCount = ringSize, .Reset = true};
		nosEngine.ScheduleNode(&schedule);
		if (Ring)
			Ring->Exit = false;
	}

	void SendPathRestart()
	{
		nosEngine.SendPathRestart(PinName2Id[NOS_NAME_STATIC("Input")]);
	}
};

nosResult RegisterBufferRing(nosNodeFunctions* functions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("BufferRing"), BufferRingNodeContext, functions)
	return NOS_RESULT_SUCCESS;
}

}