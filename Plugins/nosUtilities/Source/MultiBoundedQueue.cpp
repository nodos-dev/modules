// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <set>

#include <Nodos/PluginHelpers.hpp>

// External
#include <glm/glm.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>

#include "MultiRing.h"
#include "Ring.h"
#include "nosUtil/Stopwatch.hpp"

namespace nos::utilities
{

struct MultiBoundedQueueNodeContext : NodeContext
{
	static constexpr std::string_view CHANNEL_LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	enum MenuCommandType : uint8_t
	{
		ADD_CHANNEL = 0,
		REMOVE_CHANNEL = 1,
	};

	struct MenuCommand
	{
		MenuCommandType Type;
		uint8_t Letter;
		MenuCommand(uint32_t cmd)
		{
			Type = static_cast<MenuCommandType>(cmd & 0xFF);
			Letter = static_cast<uint8_t>((cmd >> 8) & 0xFF);
		}
		MenuCommand(MenuCommandType type, uint8_t letter) : Type(type), Letter(letter) {}
		operator uint32_t() const { return (Letter << 8) | Type; }
	};

	struct Channel
	{
		char Letter;
		nos::Name InputName;
		nos::Name OutputName;
		uuid InputId{};
		uuid OutputId{};
		nos::TypeInfo TypeInfo;
		MultiRing::Channel* RingChannel = nullptr;
		std::atomic_bool IsOutLive = false;
		bool NeedsRecreation = false;

		Channel(char letter)
			: Letter(letter),
			  InputName((std::string("Input_") + letter).c_str()),
			  OutputName((std::string("Output_") + letter).c_str()),
			  TypeInfo(NSN_Generic)
		{
		}
	};

	std::map<char, std::unique_ptr<Channel>> Channels;
	std::unordered_map<uuid, char> PinIdToLetter;
	MultiRing Ring;
	// Channels popped since the last SendScheduleRequest. One producer run
	// pushes one slot per live channel, so we must only schedule again once
	// every live channel has been popped — otherwise schedule requests pile
	// up by a factor of N (channels) per consumer tick.
	std::set<char> PoppedSinceLastSchedule;

	std::optional<uint32_t> RequestedRingSize = std::nullopt;

	enum class Status
	{
		OK,
		EFFECTIVE_RING_SIZE_ADJUSTED,
	} CurrentStatus = Status::OK;
	std::string CurrentStatusMessage;

	std::string GetName() const { return "MultiBoundedQueue"; }

	std::string GetChannelDisplayName(Channel const& ch) const
	{
		if (auto* inputPin = GetPin(ch.InputId))
			return inputPin->DisplayName.AsString();
		if (auto* outputPin = GetPin(ch.OutputId))
			return outputPin->DisplayName.AsString();
		return ch.InputName.AsString();
	}

	void SendRingStats(Channel const& ch, std::string_view state)
	{
		auto& ringChannel = *ch.RingChannel;
		auto prefix = NodeName.AsString() + " " + GetChannelDisplayName(ch);
		nosEngine.WatchLog((prefix + " Read Size").c_str(), std::to_string(ringChannel.ReadPool.size()).c_str());
		nosEngine.WatchLog((prefix + " Write Size").c_str(), std::to_string(ringChannel.WritePool.size()).c_str());
		nosEngine.WatchLog((prefix + " Total Frame Count").c_str(),
						   std::to_string(Ring.Size - ringChannel.WritePool.size()).c_str());
		nosEngine.WatchLog((prefix + " State").c_str(), state.data());
	}

	void SetStatus(Status newStatus, std::string message = "")
	{
		if (CurrentStatus == newStatus && CurrentStatusMessage == message)
			return;

		CurrentStatus = newStatus;
		CurrentStatusMessage = std::move(message);
		ClearNodeStatusMessages();
		if (CurrentStatus == Status::EFFECTIVE_RING_SIZE_ADJUSTED)
			SetNodeStatusMessage(CurrentStatusMessage, fb::NodeStatusMessageType::WARNING);
	}

	static std::optional<char> ParseLetter(std::string_view pinName)
	{
		auto pos = pinName.find_last_of('_');
		if (pos == std::string::npos || pos + 2 != pinName.size())
			return std::nullopt;
		char c = pinName[pos + 1];
		if (c < 'A' || c > 'Z')
			return std::nullopt;
		return c;
	}

	static bool IsInputPin(std::string_view pinName) { return pinName.starts_with("Input_"); }
	static bool IsOutputPin(std::string_view pinName) { return pinName.starts_with("Output_"); }

	MultiBoundedQueueNodeContext(nosFbNodePtr node) : NodeContext(node)
	{
		std::vector<uuid> pinsToUnorphan;
		for (auto* pin : *node->pins())
		{
			auto pinNameSv = pin->name()->string_view();
			if (!IsInputPin(pinNameSv) && !IsOutputPin(pinNameSv))
				continue;
			auto letter = ParseLetter(pinNameSv);
			if (!letter)
				continue;

			auto& channel = Channels[*letter];
			if (!channel)
				channel = std::make_unique<Channel>(*letter);

			if (IsInputPin(pinNameSv))
				channel->InputId = uuid(*pin->id());
			else
			{
				channel->OutputId = uuid(*pin->id());
				channel->IsOutLive = pin->live();
			}
			PinIdToLetter[uuid(*pin->id())] = *letter;

			nos::Name typeName(pin->type_name()->c_str());
			if (typeName != NSN_Generic && channel->TypeInfo->TypeName == NSN_Generic)
				channel->TypeInfo = nos::TypeInfo(typeName);

			if (auto orphanState = pin->orphan_state())
				if (orphanState->type() == fb::PinOrphanStateType::ORPHAN)
					pinsToUnorphan.push_back(uuid(*pin->id()));
		}

		for (auto& [_, ch] : Channels)
			InitChannel(*ch);

		for (auto const& pinId : pinsToUnorphan)
			SetPinOrphanState(pinId, fb::PinOrphanStateType::ACTIVE);

		AddPinValueWatcher(NSN_Size, [this](nos::Buffer const& newSize, std::optional<nos::Buffer> oldVal) {
			uint32_t size = *newSize.As<uint32_t>();
			if (oldVal && oldVal == newSize)
				return;
			RequestRingResize(size);
		});
		AddPinValueWatcher(NSN_Alignment, [this](nos::Buffer const& newAlignment, std::optional<nos::Buffer> oldVal) {
			bool any = false;
			for (auto& [_, ch] : Channels)
			{
				if (!ch->RingChannel)
					continue;
				if (ch->RingChannel->ResInterface->CheckNewResource(NSN_Alignment, newAlignment, oldVal))
				{
					nosEngine.SendPathRestart(ch->InputId);
					ch->NeedsRecreation = true;
					any = true;
				}
			}
			if (any)
			{
				Ring.Stop();
				PoppedSinceLastSchedule.clear();
			}
		});
	}

	~MultiBoundedQueueNodeContext() override { Ring.Stop(); }

	void InitChannel(Channel& ch)
	{
		std::shared_ptr<ResourceInterface> resource;
		if (ch.TypeInfo->TypeName == NOS_NAME(sys::vulkan::Buffer::GetFullyQualifiedName()))
			resource = std::make_shared<GPUBufferResource>();
		else if (ch.TypeInfo->TypeName == NOS_NAME(sys::vulkan::Texture::GetFullyQualifiedName()))
			resource = std::make_shared<GPUTextureResource>();
		else
			resource = std::make_shared<CPUTrivialResource>();

		ch.RingChannel = &Ring.AddChannel(ch.Letter, std::move(resource), &ch);
	}

	Channel* GetChannelByPinId(uuid const& id)
	{
		auto it = PinIdToLetter.find(id);
		if (it == PinIdToLetter.end())
			return nullptr;
		auto chIt = Channels.find(it->second);
		return chIt != Channels.end() ? chIt->second.get() : nullptr;
	}

	void RequestRingResize(uint32_t size)
	{
		if (size == 0)
		{
			nosEngine.LogW((GetName() + " size cannot be 0").c_str());
			return;
		}
		// A resize is applied asynchronously on OnPathStart. Do not keep restarting
		// the path while the requested size is already pending.
		if (Ring.Size == size || (RequestedRingSize && *RequestedRingSize == size))
			return;
		for (auto& [_, ch] : Channels)
		{
			if (!ch->RingChannel || !PinIdToLetter.contains(ch->InputId))
				continue;
			nosPathCommand ringSizeChange{.Event = NOS_RING_SIZE_CHANGE, .RingSize = size};
			nosEngine.SendPathCommand(ch->InputId, ringSizeChange);
			break;
		}
		Ring.Stop();
		PoppedSinceLastSchedule.clear();
		SendPathRestart();
		RequestedRingSize = size;
	}

	void SendPathRestart()
	{
		// All channels share this node context and ring. Restart its runner path
		// once instead of enqueueing one overlapping restart per input channel.
		nosEngine.SendPathRestart(NodeId);
	}

	void OnPinValueChanged(nos::Name pinName, uuid const& pinId, nosBuffer value) override
	{
		auto sv = pinName.AsString();
		if (!IsInputPin(sv))
			return;
		auto* ch = GetChannelByPinId(pinId);
		if (!ch || !ch->RingChannel)
			return;
		if (ch->RingChannel->ResInterface->CheckNewResource(NSN_Input, value, std::nullopt))
		{
			nosEngine.SendPathRestart(ch->InputId);
			Ring.Stop();
			PoppedSinceLastSchedule.clear();
			ch->NeedsRecreation = true;
		}
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		auto pinNameStr = nos::Name(params->InstigatorPinName).AsString();
		auto letter = ParseLetter(pinNameStr);
		if (!letter)
			return NOS_RESULT_FAILED;
		auto chIt = Channels.find(*letter);
		if (chIt == Channels.end())
			return NOS_RESULT_FAILED;
		auto& ch = *chIt->second;
		if (ch.TypeInfo->TypeName != NSN_Generic)
			return NOS_RESULT_FAILED;
		ch.TypeInfo = nos::TypeInfo(params->IncomingTypeName);
		if (ch.RingChannel)
		{
			Ring.Stop();
			PoppedSinceLastSchedule.clear();
			Ring.RemoveChannel(*letter);
			ch.RingChannel = nullptr;
		}
		for (size_t i = 0; i < params->PinCount; i++)
		{
			auto& pinInfo = params->Pins[i];
			if (pinInfo.Id == ch.InputId || pinInfo.Id == ch.OutputId)
				pinInfo.OutResolvedTypeName = ch.TypeInfo->TypeName;
		}
		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(const nosPinUpdate*) override
	{
		for (auto& [_, ch] : Channels)
			if (!ch->RingChannel)
				InitChannel(*ch);
	}

	void OnNodeUpdated(nosNodeUpdate const* update) override
	{
		if (update->Type == NOS_NODE_UPDATE_PIN_DELETED)
		{
			auto it = PinIdToLetter.find(update->PinDeleted);
			if (it == PinIdToLetter.end())
				return;
			char letter = it->second;
			PinIdToLetter.erase(it);
			auto chIt = Channels.find(letter);
			if (chIt == Channels.end())
				return;
			auto& ch = *chIt->second;
			bool inputAlive = PinIdToLetter.contains(ch.InputId);
			bool outputAlive = PinIdToLetter.contains(ch.OutputId);
			if (!inputAlive && !outputAlive)
			{
				if (ch.RingChannel)
				{
					Ring.RemoveChannel(letter);
					ch.RingChannel = nullptr;
				}
				Channels.erase(chIt);
			}
		}
		else if (update->Type == NOS_NODE_UPDATE_PIN_CREATED)
		{
			auto* pin = update->PinCreated;
			auto sv = pin->name()->string_view();
			if (!IsInputPin(sv) && !IsOutputPin(sv))
				return;
			auto letter = ParseLetter(sv);
			if (!letter)
				return;
			auto& chPtr = Channels[*letter];
			if (!chPtr)
				chPtr = std::make_unique<Channel>(*letter);
			if (IsInputPin(sv))
				chPtr->InputId = uuid(*pin->id());
			else
			{
				chPtr->OutputId = uuid(*pin->id());
				chPtr->IsOutLive = pin->live();
			}
			PinIdToLetter[uuid(*pin->id())] = *letter;
			if (!chPtr->RingChannel)
				InitChannel(*chPtr);
		}
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		if (Channels.empty() || Ring.Exit)
			return NOS_RESULT_FAILED;

		NodeExecuteParams pins(params);
		uint32_t requestedSize = *pins.GetPinData<uint32_t>(NSN_Size);

		struct Gathered
		{
			Channel* NodeCh;
			MultiRing::Channel* RingCh;
			void* Input;
		};
		std::vector<Gathered> gathered;
		gathered.reserve(Channels.size());
		std::vector<MultiRing::Channel*> wantedRings;
		wantedRings.reserve(Channels.size());

		uint32_t maxRequired = requestedSize;
		std::string adjustMessage;
		for (auto& [_, ch] : Channels)
		{
			if (!ch->RingChannel || ch->RingChannel->Resources.empty() || !ch->TypeInfo)
				continue;
			auto it = pins.find(ch->InputName);
			if (it == pins.end())
				continue;
			void* input = ch->RingChannel->ResInterface->GetPinInfo(it->second, false);
			if (!input)
				continue;
			auto [required, message] = ch->RingChannel->ResInterface->GetRequiredRingSize(input, requestedSize);
			if (required > maxRequired)
			{
				maxRequired = required;
				adjustMessage = message;
			}
			gathered.push_back({ch.get(), ch->RingChannel, input});
			wantedRings.push_back(ch->RingChannel);
		}
		if (gathered.empty())
		{
			SendScheduleRequest(0);
			return NOS_RESULT_FAILED;
		}

		bool effectiveSizeAdjusted = maxRequired != requestedSize;
		if (effectiveSizeAdjusted)
			SetStatus(Status::EFFECTIVE_RING_SIZE_ADJUSTED, adjustMessage);
		else
			SetStatus(Status::OK);

		if (Ring.Size != maxRequired)
		{
			RequestRingResize(maxRequired);
			return NOS_RESULT_FAILED;
		}

		std::vector<MultiRing::SlotPair> slots;
		for (auto const& g : gathered)
			SendRingStats(*g.NodeCh, "Pre Push");
		if (!Ring.BeginPushSubset(100, wantedRings, slots))
			return Ring.Exit ? NOS_RESULT_FAILED : NOS_RESULT_PENDING;

		for (size_t i = 0; i < gathered.size(); ++i)
		{
			auto& g = gathered[i];
			auto* slot = slots[i].second;
			g.RingCh->ResInterface->Push(slot, g.Input, params,
										 NOS_NAME_STATIC("MultiBoundedQueue"), false);
			if (!g.NodeCh->IsOutLive)
			{
				ChangePinLiveness(g.NodeCh->OutputName, true);
				g.NodeCh->IsOutLive = true;
			}
		}

		Ring.EndPushAll(slots);
		for (auto const& g : gathered)
			SendRingStats(*g.NodeCh, "Post Push");
		return NOS_RESULT_SUCCESS;
	}

	nosResult CopyFrom(nosCopyInfo* cpy) override
	{
		auto* ch = GetChannelByPinId(cpy->ID);
		if (!ch || !ch->RingChannel || Ring.Exit)
			return NOS_RESULT_FAILED;
		if (!ch->IsOutLive)
			return NOS_RESULT_SUCCESS;

		ResourceInterface::ResourceBase* slot;
		SendRingStats(*ch, "Pre Begin Pop");
		{
			ScopedProfilerEvent _({.Name = "Wait For Filled Slot"});
			slot = Ring.BeginPop(*ch->RingChannel, 100);
		}
		if (!slot)
			return Ring.Exit ? NOS_RESULT_FAILED : NOS_RESULT_PENDING;
		SendRingStats(*ch, "Post Begin Pop");

		// Propagate the slot resource's descriptor onto the output pin before
		// Copy reads cpy->PinData as the destination — otherwise the GPU copy
		// targets the stale (default-sized) output descriptor.
		nos::Buffer outPinVal;
		if (ch->RingChannel->ResInterface->BeginCopyFrom(slot, *cpy->PinData, outPinVal))
			nosEngine.SetPinValueByName(NodeId, ch->OutputName, outPinVal);

		ch->RingChannel->ResInterface->Copy(slot, cpy, NodeId);

		cpy->CopyFromOptions.ShouldSetSourceFrameNumber = true;
		cpy->FrameNumber = slot->FrameNumber;

		Ring.EndPop(*ch->RingChannel, slot);
		SendRingStats(*ch, "End Copy From");

		PoppedSinceLastSchedule.insert(ch->Letter);
		size_t liveCount = 0;
		for (auto& [_, c] : Channels)
			if (c->IsOutLive)
				++liveCount;
		if (PoppedSinceLastSchedule.size() >= liveCount)
		{
			SendScheduleRequest(1);
			PoppedSinceLastSchedule.clear();
		}
		return NOS_RESULT_SUCCESS;
	}

	void OnEndFrame(uuid const& pinId, nosEndFrameCause cause) override
	{
		if (cause != NOS_END_FRAME_FAILED)
			return;
		auto* ch = GetChannelByPinId(pinId);
		if (!ch)
			return;
		if (pinId == ch->OutputId)
			return;
		if (!ch->IsOutLive)
			return;
		ChangePinLiveness(ch->OutputName, false);
		ch->IsOutLive = false;
	}

	void SendScheduleRequest(uint32_t count, bool reset = false) const
	{
		nosScheduleNodeParams schedule{.NodeId = NodeId, .AddScheduleCount = count, .Reset = reset};
		nosEngine.ScheduleNode(&schedule);
	}

	void OnPathCommand(const nosPathCommand* command) override
	{
		switch (command->Event)
		{
		case NOS_RING_SIZE_CHANGE:
			if (command->RingSize == 0)
				return;
			RequestedRingSize = command->RingSize;
			nosEngine.SetPinValue(*GetPinId(NSN_Size), nos::Buffer::From(command->RingSize));
			break;
		default: return;
		}
	}

	void OnPathStop() override
	{
		Ring.Stop();
		PoppedSinceLastSchedule.clear();
	}

	void OnPathStart() override
	{
		if (Channels.empty())
			return;

		PoppedSinceLastSchedule.clear();

		Ring.ResetAll(false);

		if (RequestedRingSize)
		{
			Ring.ResizeAll(*RequestedRingSize);
			for (auto& [_, ch] : Channels)
				ch->NeedsRecreation = false;
			RequestedRingSize = std::nullopt;
		}
		for (auto& [_, ch] : Channels)
		{
			if (ch->NeedsRecreation && ch->RingChannel)
			{
				Ring.RecreateChannelResources(*ch->RingChannel);
				ch->NeedsRecreation = false;
			}
		}

		size_t totalSchedule = 0;
		for (auto& [_, ch] : Channels)
		{
			if (!ch->RingChannel)
				continue;
			if (ch->RingChannel->Resources.empty())
			{
				totalSchedule = std::max<size_t>(totalSchedule, 1);
				continue;
			}
			auto emptySlotCount = Ring.WritePoolSize(*ch->RingChannel);
			totalSchedule = std::max(totalSchedule, emptySlotCount);
			ch->RingChannel->ResInterface->OnPathStart();
		}
		Ring.Start();
		if (totalSchedule > 0)
		{
			nosScheduleNodeParams schedule{.NodeId = NodeId, .AddScheduleCount = (uint32_t)totalSchedule};
			nosEngine.ScheduleNode(&schedule);
		}
	}

	void OnNodeMenuRequested(nosContextMenuRequestPtr request) override
	{
		flatbuffers::FlatBufferBuilder fbb;
		std::vector items = {
			nos::CreateContextMenuItemDirect(fbb, "Add Channel", MenuCommand(ADD_CHANNEL, 0))};
		HandleEvent(CreateAppEvent(fbb, app::CreateAppContextMenuUpdateDirect(
											fbb, request->item_id(), request->pos(), request->instigator(), &items)));
	}

	void OnPinMenuRequested(nos::Name pinName, nosContextMenuRequestPtr request) override
	{
		auto sv = pinName.AsString();
		if (!IsInputPin(sv) && !IsOutputPin(sv))
			return;
		auto letter = ParseLetter(sv);
		if (!letter)
			return;
		if (Channels.size() <= 1)
			return;
		flatbuffers::FlatBufferBuilder fbb;
		std::vector items = {nos::CreateContextMenuItemDirect(
			fbb, "Remove Channel", MenuCommand(REMOVE_CHANNEL, static_cast<uint8_t>(*letter)))};
		HandleEvent(CreateAppEvent(fbb, app::CreateAppContextMenuUpdateDirect(
											fbb, request->item_id(), request->pos(), request->instigator(), &items)));
	}

	void OnMenuCommand(uuid const& itemID, uint32_t cmd) override
	{
		auto command = MenuCommand(cmd);
		switch (command.Type)
		{
		case ADD_CHANNEL:
		{
			char newLetter = 0;
			for (char c : CHANNEL_LETTERS)
			{
				if (!Channels.contains(c))
				{
					newLetter = c;
					break;
				}
			}
			if (newLetter == 0)
			{
				SetNodeStatusMessage("Maximum number of channels reached", fb::NodeStatusMessageType::WARNING);
				return;
			}

			fb::TPin inPin;
			inPin.id = uuid(nosEngine.GenerateID());
			inPin.name = std::string("Input_") + newLetter;
			inPin.type_name = "nos.Generic";
			inPin.show_as = fb::ShowAs::INPUT_PIN;
			inPin.can_show_as = fb::CanShowAs::INPUT_PIN_ONLY;

			fb::TPin outPin;
			outPin.id = uuid(nosEngine.GenerateID());
			outPin.name = std::string("Output_") + newLetter;
			outPin.type_name = "nos.Generic";
			outPin.show_as = fb::ShowAs::OUTPUT_PIN;
			outPin.can_show_as = fb::CanShowAs::OUTPUT_PIN_ONLY;
			outPin.live = true;

			nos::TPartialNodeUpdate update;
			update.node_id = NodeId;
			update.pins_to_add.emplace_back(std::make_unique<fb::TPin>(std::move(inPin)));
			update.pins_to_add.emplace_back(std::make_unique<fb::TPin>(std::move(outPin)));
			flatbuffers::FlatBufferBuilder fbb;
			HandleEvent(CreateAppEvent(fbb, nos::CreatePartialNodeUpdate(fbb, &update)));
			break;
		}
		case REMOVE_CHANNEL:
		{
			char letter = static_cast<char>(command.Letter);
			auto it = Channels.find(letter);
			if (it == Channels.end())
				return;
			auto& ch = *it->second;
			nos::TPartialNodeUpdate update;
			update.node_id = NodeId;
			update.pins_to_delete = {ch.InputId, ch.OutputId};
			flatbuffers::FlatBufferBuilder fbb;
			HandleEvent(CreateAppEvent(fbb, nos::CreatePartialNodeUpdate(fbb, &update)));
			break;
		}
		}
	}
};

nosResult RegisterMultiBoundedQueue(nosNodeFunctions* functions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("MultiBoundedQueue"), MultiBoundedQueueNodeContext, functions)
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities
