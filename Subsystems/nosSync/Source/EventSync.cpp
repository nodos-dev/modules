#include <Nodos/Plugin.hpp>

#include "nosSync/nosSync.h"

#define RANDOMIZE_EVENT_ORDER 0 // For diagnosis, set to 1 to randomize the order of events when waiting for consensus

namespace nos::sync
{

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

struct EventGroup
{
	uint32_t Id;
	double Timeout; // Maximum time to wait for consensus in number of time units (delta-seconds)
	double ConsensusTolerance; // Fraction of the event time to allow for consensus

	std::unordered_map<uint64_t, Event> Events;
	void AddEvent(Event&& event)
	{
		Events.emplace(event.Id, std::move(event));
	}
	void RemoveEvent(uint64_t eventId)
	{
		auto it = Events.find(eventId);
		if (it != Events.end())
			Events.erase(it);
	}
};

std::string GetEventGroupString(Ref<Event> event, Ref<EventGroup> eventGroup, nosVec2u smallestDeltaSecs)
{
	char eventGroupStr[256];
	std::snprintf(eventGroupStr, sizeof(eventGroupStr), "[%llu:%lu:(%lu/%lu)]", event->PathGroupId, eventGroup->Id, smallestDeltaSecs.x, smallestDeltaSecs.y);
	return std::string(eventGroupStr);
}

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

	std::unordered_map<uint64_t, Ref<Event>> GetSyncedEvents(Ref<Event> event, nosVec2u* outSmallestDeltaSecs = nullptr)
	{
		std::unordered_map<uint64_t, Ref<Event>> events;
		nosVec2u smallestDeltaSecs = event->DeltaSeconds;
		auto it = Groups.find(event->EventGroupId);
		if (it == Groups.end())
			return events; // Event group not found, return empty
		auto& group = it->second;
		for (auto& [eid, e] : group.Events)
		{
			if (group.Id == NOS_SYNC_NO_SYNC_EVENT_GROUP_ID)
			{
				if (e.Id != event->Id)
					continue;
				else
					events.emplace(eid, e);
			}
			else
			{
				if (e.PathGroupId == event->PathGroupId
					&& CanTimeStepsAlign(e.DeltaSeconds, event->DeltaSeconds))
				{
					// Only consider events with the same delta-seconds and in same connected component
					events.emplace(eid, e);
					smallestDeltaSecs = GetSmallerDeltaSeconds(smallestDeltaSecs, e.DeltaSeconds);
				}
			}
		}
		if (outSmallestDeltaSecs)
			*outSmallestDeltaSecs = smallestDeltaSecs;
		return events;
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
		return NOS_RESULT_FAILED;
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
		return NOS_RESULT_FAILED; // Failed to get current path group ID
	auto& eventGroup = it->second;
	auto nextId = GEventSync.NextEventId++;

	eventGroup.Events[nextId] = {
		.Id = nextId,
		.PathGroupId = *pathGroupId,
		.EventGroupId = eventGroup.Id,
		.PfnReset = params->ResetFn,
		.PfnWait = params->WaitFn,
		.PfnNotifySyncGroupHealth = params->NotifyHealthFn,
		.UserData = params->UserData,
		.DeltaSeconds = GetReducedDeltaSeconds(params->DeltaSeconds),
		.IsExternallySynchronized = params->IsExternallySynchronized
	};
	*params->OutEventId = nextId;
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL UnregisterEvent(uint64_t eventId)
{
	std::unique_lock lock(GEventSync.Mutex);
	for (auto git = GEventSync.Groups.begin(); git != GEventSync.Groups.end(); ++git)
	{
		auto& [groupId, group] = *git;
		for (auto eit = group.Events.begin(); eit != group.Events.end(); )
		{
			if (eit->first == eventId)
				eit = group.Events.erase(eit);
			else
				++eit;
		}
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

void NotifySyncGroupHealth(std::unordered_map<uint64_t, Ref<Event>> const& syncedEvents, nosConsensusStatus consensusStatus, nosBool areSyncSourcesMixed)
{
	nosSyncGroupHealth healthInfo {
		.AreSyncSourcesMixed = areSyncSourcesMixed,
		.ConsensusStatus = consensusStatus
	};
	for (const auto& [eventId, event] : syncedEvents)
		event->NotifyHealth(&healthInfo);
}

nosResult NOSAPI_CALL WaitForConsensus(uint32_t eventId, uint64_t* outTimestamp, uint64_t* outCount)
{
	std::unique_lock lock(GEventSync.Mutex);
	auto maybeEvent = GEventSync.GetEvent(eventId);
	if (!maybeEvent)
		return NOS_RESULT_NOT_FOUND;
	auto& [event, eventGroup] = *maybeEvent;

	// Gather the events that should be considered for consensus.
	// They should be in the same group and have the same delta-seconds.
	nosVec2u smallestDeltaSecs;
	auto events = GEventSync.GetSyncedEvents(event, &smallestDeltaSecs);

	nosBool syncSourcesAreMixed = NOS_FALSE;
	std::optional<bool> isAllExternallySynced = std::nullopt;
	for (const auto& [eid, ev] : events)
	{
		if (!isAllExternallySynced.has_value())
			isAllExternallySynced = ev->IsExternallySynchronized;
		else if (*isAllExternallySynced != ev->IsExternallySynchronized)
		{
			syncSourcesAreMixed = NOS_TRUE;
			break;
		}
	}

	event->HasRequestedConsensus = true;
	uint32_t rcvd = 0;
	// Check if any event has already requested consensus
	for (const auto& [eid, ev] : events)
	{
		if (ev->HasRequestedConsensus)
			rcvd++;
	}
	auto numWaiting = events.size();
	bool shouldWait = (rcvd == 1);
	bool shouldReset = (rcvd == numWaiting);
	if (shouldReset)
	{
		for (auto& [eid, ev] : events)
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

	auto eventGroupStr = GetEventGroupString(event, eventGroup, smallestDeltaSecs);

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
		nosEngine.LogD("Resetting events of group %s", eventGroupStr.c_str());

		for (const auto& [eventId, event] : events)
		{
			auto res = event->Reset(event->UserData);
			if (res == NOS_RESULT_FAILED)
			{
				// Error when resetting, consensus cannot be achieved
				nosEngine.LogE("Failed to reset event %llu in group %s", eventId, eventGroupStr.c_str());
				NotifySyncGroupHealth(events, NOS_CONSENSUS_ATTEMPT_FAILED, syncSourcesAreMixed);
				return res;
			}
		}
	}

	nosEngine.LogD("Attempting to achieve consensus on event group %s", eventGroupStr.c_str());

	uint64_t lastConsensusTimestamp = 0;
	uint64_t startTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

	if (events.empty())
	{
		nosEngine.LogE("No one is waiting on event group %s", eventGroupStr.c_str());
		NotifySyncGroupHealth(events, NOS_CONSENSUS_ACHIEVED, syncSourcesAreMixed);
		return NOS_RESULT_NOT_FOUND; // No waiters to synchronize
	}

	std::unordered_map<Ref<Event>, uint64_t> eventTimestamps;
	eventTimestamps.reserve(events.size()); // Reserve space for all events
	// Collect initial timestamps for all events
	for (const auto& [eventId, event] : events)
	{
		auto waitRes = event->Wait(event->UserData);
		if (waitRes.Result == NOS_RESULT_FAILED)
		{
			// Error when waiting for an event, consensus cannot be achieved
			nosEngine.LogE("Failed to wait for event %llu in group %s", eventId, eventGroupStr.c_str());
			NotifySyncGroupHealth(events, NOS_CONSENSUS_ATTEMPT_FAILED, syncSourcesAreMixed);
			return waitRes.Result;
		}
		eventTimestamps[event] = waitRes.Timestamp;
	}

	uint64_t minTs = UINT64_MAX, maxTs = 0;
	while (true)
	{
		uint64_t currentTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		auto currDiffFromStartNs = currentTimeNs - startTimeNs;
		auto eventIntervalNs = GetIntervalFromDeltaSecs(smallestDeltaSecs) * 1e9;
		auto frac = currDiffFromStartNs / eventIntervalNs;
		if (frac >= (1.0 + eventGroup->Timeout))
		{
			nosEngine.LogE("Timeout waiting for consensus on event group %s", eventGroupStr.c_str());
			NotifySyncGroupHealth(events, NOS_CONSENSUS_TIMEOUT, syncSourcesAreMixed);
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
			nosEngine.LogD("Consensus achieved on event group %s with timestamp %s with relative time span %.3f", eventGroupStr.c_str(), formatted.c_str(), (diffNs / eventIntervalNs));
			*outTimestamp = event->LastAlignedWaitedTimestamp;
			*outCount = event->OccurenceCountAtSync;
			NotifySyncGroupHealth(events, NOS_CONSENSUS_ACHIEVED, syncSourcesAreMixed);
			return NOS_RESULT_SUCCESS;
		}
		// Consensus is not achieved
		
		// Wait for the non-synchronized events again
		for (const auto& [eventId, event] : events)
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
							   eventGroupStr.c_str(),
							   event->Id,
							   std::format("{:%H:%M:%S}", curTime).c_str(),
							   std::format("{:%H:%M:%S}", maxTime).c_str());
			}

			auto waitRes = event->Wait(event->UserData);
			if (waitRes.Result == NOS_RESULT_FAILED)
			{
				// Error when waiting for an event, consensus cannot be achieved
				NotifySyncGroupHealth(events, NOS_CONSENSUS_ATTEMPT_FAILED, syncSourcesAreMixed);
				return waitRes.Result;
			}
			eventTimestamps[event] = waitRes.Timestamp;
		}
	}
}

}