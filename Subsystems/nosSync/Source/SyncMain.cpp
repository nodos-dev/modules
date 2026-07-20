// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <nosSync/nosSync.h>

#include <Nodos/SubsystemAPI.h>

#include <shared_mutex>
#include <format>
#include <numeric>
#include <random>

#define RANDOMIZE_EVENT_ORDER 0 // For diagnosis, set to 1 to randomize the order of events when waiting for consensus

NOS_INIT_WITH_MIN_REQUIRED_MINOR(0);

NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

static bool operator==(const nosVec2u& a, const nosVec2u& b) { return a.x == b.x && a.y == b.y; }

template <>
struct std::hash<nosVec2u>
{
	size_t operator()(const nosVec2u& v) const noexcept
	{
		return std::hash<uint64_t>{}(static_cast<uint64_t>(v.x) << 32 | v.y);
	}
};

namespace nos::sync
{
std::unordered_map<uint32_t, nosSyncSubsystem*> GExportedSubsystemVersions;

uint64_t NowNs()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Helper to compare two rational time steps (a/b < c/d) using cross-multiplication
static int CompareDeltaSeconds(nosVec2u d1, nosVec2u d2)
{
	if (d1.y == 0 || d2.y == 0)
		return 0;
	uint64_t left = static_cast<uint64_t>(d1.x) * d2.y;
	uint64_t right = static_cast<uint64_t>(d2.x) * d1.y;
	return (left < right) ? -1 : (left > right ? 1 : 0);
}

static nosVec2u ReduceDeltaSeconds(nosVec2u deltaSecs)
{
	if (deltaSecs.y == 0)
		return deltaSecs;
	auto gcd = std::gcd(deltaSecs.x, deltaSecs.y);
	return { deltaSecs.x / gcd, deltaSecs.y / gcd };
}

// Determines if two time steps can align, meaning one is an integer multiple of the other
bool CanTimeStepsAlign(nosVec2u d1, nosVec2u d2)
{
	if (d1.y == 0 || d2.y == 0 || d1.x == 0 || d2.x == 0)
		return false;

	auto d1Reduced = ReduceDeltaSeconds(d1);
	auto d2Reduced = ReduceDeltaSeconds(d2);

	uint64_t ad = static_cast<uint64_t>(d1Reduced.x) * d2Reduced.y;
	uint64_t bc = static_cast<uint64_t>(d1Reduced.y) * d2Reduced.x;

	return (ad % bc == 0) || (bc % ad == 0);
}

nosVec2u GetSmallerDeltaSeconds(nosVec2u d1, nosVec2u d2) { return (CompareDeltaSeconds(d1, d2) <= 0) ? d1 : d2; }

nosVec2u GetLargerDeltaSeconds(nosVec2u d1, nosVec2u d2) { return (CompareDeltaSeconds(d1, d2) >= 0) ? d1 : d2; }

double GetIntervalFromDeltaSecs(nosVec2u deltaSecs)
{
	if (deltaSecs.y == 0)
		return 0.0;										   // Avoid division by zero
	return static_cast<double>(deltaSecs.x) / deltaSecs.y; // Convert delta-seconds to seconds
}

struct WaitResult
{
	nosResult Result;
	uint64_t Timestamp;
};

struct Event
{
	uint64_t Id;
	uint64_t PathGroupId = 0;
	uint32_t EventGroupId = 0;
	nosResetEventPfn PfnReset = nullptr; // Optional callback for resetting the event
	nosEventWaitPfn PfnWait;
	nosNotifySyncGroupHealthPfn PfnNotifySyncGroupHealth = nullptr; // Optional callback for notifying about sync group health
	void* UserData;
	nosVec2u DeltaSeconds = { 0, 0 }; // Time units in delta-seconds (x, y) for this event
	uint64_t LastAlignedWaitedTimestamp = 0; // Last timestamp when this event was waited on
	uint64_t OccurenceCountAtSync = 0; // Number of times this event was fired when consensus achieved
	bool HasRequestedConsensus = false; // Whether the caller has requested consensus
	bool IsExternallySynchronized; // Whether the event is synchronized by an external source.
	
	void NotifyHealth(const nosSyncGroupHealth* status)
	{
		if (PfnNotifySyncGroupHealth)
			PfnNotifySyncGroupHealth(UserData, status);
	}

	WaitResult Wait(void* userData)
	{
		WaitResult res{};
		if (PfnWait)
		{
			nosWaitResult outRes{};
			res.Result = PfnWait(userData, &outRes);
			auto nowNs = NowNs();
			if (res.Result == NOS_RESULT_SUCCESS)
			{
				auto ts = nowNs - outRes.TimeSinceLastEventNs; // Timestamp when the event was last waited on
				LastAlignedWaitedTimestamp = ts;
				OccurenceCountAtSync = outRes.EventCount;
				res.Timestamp = ts;
			}
		}
		return res;
	}

	nosResult Reset(void* userData)
	{
		if (PfnReset)
			return PfnReset(userData);
		return NOS_RESULT_SUCCESS;
	}
};


/// Group of events that are in same path group and are synchronized together. Their delta-seconds are aligned and they
/// share a common consensus timestamp.
struct SyncGroup
{
	uint32_t EventGroupId;
	uint64_t PathGroupId;
	nosVec2u SmallestDeltaSeconds = {0, 0}; // The smallest delta-seconds among all events in this sync group

	std::unordered_map<uint64_t, Ref<Event>> Events; // Event ID x Events of this sync group

	std::optional<uint64_t> ConsensusTimestampNs;
	nosConsensusStatus ConsensusStatus = NOS_CONSENSUS_IN_PROGRESS;

	std::string LogString() const
	{
		char syncGroupStr[256];
		std::snprintf(syncGroupStr,
					  sizeof(syncGroupStr),
					  "[%llu:%lu:(%lu/%lu)]",
					  PathGroupId,
					  EventGroupId,
					  SmallestDeltaSeconds.x,
					  SmallestDeltaSeconds.y);
		return std::string(syncGroupStr);
	}

	nosBool AreSyncSourcesMixed() const
	{
		for (const auto& [eventId, event] : Events)
			if (event->IsExternallySynchronized != Events.begin()->second->IsExternallySynchronized)
				return NOS_TRUE;
		return NOS_FALSE;
	}

	void UpdateState(nosConsensusStatus consensusStatus)
	{
		ConsensusStatus = consensusStatus;
		nosSyncGroupHealth healthInfo{
			.AreSyncSourcesMixed = AreSyncSourcesMixed(),
			.ConsensusStatus = consensusStatus
		};
		for (const auto& [eventId, event] : Events)
			event->NotifyHealth(&healthInfo);
	}
};

struct EventGroup
{
	uint32_t Id;
	double Timeout; // Maximum time to wait for consensus in number of time units (delta-seconds)
	double ConsensusTolerance; // Fraction of the event time to allow for consensus
	std::unordered_map<uint64_t, Event> Events;

	std::unordered_map<uint64_t, std::unordered_map<nosVec2u, SyncGroup>> SyncGroups; // PathGroupId x SmallestDeltaSeconds x SyncGroup of events that are synchronized together

	void RebuildSyncGroups()
	{
		SyncGroups.clear();
		if (Id == NOS_SYNC_NO_SYNC_EVENT_GROUP_ID)
			return; // Events in this group are not synchronized with each other
		for (auto& [eventId, event] : Events)
		{
			auto& pathGroupSyncGroups = SyncGroups[event.PathGroupId];
			// Alignability is not transitive: this event may bridge sync groups that could not align with
			// each other before, so collect every group it aligns with into one.
			SyncGroup merged{.EventGroupId = Id, .PathGroupId = event.PathGroupId, .SmallestDeltaSeconds = event.DeltaSeconds};
			merged.Events.emplace(eventId, event);
			for (auto it = pathGroupSyncGroups.begin(); it != pathGroupSyncGroups.end();)
			{
				auto& syncGroup = it->second;
				bool aligns = false;
				for (auto& [otherId, other] : syncGroup.Events)
					if ((aligns = CanTimeStepsAlign(other->DeltaSeconds, event.DeltaSeconds)))
						break;
				if (aligns)
				{
					merged.Events.merge(syncGroup.Events);
					merged.SmallestDeltaSeconds = GetSmallerDeltaSeconds(merged.SmallestDeltaSeconds, syncGroup.SmallestDeltaSeconds);
					it = pathGroupSyncGroups.erase(it);
				}
				else
					++it;
			}
			auto existing = pathGroupSyncGroups.find(merged.SmallestDeltaSeconds);
			if (existing != pathGroupSyncGroups.end())
				existing->second.Events.merge(merged.Events); // Zero delta-seconds never align; group them by exact rate
			else
				pathGroupSyncGroups.emplace(merged.SmallestDeltaSeconds, std::move(merged));
		}
	}

	SyncGroup* FindSyncGroupOf(uint64_t eventId, uint64_t pathGroupId)
	{
		auto it = SyncGroups.find(pathGroupId);
		if (it == SyncGroups.end())
			return nullptr;
		for (auto& [deltaSecs, syncGroup] : it->second)
			if (syncGroup.Events.contains(eventId))
				return &syncGroup;
		return nullptr;
	}
};

struct EventSync
{
	std::shared_mutex Mutex;
	uint64_t NextEventId = 1;
	std::unordered_map<uint32_t, EventGroup> Groups;

	std::optional<std::pair<Ref<Event>, Ref<EventGroup>>> GetEvent(uint64_t eventId)
	{
		Event* event = nullptr;
		EventGroup* eventGroup = nullptr;
		for (auto git = Groups.begin(); git != Groups.end(); ++git)
		{
			auto& [groupId, group] = *git;
			auto it = group.Events.find(eventId);
			if (it != group.Events.end())
			{
				event = &it->second;
				eventGroup = &group;
				break;
			}
		}
		if (!event || !eventGroup)
			return std::nullopt;
		return std::make_pair(Ref(*event), Ref(*eventGroup));
	}
} GEventSync = {};

nosVec2u GetReducedDeltaSeconds(nosVec2u deltaSecs)
{
	if (deltaSecs.y == 0)
		return deltaSecs;
	auto gcd = std::gcd(deltaSecs.x, deltaSecs.y);
	return { deltaSecs.x / gcd, deltaSecs.y / gcd };
}

nosResult NOSAPI_CALL RegisterEventGroup(const nosRegisterEventGroupParams* params)
{
	if (!params)
		return NOS_RESULT_INVALID_ARGUMENT; // Invalid parameters
	auto it = GEventSync.Groups.find(params->Id);
	if (it != GEventSync.Groups.end())
	{
		nosEngine.LogE("Trying to register an event group with an ID that already exists: %u", params->Id);
		return NOS_RESULT_FAILED;
	}
	GEventSync.Groups[params->Id] = {
		.Id = params->Id,
		.Timeout = params->Timeout,
		.ConsensusTolerance = params->ConsensusTolerance
	};
	return NOS_RESULT_SUCCESS;
}

std::optional<uint64_t> GetCurrentPathGroupId()
{
	uint64_t pathGroupId = 0;
	auto res = nosEngine.GetCurrentPathGroupId(&pathGroupId);
	if (res != NOS_RESULT_SUCCESS || pathGroupId == 0)
	{
		nosEngine.LogE("Failed to get current path group ID: Error code %d", res);
		return std::nullopt;
	}
	return pathGroupId;
}

static_assert(NOS_SYNC_VERSION_MAJOR == 3, "Remove the template parameter");
template<bool HasHealthNotificationSupport, bool HasExternallySynchronizedParam>
nosResult NOSAPI_CALL RegisterEvent(const nosRegisterEventParams* params)
{
	if (!params->WaitFn || !params->OutEventId)
		return NOS_RESULT_INVALID_ARGUMENT; // Invalid parameters
	std::unique_lock lock(GEventSync.Mutex);
	auto it = GEventSync.Groups.find(params->EventGroupId);
	if (it == GEventSync.Groups.end())
		return NOS_RESULT_NOT_FOUND; // Event group not found
	auto pathGroupId = GetCurrentPathGroupId();
	if (!pathGroupId.has_value())
	{
		nosEngine.LogE("Failed to get current path group ID when registering an event");
		return NOS_RESULT_FAILED;
	}
	auto& eventGroup = it->second;
	auto nextId = GEventSync.NextEventId++;
	
	nosNotifySyncGroupHealthPfn notifyHealthFn{};
	if constexpr (HasHealthNotificationSupport)
	{
		notifyHealthFn = params->NotifyHealthFn;
	}
	bool isExternallySynchronized = false;
	if constexpr (HasExternallySynchronizedParam)
	{
		isExternallySynchronized = params->IsExternallySynchronized;
	}

	eventGroup.Events[nextId] = {
		.Id = nextId,
		.PathGroupId = *pathGroupId,
		.EventGroupId = eventGroup.Id,
		.PfnReset = params->ResetFn,
		.PfnWait = params->WaitFn,
		.PfnNotifySyncGroupHealth = notifyHealthFn,
		.UserData = params->UserData,
		.DeltaSeconds = GetReducedDeltaSeconds(params->DeltaSeconds),
		.IsExternallySynchronized = isExternallySynchronized
	};
	eventGroup.RebuildSyncGroups();

	*params->OutEventId = nextId;
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL UnregisterEvent(uint64_t eventId)
{
	std::unique_lock lock(GEventSync.Mutex);
	for (auto& [groupId, group] : GEventSync.Groups)
	{
		if (group.Events.erase(eventId))
			group.RebuildSyncGroups();
	}
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL UnregisterEventGroup(uint32_t eventGroupId)
{
	std::unique_lock lock(GEventSync.Mutex);
	auto it = GEventSync.Groups.find(eventGroupId);
	if (it == GEventSync.Groups.end())
		return NOS_RESULT_NOT_FOUND; // Event group not found
	GEventSync.Groups.erase(it);
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL WaitForConsensus(uint32_t eventId, uint64_t* outTimestamp, uint64_t* outCount)
{
	std::unique_lock lock(GEventSync.Mutex);
	auto maybeEvent = GEventSync.GetEvent(eventId);
	if (!maybeEvent)
		return NOS_RESULT_NOT_FOUND;
	auto& [event, eventGroup] = *maybeEvent;

	// The events to consider for consensus: this event's sync group. In the no-sync group,
	// each event synchronizes alone, so a temporary group holding only this event is used.
	SyncGroup noSyncGroup{.EventGroupId = eventGroup->Id, .PathGroupId = event->PathGroupId, .SmallestDeltaSeconds = event->DeltaSeconds};
	SyncGroup* syncGroup = &noSyncGroup;
	if (eventGroup->Id == NOS_SYNC_NO_SYNC_EVENT_GROUP_ID)
		noSyncGroup.Events.emplace(event->Id, event);
	else
	{
		syncGroup = eventGroup->FindSyncGroupOf(event->Id, event->PathGroupId);
		if (!syncGroup)
		{
			nosEngine.LogE("No sync group found for event %llu in event group %u", event->Id, eventGroup->Id);
			return NOS_RESULT_FAILED;
		}
	}
	auto& syncedEvents = syncGroup->Events;



	event->HasRequestedConsensus = true;
	uint32_t rcvd = 0;
	// Check if any event has already requested consensus
	for (const auto& [eid, ev] : syncedEvents)
	{
		if (ev->HasRequestedConsensus)
			rcvd++;
	}
	auto numWaiting = syncedEvents.size();
	bool shouldWait = (rcvd == 1);
	bool shouldReset = (rcvd == numWaiting);
	if (shouldReset)
	{
		for (auto& [eid, ev] : syncedEvents)
			ev->HasRequestedConsensus = false; // Reset the consensus request flag for all events
	}

	if (!shouldWait)
	{
		if (outTimestamp)
			*outTimestamp = event->LastAlignedWaitedTimestamp;
		if (outCount)
			*outCount = event->OccurenceCountAtSync;
		return NOS_RESULT_SUCCESS; // Already waited for consensus
	}

	auto syncGroupStr = syncGroup->LogString();

	// Diagnosis code, randomize the order of events to treat everyone equally
#if RANDOMIZE_EVENT_ORDER
	{
		std::unordered_set<size_t> usedIndices;
		std::vector<uint64_t> eventIds;
		for (const auto& [eid, ev] : events)
		{
			eventIds.push_back(eid);
		}
		
		auto copy = std::move(events);
		events = {};

		// Randomly select from eventIds, avoiding duplicates
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<size_t> dis(0, eventIds.size() - 1);
		while (usedIndices.size() < copy.size())
		{
			size_t idx = dis(gen);
			if (usedIndices.find(idx) == usedIndices.end())
			{
				usedIndices.insert(idx);
				events.emplace(eventIds[idx], copy.at(eventIds[idx]));
			}
		}
	}
	std::string idOrder = "[";
	for (auto& [id, _] : events)
	{
		idOrder += std::to_string(id) + ", ";
	}
	nosEngine.LogI("Waiting for consensus on event group %u with events: %s", eventGroup->Id, (idOrder + "]").c_str());
#endif


	if (NOS_SYNC_NO_SYNC_EVENT_GROUP_ID != eventGroup->Id)
	{
		nosEngine.LogD("Resetting events of group %s", syncGroupStr.c_str());

		for (const auto& [eventId, event] : syncedEvents)
		{
			auto res = event->Reset(event->UserData);
			if (res == NOS_RESULT_FAILED)
			{
				// Error when resetting, consensus cannot be achieved
				nosEngine.LogE("Failed to reset event %llu in group %s", eventId, syncGroupStr.c_str());
				syncGroup->UpdateState(NOS_CONSENSUS_ATTEMPT_FAILED);
				return res;
			}
		}
	}

	nosEngine.LogD("Attempting to achieve consensus on event group %s", syncGroupStr.c_str());
	
	uint64_t lastConsensusTimestamp = 0;
	uint64_t startTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

	if (syncedEvents.empty())
	{
		nosEngine.LogE("No one is waiting on event group %s", syncGroupStr.c_str());
		syncGroup->UpdateState(NOS_CONSENSUS_ACHIEVED);
		return NOS_RESULT_NOT_FOUND; // No waiters to synchronize
	}

	std::unordered_map<Ref<Event>, uint64_t> eventTimestamps;
	eventTimestamps.reserve(syncedEvents.size()); // Reserve space for all events
	// Collect initial timestamps for all events
	for (const auto& [eventId, event] : syncedEvents)
	{
		auto waitRes = event->Wait(event->UserData);
		if (waitRes.Result == NOS_RESULT_FAILED)
		{
			// Error when waiting for an event, consensus cannot be achieved
			nosEngine.LogE("Failed to wait for event %llu in group %s", eventId, syncGroupStr.c_str());
			syncGroup->UpdateState(NOS_CONSENSUS_ATTEMPT_FAILED);
			return waitRes.Result;
		}
		eventTimestamps[event] = waitRes.Timestamp;
	}

	uint64_t minTs = UINT64_MAX, maxTs = 0;
	while (true)
	{
		uint64_t currentTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		auto currDiffFromStartNs = currentTimeNs - startTimeNs;
		auto eventIntervalNs = GetIntervalFromDeltaSecs(syncGroup->SmallestDeltaSeconds) * 1e9;
		auto frac = currDiffFromStartNs / eventIntervalNs;
		if (frac >= (1.0 + eventGroup->Timeout))
		{
			nosEngine.LogE("Timeout waiting for consensus on event group %s", syncGroupStr.c_str());
			syncGroup->UpdateState(NOS_CONSENSUS_TIMEOUT);
			return NOS_RESULT_TIMEOUT; // Timeout reached
		}
		
		// Find min and max timestamps
		minTs = UINT64_MAX;
		maxTs = 0;
		for (const auto& [event, ts] : eventTimestamps)
		{
			minTs = std::min(minTs, ts);
			maxTs = std::max(maxTs, ts);
		}

		// Check if consensus is achieved
		auto diffNs = (maxTs - minTs);
		if ((diffNs / eventIntervalNs <= eventGroup->ConsensusTolerance))
		{
			lastConsensusTimestamp = maxTs;
			auto time = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::nanoseconds(lastConsensusTimestamp));
			std::string formatted = std::format("{:%H:%M:%S}", time);
			nosEngine.LogD("Consensus achieved on event group %s with timestamp %s with relative time span %.3f", syncGroupStr.c_str(), formatted.c_str(), (diffNs / eventIntervalNs));
			syncGroup->ConsensusTimestampNs = lastConsensusTimestamp;
			*outTimestamp = event->LastAlignedWaitedTimestamp;
			*outCount = event->OccurenceCountAtSync;
			syncGroup->UpdateState(NOS_CONSENSUS_ACHIEVED);
			return NOS_RESULT_SUCCESS;
		}
		// Consensus is not achieved
		
		// Wait for the non-synchronized events again
		for (const auto& [eventId, event] : syncedEvents)
		{
			// If the the event's timestamp is already within the tolarence compared to the maximum timestamp, skip it
			auto ts = eventTimestamps[event];
			auto difFrac = (maxTs - ts) / eventIntervalNs;
			if (difFrac <= eventGroup->ConsensusTolerance)
				continue;

			// Log the event that is behind
			{
				auto curTime =
					std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(ts));
				auto maxTime =
					std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(maxTs));
				nosEngine.LogD("Event group %s entry %llu is behind: Current %s, waiting for %s",
							   syncGroupStr.c_str(),
							   event->Id,
							   std::format("{:%H:%M:%S}", curTime).c_str(),
							   std::format("{:%H:%M:%S}", maxTime).c_str());
			}

			auto waitRes = event->Wait(event->UserData);
			if (waitRes.Result == NOS_RESULT_FAILED)
			{
				// Error when waiting for an event, consensus cannot be achieved
				syncGroup->UpdateState(NOS_CONSENSUS_ATTEMPT_FAILED);
				return waitRes.Result;
			}
			eventTimestamps[event] = waitRes.Timestamp;
		}
	}
}

nosResult NOSAPI_CALL GetCurrentSyncGroupTimeline(uint32_t eventGroupId,
	nosVec2u deltaSeconds,
	nosSyncGroupTimeline* outTimeline)
{
	if (!outTimeline)
		return NOS_RESULT_INVALID_ARGUMENT;
	auto pathGroupId = GetCurrentPathGroupId();
	if (!pathGroupId.has_value())
		return NOS_RESULT_FAILED;
	std::shared_lock lock(GEventSync.Mutex);
	auto git = GEventSync.Groups.find(eventGroupId);
	if (git == GEventSync.Groups.end())
		return NOS_RESULT_NOT_FOUND;
	auto& eventGroup = git->second;
	auto pgIt = eventGroup.SyncGroups.find(*pathGroupId);
	if (pgIt == eventGroup.SyncGroups.end())
		return NOS_RESULT_NOT_FOUND;

	bool rateRequested = deltaSeconds.x != 0 && deltaSeconds.y != 0;
	auto requestedRate = rateRequested ? GetReducedDeltaSeconds(deltaSeconds) : nosVec2u{0, 0};

	SyncGroup* syncGroup = nullptr;
	for (auto& [groupRate, group] : pgIt->second)
	{
		if (rateRequested && !CanTimeStepsAlign(groupRate, requestedRate))
			continue;
		if (syncGroup)
		{
			nosEngine.LogE("Event group %u forms multiple sync groups with path group %llu; requested rate %u/%u is ambiguous",
						   eventGroupId, *pathGroupId, deltaSeconds.x, deltaSeconds.y);
			return NOS_RESULT_FAILED;
		}
		syncGroup = &group;
	}
	if (!syncGroup)
		return NOS_RESULT_NOT_FOUND;
	if (!syncGroup->ConsensusTimestampNs.has_value())
		return NOS_RESULT_PENDING; // No consensus achieved yet on this sync group

	auto rate = rateRequested ? requestedRate : syncGroup->SmallestDeltaSeconds;
	auto intervalNs = GetIntervalFromDeltaSecs(rate) * 1e9;
	if (intervalNs <= 0)
		return NOS_RESULT_INVALID_ARGUMENT;
	uint64_t consensusTs = *syncGroup->ConsensusTimestampNs;
	auto nowNs = NowNs();
	uint64_t ticksSinceConsensus = nowNs > consensusTs ? static_cast<uint64_t>((nowNs - consensusTs) / intervalNs) : 0;
	outTimeline->ConsensusTimestampNs = consensusTs;
	outTimeline->ProjectedEventTimestampNs = consensusTs + static_cast<uint64_t>(ticksSinceConsensus * intervalNs);
	outTimeline->DeltaSeconds = rate;
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL Export(uint32_t minorVersion, void** outSubsystemContext)
{
	auto it = GExportedSubsystemVersions.find(minorVersion);
	if (it != GExportedSubsystemVersions.end())
	{
		*outSubsystemContext = it->second;
		return NOS_RESULT_SUCCESS;
	}
	auto* subsystem = new nosSyncSubsystem();
	subsystem->RegisterEventGroup = RegisterEventGroup;
	static_assert(NOS_SYNC_VERSION_MAJOR == 3, "Update the exported subsystem versions if the major version changes");
	if (minorVersion >= 2)
		subsystem->RegisterEvent = RegisterEvent<true, true>;
	else
		subsystem->RegisterEvent = RegisterEvent<false, false>;
	subsystem->UnregisterEvent = UnregisterEvent;
	subsystem->WaitForConsensus = WaitForConsensus;
	subsystem->UnregisterEventGroup = UnregisterEventGroup;
	subsystem->GetCurrentSyncGroupTimeline = GetCurrentSyncGroupTimeline;
	*outSubsystemContext = subsystem;
	GExportedSubsystemVersions[minorVersion] = subsystem;
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL Initialize()
{
	nosRegisterEventGroupParams defaultGroup{
		.Id = NOS_SYNC_DEFAULT_EVENT_GROUP_ID,
		.Timeout = 10.0,	// Allow 10 frames for sync
		.ConsensusTolerance = 0.49f, // Allow a fraction frame time for tolerance
	};
	RegisterEventGroup(&defaultGroup);
	nosRegisterEventGroupParams noSyncGroup{
		.Id = NOS_SYNC_NO_SYNC_EVENT_GROUP_ID,
		.Timeout = 0.0,	// Don't care
		.ConsensusTolerance = 0.0, // Don't care
	};
	RegisterEventGroup(&noSyncGroup);
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL UnloadSubsystem()
{
	for (auto& [minorVersion, subsystem] : GExportedSubsystemVersions)
		delete subsystem;
	UnregisterEventGroup(NOS_SYNC_DEFAULT_EVENT_GROUP_ID);
	UnregisterEventGroup(NOS_SYNC_NO_SYNC_EVENT_GROUP_ID);
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportSubsystem(nosSubsystemFunctions* funcs)
{
	funcs->OnRequest = Export;
	funcs->Initialize = Initialize;
	funcs->OnPreUnloadSubsystem = UnloadSubsystem;
	return NOS_RESULT_SUCCESS;
}
}
}
