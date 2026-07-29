// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#pragma once

#include <algorithm>
#include <mutex>
#include <set>
#include <shared_mutex>

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
		MultiRing::ChannelPtr RingChannel;
		std::atomic_bool IsOutLive = false;
		bool NeedsRecreation = false;
		// Watch log prefix, rebuilt whenever the pins change. The per-frame
		// stats path must not read the SDK pin map, which another runner
		// thread rewrites on pin updates.
		std::string StatsPrefix;

		Channel(char letter)
			: Letter(letter),
			  InputName((std::string("Input_") + letter).c_str()),
			  OutputName((std::string("Output_") + letter).c_str()),
			  TypeInfo(NSN_Generic)
		{
		}
	};

	using ChannelPtr = std::shared_ptr<Channel>;

	std::map<char, ChannelPtr> Channels;
	std::unordered_map<uuid, char> PinIdToLetter;
	MultiRing Ring;
	// One producer run pushes one slot to each channel it gathered, so we must
	// only schedule again once every one of those has been popped — otherwise
	// schedule requests pile up by a factor of N (channels) per consumer tick.
	//
	// The expectation is what the producer actually pushed, not which output
	// pins are live. Pin liveness means "run CopyFrom even when the pin is
	// clean"; it does not mean a CopyFrom exists. An output that is live but not
	// on a compiled path is never popped, and counting it would hold the round
	// open forever.
	std::set<char> PushedLastRound;
	std::set<char> PoppedSinceLastSchedule;

	void ResetScheduleRoundUnlocked()
	{
		PushedLastRound.clear();
		PoppedSinceLastSchedule.clear();
	}

	void ForgetChannelInRoundUnlocked(char letter)
	{
		PushedLastRound.erase(letter);
		PoppedSinceLastSchedule.erase(letter);
	}

	// True once every channel of the last producer run has been consumed.
	bool IsRoundCompleteUnlocked() const
	{
		return !PushedLastRound.empty() &&
			   std::includes(PoppedSinceLastSchedule.begin(), PoppedSinceLastSchedule.end(),
							 PushedLastRound.begin(), PushedLastRound.end());
	}

	// Guards everything above. The node's pins are spread over several paths —
	// the producer path executes the node while each consumer path calls
	// CopyFrom for its own output — so these callbacks run concurrently on
	// different threads, and the per-frame ones only read.
	//
	// Not recursive, deliberately. SetPinValue on a pin this runner manages
	// dispatches OnPinValueChanged inline on the calling thread rather than
	// enqueueing it, so holding this across such a call would let the callback
	// observe half-updated state. The rule is to not do that — never hold this
	// across an engine call that can re-enter. A deadlock in testing is the
	// wanted failure; a recursive mutex would hide it.
	//
	// Never held across a blocking ring wait or a GPU wait; take it, snapshot
	// or commit, release. Lock order is always StateMutex before Ring's mutex.
	std::shared_mutex StateMutex;

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

	void UpdateStatsPrefixUnlocked(Channel& ch)
	{
		ch.StatsPrefix = NodeName.AsString() + " " + GetChannelDisplayName(ch);
	}

	void SendRingStats(Channel const& ch, MultiRing::Channel& ringChannel, std::string_view state)
	{
		std::string prefix;
		{
			std::shared_lock lock(StateMutex);
			prefix = ch.StatsPrefix;
		}
		auto stats = Ring.GetStats(ringChannel);
		nosEngine.WatchLog((prefix + " Read Size").c_str(), std::to_string(stats.ReadCount).c_str());
		nosEngine.WatchLog((prefix + " Write Size").c_str(), std::to_string(stats.WriteCount).c_str());
		nosEngine.WatchLog((prefix + " Total Frame Count").c_str(), std::to_string(stats.TotalFrameCount).c_str());
		nosEngine.WatchLog((prefix + " State").c_str(), state.data());
	}

	void SetStatus(Status newStatus, std::string message = "")
	{
		std::unique_lock lock(StateMutex);
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
				channel = std::make_shared<Channel>(*letter);

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
		{
			InitChannelUnlocked(*ch);
			UpdateStatsPrefixUnlocked(*ch);
		}

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
			{
				std::unique_lock lock(StateMutex);
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
					ResetScheduleRoundUnlocked();
			}
			if (any)
				Ring.Stop();
		});
	}

	~MultiBoundedQueueNodeContext() override { Ring.Stop(); }

	void InitChannelUnlocked(Channel& ch)
	{
		std::shared_ptr<ResourceInterface> resource;
		if (ch.TypeInfo->TypeName == NOS_NAME(sys::vulkan::Buffer::GetFullyQualifiedName()))
			resource = std::make_shared<GPUBufferResource>();
		else if (ch.TypeInfo->TypeName == NOS_NAME(sys::vulkan::Texture::GetFullyQualifiedName()))
			resource = std::make_shared<GPUTextureResource>();
		else
			resource = std::make_shared<CPUTrivialResource>();

		ch.RingChannel = Ring.AddChannel(ch.Letter, std::move(resource));
	}

	ChannelPtr GetChannelByPinIdUnlocked(uuid const& id)
	{
		auto it = PinIdToLetter.find(id);
		if (it == PinIdToLetter.end())
			return nullptr;
		auto chIt = Channels.find(it->second);
		return chIt != Channels.end() ? chIt->second : nullptr;
	}

	void RequestRingResize(uint32_t size)
	{
		if (size == 0)
		{
			nosEngine.LogW((GetName() + " size cannot be 0").c_str());
			return;
		}
		{
			std::unique_lock lock(StateMutex);
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
			ResetScheduleRoundUnlocked();
			RequestedRingSize = size;
		}
		Ring.Stop();
		SendPathRestart();
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
		{
			std::unique_lock lock(StateMutex);
			auto ch = GetChannelByPinIdUnlocked(pinId);
			if (!ch || !ch->RingChannel)
				return;
			if (!ch->RingChannel->ResInterface->CheckNewResource(NSN_Input, value, std::nullopt))
				return;
			nosEngine.SendPathRestart(ch->InputId);
			ResetScheduleRoundUnlocked();
			ch->NeedsRecreation = true;
		}
		Ring.Stop();
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		auto pinNameStr = nos::Name(params->InstigatorPinName).AsString();
		auto letter = ParseLetter(pinNameStr);
		if (!letter)
			return NOS_RESULT_FAILED;
		std::unique_lock lock(StateMutex);
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
			ResetScheduleRoundUnlocked();
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
		std::unique_lock lock(StateMutex);
		for (auto& [_, ch] : Channels)
		{
			if (!ch->RingChannel)
				InitChannelUnlocked(*ch);
			UpdateStatsPrefixUnlocked(*ch);
		}
	}

	void OnNodeUpdated(nosNodeUpdate const* update) override
	{
		std::unique_lock lock(StateMutex);
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
				// Drop the popped mark with the channel: a stale entry counts
				// towards a round whose live channel count has just shrunk.
				ForgetChannelInRoundUnlocked(letter);
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
				chPtr = std::make_shared<Channel>(*letter);
			if (IsInputPin(sv))
				chPtr->InputId = uuid(*pin->id());
			else
			{
				chPtr->OutputId = uuid(*pin->id());
				chPtr->IsOutLive = pin->live();
			}
			PinIdToLetter[uuid(*pin->id())] = *letter;
			if (!chPtr->RingChannel)
				InitChannelUnlocked(*chPtr);
			UpdateStatsPrefixUnlocked(*chPtr);
		}
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		if (Ring.Exit)
			return NOS_RESULT_FAILED;

		NodeExecuteParams pins(params);
		uint32_t requestedSize = *pins.GetPinData<uint32_t>(NSN_Size);

		struct Gathered
		{
			ChannelPtr NodeCh;
			MultiRing::ChannelPtr RingCh;
			void* Input;
		};
		std::vector<Gathered> gathered;
		std::vector<MultiRing::ChannelPtr> wantedRings;

		uint32_t maxRequired = requestedSize;
		std::string adjustMessage;
		{
			std::shared_lock lock(StateMutex);
			if (Channels.empty())
				return NOS_RESULT_FAILED;
			gathered.reserve(Channels.size());
			wantedRings.reserve(Channels.size());
			for (auto& [_, ch] : Channels)
			{
				if (!ch->RingChannel || !ch->TypeInfo || !Ring.HasResources(*ch->RingChannel))
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
				gathered.push_back({ch, ch->RingChannel, input});
				wantedRings.push_back(ch->RingChannel);
			}
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
			SendRingStats(*g.NodeCh, *g.RingCh, "Pre Push");
		if (!Ring.BeginPushSubset(100, wantedRings, slots))
			return Ring.Exit ? NOS_RESULT_FAILED : NOS_RESULT_PENDING;

		for (size_t i = 0; i < gathered.size(); ++i)
		{
			auto& g = gathered[i];
			auto* slot = slots[i].second.get();
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
			SendRingStats(*g.NodeCh, *g.RingCh, "Post Push");

		{
			// These are the pops the next round waits for.
			std::unique_lock lock(StateMutex);
			PushedLastRound.clear();
			for (auto const& g : gathered)
				PushedLastRound.insert(g.NodeCh->Letter);
		}
		return NOS_RESULT_SUCCESS;
	}

	nosResult CopyFrom(nosCopyInfo* cpy) override
	{
		ChannelPtr ch;
		MultiRing::ChannelPtr ringCh;
		{
			std::shared_lock lock(StateMutex);
			ch = GetChannelByPinIdUnlocked(cpy->ID);
			if (!ch || !ch->RingChannel || Ring.Exit)
				return NOS_RESULT_FAILED;
			if (!ch->IsOutLive)
				return NOS_RESULT_SUCCESS;
			ringCh = ch->RingChannel;
		}

		MultiRing::SlotPtr slot;
		SendRingStats(*ch, *ringCh, "Pre Begin Pop");
		{
			ScopedProfilerEvent _({.Name = "Wait For Filled Slot"});
			slot = Ring.BeginPop(*ringCh, 100);
		}
		if (!slot)
			return Ring.Exit ? NOS_RESULT_FAILED : NOS_RESULT_PENDING;
		SendRingStats(*ch, *ringCh, "Post Begin Pop");

		// Propagate the slot resource's descriptor onto the output pin before
		// Copy reads cpy->PinData as the destination — otherwise the GPU copy
		// targets the stale (default-sized) output descriptor.
		nos::Buffer outPinVal;
		if (ringCh->ResInterface->BeginCopyFrom(slot.get(), *cpy->PinData, outPinVal))
			nosEngine.SetPinValueByName(NodeId, ch->OutputName, outPinVal);

		ringCh->ResInterface->Copy(slot.get(), cpy, NodeId);

		cpy->CopyFromOptions.ShouldSetSourceFrameNumber = true;
		cpy->FrameNumber = slot->FrameNumber;

		Ring.EndPop(*ringCh, std::move(slot));
		SendRingStats(*ch, *ringCh, "End Copy From");

		bool schedule = false;
		{
			std::unique_lock lock(StateMutex);
			PoppedSinceLastSchedule.insert(ch->Letter);
			if (IsRoundCompleteUnlocked())
			{
				ResetScheduleRoundUnlocked();
				schedule = true;
			}
		}
		if (schedule)
			SendScheduleRequest(1);
		return NOS_RESULT_SUCCESS;
	}

	void OnEndFrame(uuid const& pinId, nosEndFrameCause cause) override
	{
		if (cause != NOS_END_FRAME_FAILED)
			return;
		std::unique_lock lock(StateMutex);
		auto ch = GetChannelByPinIdUnlocked(pinId);
		if (!ch)
			return;
		if (pinId == ch->OutputId)
			return;
		if (!ch->IsOutLive)
			return;
		ChangePinLiveness(ch->OutputName, false);
		ch->IsOutLive = false;
		// Drop the popped mark with the liveness, for the same reason: leaving it
		// set while the live count shrinks satisfies the round test early on every
		// later pop.
		ForgetChannelInRoundUnlocked(ch->Letter);
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
		{
			if (command->RingSize == 0)
				return;
			{
				std::unique_lock lock(StateMutex);
				RequestedRingSize = command->RingSize;
			}
			nosEngine.SetPinValue(*GetPinId(NSN_Size), nos::Buffer::From(command->RingSize));
			break;
		}
		default: return;
		}
	}

	void OnPathStop() override
	{
		{
			std::unique_lock lock(StateMutex);
			ResetScheduleRoundUnlocked();
		}
		Ring.Stop();
	}

	void OnPathStart() override
	{
		size_t totalSchedule = 0;
		{
			std::unique_lock lock(StateMutex);
			if (Channels.empty())
				return;

			ResetScheduleRoundUnlocked();

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

			for (auto& [_, ch] : Channels)
			{
				if (!ch->RingChannel)
					continue;
				if (!Ring.HasResources(*ch->RingChannel))
				{
					totalSchedule = std::max<size_t>(totalSchedule, 1);
					continue;
				}
				totalSchedule = std::max(totalSchedule, Ring.WritePoolSize(*ch->RingChannel));
				ch->RingChannel->ResInterface->OnPathStart();
			}
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
		{
			std::shared_lock lock(StateMutex);
			if (Channels.size() <= 1)
				return;
		}
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
			{
				std::shared_lock lock(StateMutex);
				for (char c : CHANNEL_LETTERS)
				{
					if (!Channels.contains(c))
					{
						newLetter = c;
						break;
					}
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
			uuid inputId, outputId;
			{
				std::shared_lock lock(StateMutex);
				auto it = Channels.find(letter);
				if (it == Channels.end())
					return;
				inputId = it->second->InputId;
				outputId = it->second->OutputId;
			}
			nos::TPartialNodeUpdate update;
			update.node_id = NodeId;
			update.pins_to_delete = {inputId, outputId};
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
