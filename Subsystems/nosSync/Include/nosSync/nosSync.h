/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#ifndef NOS_SYNC_H_INCLUDED
#define NOS_SYNC_H_INCLUDED

#include <Nodos/Types.h>

#include <Nodos/PluginAPI.h>

NOS_BEGIN_C_LINKAGE

#define NOS_SYNC_DEFAULT_EVENT_GROUP_ID 1u
#define NOS_SYNC_NO_SYNC_EVENT_GROUP_ID 0u
	
typedef struct nosWaitResult {
	uint64_t TimeSinceLastEventNs;
	uint64_t EventCount;
} nosWaitResult;

typedef enum nosConsensusStatus
{
	NOS_CONSENSUS_IN_PROGRESS = 0,
	NOS_CONSENSUS_ACHIEVED,
	NOS_CONSENSUS_TIMEOUT,
	NOS_CONSENSUS_ATTEMPT_FAILED,
} nosConsensusStatus;

typedef struct nosSyncGroupHealth
{
	/// Path-group contains events with mixed sources (external and internal).
	nosBool AreSyncSourcesMixed;
	/// Reports the consensus status of the event group. If consensus cannot be achieved, it may indicate that the
	/// events in the group are not properly aligned in time.
	nosConsensusStatus ConsensusStatus;
} nosSyncGroupHealth;

typedef nosResult (*nosResetEventPfn)(void* userData);
typedef nosResult (*nosEventWaitPfn)(void* userData, nosWaitResult* outResult);
typedef void (*nosNotifySyncGroupHealthPfn)(void* userData, const nosSyncGroupHealth* status);

typedef struct nosRegisterEventGroupParams {
	uint32_t Id; /// Unique identifier for the event group.
	double Timeout; /// Number of frames to wait before timing out.
	double ConsensusTolerance; /// Fraction of an event time to allow during consensus.
} nosRegisterEventGroupParams;

typedef struct nosRegisterEventParams {
	uint32_t EventGroupId; /// The event group to register the event in. If 0, it will not be synced with other 0 ID'd events.
	nosVec2u DeltaSeconds; /// Interval between events.
	void* UserData; /// User data to pass to the wait function.
	nosResetEventPfn ResetFn; /// To initialize clocks that will be used in the wait function, and reset event states.
	nosEventWaitPfn WaitFn; /// The wait function to call when waiting for consensus.
	uint64_t* OutEventId; /// Output parameter for the unique event identifier.
	nosNotifySyncGroupHealthPfn NotifyHealthFn; /// Optional callback to subscribe to the health status of sync group (events in this event group that falls into same path group).
	/// Whether the event is synchronized by an external source.
	nosBool IsExternallySynchronized;
} nosRegisterEventParams;

typedef struct nosSyncSubsystem
{
	/// ---------------------
	/// Event Synchronization
	/// ---------------------
	/// 
	nosResult (NOSAPI_CALL* RegisterEventGroup)(const nosRegisterEventGroupParams* params);

	/// Registers an event that can be waited with `waitFn` in an event group specified by `eventGroupId`.
	/// When WaitForConsensus is called, it will poll wait functions until they agree on the same event timestamp.
	nosResult (NOSAPI_CALL* RegisterEvent)(const nosRegisterEventParams* params);

	/// Unregisters a previously registered event using its unique identifier.
	nosResult (NOSAPI_CALL* UnregisterEvent)(uint64_t eventId);

	/// Waits until all registered waiters agree on the same event timestamp,
	/// indicating they're all synchronized on the same event occurrence.
	nosResult (NOSAPI_CALL* WaitForConsensus)(uint32_t eventId, uint64_t* outTimestamp, uint64_t* outCount);

	nosResult (NOSAPI_CALL* UnregisterEventGroup)(uint32_t eventGroupId);

	nosResult (NOSAPI_CALL* CreatePromise)(const char* tag, nosObjectReference* outPromise);
	nosResult (NOSAPI_CALL* WaitPromise)(nosObjectId promise, uint64_t timeoutNs);
	nosResult (NOSAPI_CALL* FulfillPromise)(nosObjectId promise);
	nosResult (NOSAPI_CALL* ResetPromise)(nosObjectId promise);
	/// 
	/// ---------------------
} nosSyncSubsystem;

#pragma region Helper Declarations & Macros

// Make sure these are same with nossys file.
#define NOS_SYNC_NAME "nos.sync"
#define NOS_SYNC_VERSION_MAJOR 12
#define NOS_SYNC_VERSION_MINOR 0

extern struct nosPluginInfo nosSyncPluginInfo;
extern nosSyncSubsystem* nosSync;

#define NOS_SYNC_INIT()              \
	nosPluginInfo nosSyncPluginInfo; \
	nosSyncSubsystem* nosSync = nullptr;

#define NOS_SYNC_IMPORT() NOS_IMPORT_DEP(NOS_SYNC_NAME, nosSyncPluginInfo, nosSync)

#pragma endregion

NOS_END_C_LINKAGE

#endif