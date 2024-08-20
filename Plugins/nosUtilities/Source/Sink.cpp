// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

// stl
#include <chrono>
#include <nosUtil/Stopwatch.hpp>

namespace nos::utilities
{
using clock = std::chrono::high_resolution_clock;

struct SinkNode : NodeContext
{
	std::mutex Mutex;
	std::atomic<bool> ShouldStop = false;
	std::atomic<float> Fps = 1000.0f / 60.0f;
	std::atomic<bool> Wait = true;
	std::thread Thread;
	bool AcceptRepeat = false;
	clock::time_point LastCopy = clock::now();

	SinkNode(const nosFbNode* inNode) : NodeContext(inNode)
	{
		flatbuffers::FlatBufferBuilder fbb;
		Thread = std::thread([this]() { SinkThread(); });
		HandleEvent(CreateAppEvent(fbb, nos::app::CreateSetThreadNameDirect(fbb, (u64)Thread.native_handle(), "Sink Thread")));
	}

	~SinkNode() 
	{ 
		ShouldStop = true;
		Thread.join();
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto& lastCopy = LastCopy;

		if (nosVulkan)
		{
			nosCmd cmd{};
			nosCmdBeginParams beginParams = {.Name = NOS_NAME("Sink Submit"), .AssociatedNodeId = NodeId, .OutCmdHandle = &cmd};
			nosVulkan->Begin2(&beginParams);
			nosGPUEvent event{};
			nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = Wait ? &event : nullptr };
			nosVulkan->End(cmd, &endParams);
			if (Wait)
			{
				util::Stopwatch sw;
				nosVulkan->WaitGpuEvent(&event, 1000000000);
				nosEngine.WatchLog("Sink GPU Wait", sw.ElapsedString().c_str());
			}
		}

		lastCopy = clock::now();

		return NOS_RESULT_SUCCESS;
	};

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{
		if (NOS_NAME_STATIC("Sink FPS") == pinName)
		{
			Fps = *(float*)value.Data;
			nosEngine.RecompilePath(NodeId);
		}
		if (NOS_NAME_STATIC("Wait") == pinName)
		{
			Wait = *(bool*)value.Data;
		}
		if (NOS_NAME_STATIC("AcceptRepeat") == pinName)
		{
			AcceptRepeat = *(bool*)value.Data;
			nosEngine.RecompilePath(NodeId);
		}
	};

	void SinkThread()
	{
		clock::time_point lastCopy;
		while (!ShouldStop)
		{
			auto now = clock::now();

			float diff = std::chrono::duration_cast<std::chrono::microseconds>(now - lastCopy).count() / 1000.0f;
			if (diff < 1000.f / Fps)
				continue;
			lastCopy = now;
			flatbuffers::FlatBufferBuilder fbb;
			std::vector<flatbuffers::Offset<app::AppEvent>> Offsets;
			{
				std::unique_lock lock(Mutex);
				if (ShouldStop)
					break;
				Offsets.push_back(CreateAppEventOffset(
						fbb,
						nos::app::CreateScheduleRequest(
							fbb, nos::app::ScheduleRequestKind::NODE, &NodeId, 1)));
				HandleEvent(CreateAppEvent(fbb, app::CreateBatchAppEventDirect(fbb, &Offsets)));
			}
		}
	}

	void GetScheduleInfo(nosScheduleInfo* info) override
	{
		info->Type = NOS_SCHEDULE_TYPE_ON_DEMAND;
		info->DeltaSeconds = {10000u, (uint32_t)std::floor(Fps * 10000)};
		info->Importance = 0;
		for (int i = 0; i < info->PinInfosCount; i++)
			info->PinInfos[i].NeedsRepeat = AcceptRepeat;
	}

};

nosResult RegisterSink(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("Sink"), SinkNode, fn)
	return NOS_RESULT_SUCCESS;
}
}
