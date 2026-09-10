#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

#include <nosTransfer/Transfer.hpp>

#include "Names.h"
#include "nosReflect/Reflect_generated.h"

namespace nos::reflect
{
template <typename T>
class RingBuffer
{
public:
	explicit RingBuffer(size_t capacity, RingBufferServeMode mode = RingBufferServeMode::WaitUntilFull)
		: Capacity(capacity),
		Buffer(capacity),
		Garbage(capacity, false),
		Head(0),
		Tail(0),
		CurrentSize(0),
		Mode(mode),
		ExitRequested(false)
	{
		Reset(capacity, mode);
	}

	RingBuffer(const RingBuffer&) = delete;
	RingBuffer& operator=(const RingBuffer&) = delete;

	T* BeginPush(uint32_t timeoutMs)
	{
		auto ret = BeginPush(1, timeoutMs);
		if (!ret)
			return nullptr;
		return (*ret)[0];
	}

	std::optional<std::vector<T*>> BeginPush(size_t count, uint32_t timeoutMs)
	{
		std::unique_lock lock(Mutex);
		DiscardGarbageForPush(count);
		if (!ReadyForPushCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			[this, count]() -> bool {
				return (CurrentSize + count) <= Capacity || ExitRequested.load();
			}))
			return std::nullopt; // timeout

		if (ExitRequested.load())
			return std::nullopt;

		std::vector<T*> result(count);
		for (size_t i = 0; i < count; ++i)
			result[i] = &Buffer[(Head + i) % Capacity];
		return result;
	}

	void EndPush(size_t count = 1)
	{
		std::unique_lock lock(Mutex);
		for (size_t i = 0; i < count; ++i)
			Garbage[(Head + i) % Capacity] = false;
		Head = (Head + count) % Capacity;
		CurrentSize += count;
		NOS_SOFT_CHECK(CurrentSize <= Capacity, "Push count cannot exceed ring capacity!");
		ReadyForPopCV.notify_all();
	}

	T* BeginPop(uint32_t timeoutMs)
	{
		auto ret = BeginPop(1, timeoutMs);
		if (!ret)
			return nullptr;
		return (*ret)[0];
	}

	std::optional<std::vector<T*>> BeginPop(size_t count, uint32_t timeoutMs)
	{
		std::unique_lock lock(Mutex);
		if (!ReadyForPopCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
			[this, count]() -> bool {
				return CurrentSize >= count || ExitRequested.load();
			}))
			return std::nullopt; // timeout

		if (ExitRequested.load())
			return std::nullopt;

		std::vector<T*> result(count);
		for (size_t i = 0; i < count; ++i)
			result[i] = &Buffer[(Tail + i) % Capacity];
		return result;
	}

	void EndPop(size_t count = 1)
	{
		std::unique_lock lock(Mutex);
		Tail = (Tail + count) % Capacity;
		NOS_SOFT_CHECK(CurrentSize >= count, "Pop count cannot be smaller than current ring size!");
		CurrentSize = (CurrentSize >= count) ? (CurrentSize - count) : 0;
		lock.unlock();
		ReadyForPushCV.notify_all();
	}

	void Shutdown()
	{
		ExitRequested.store(true);
		ReadyForPopCV.notify_all();
		ReadyForPushCV.notify_all();
	}

	bool IsShuttingDown() const noexcept
	{
		return ExitRequested.load();
	}

	bool IsEmpty() const
	{
		std::unique_lock lock(Mutex);
		return CurrentSize == 0;
	}

	bool IsFull() const
	{
		std::unique_lock lock(Mutex);
		return CurrentSize == Capacity;
	}

	// Whether the entry a pop returns at this offset is garbage kept from before a reset.
	bool IsGarbage(size_t offset = 0) const
	{
		std::unique_lock lock(Mutex);
		return Garbage[(Tail + offset) % Capacity];
	}

	size_t GetCurrentSize() const
	{
		std::unique_lock lock(Mutex);
		return CurrentSize;
	}

	size_t GetCapacity() const noexcept
	{
		return Capacity;
	}

	void Reset(std::optional<size_t> newCapacity = std::nullopt, std::optional<RingBufferServeMode> newMode = std::nullopt)
	{
		std::unique_lock lock(Mutex);
		const bool capacityChanged = newCapacity && *newCapacity != Capacity;
		if (capacityChanged)
			Capacity = *newCapacity;
		if (newMode)
			Mode = *newMode;
		switch (Mode)
		{
		case RingBufferServeMode::WaitUntilFull:
			// Full from the start. The slots hold the last frames in write order, oldest at
			// Head, and are served as garbage while the producer overwrites them, so the
			// consumer never waits for the fill. A resize has no frames worth keeping and
			// starts from empty garbage entries.
			if (capacityChanged)
			{
				Buffer.clear();
				Buffer.resize(Capacity);
				Head = 0;
			}
			Tail = Head;
			CurrentSize = Capacity;
			Garbage.assign(Capacity, true);
			break;
		case RingBufferServeMode::ServeImmediately:
			Head = 0;
			Tail = 0;
			CurrentSize = 0;
			Buffer.clear();
			Buffer.resize(Capacity);
			Garbage.assign(Capacity, false);
			break;
		}
		lock.unlock();
		ExitRequested.store(false);
		ReadyForPopCV.notify_all();
		ReadyForPushCV.notify_all();
	}

	RingBufferServeMode GetMode() const
	{
		return Mode;
	}

private:
	// Garbage only stands in until real frames arrive. When a real frame needs the room,
	// the oldest garbage goes first rather than making the producer wait. Called under
	// the lock.
	void DiscardGarbageForPush(size_t count)
	{
		while (CurrentSize + count > Capacity && CurrentSize > 0 && Garbage[Tail])
		{
			Tail = (Tail + 1) % Capacity;
			--CurrentSize;
		}
	}

	size_t Capacity;
	std::vector<T> Buffer;
	std::vector<bool> Garbage;
	size_t Head;
	size_t Tail;
	size_t CurrentSize;

	mutable std::mutex Mutex;
	std::condition_variable ReadyForPopCV;
	std::condition_variable ReadyForPushCV;
	std::atomic_bool ExitRequested;
	RingBufferServeMode Mode = RingBufferServeMode::WaitUntilFull;
};

struct CopyingSlot : transfer::Slot
{
	uint64_t FrameNumber = 0;
	CopyingSlot(nosObjectId handle) : transfer::Slot(handle) {}
};

struct ObjectSlot
{
	uint64_t FrameNumber = 0;
	ObjectRef Object;
	ObjectSlot() = default;
	ObjectSlot(ObjectRef obj) : Object(std::move(obj)) {}
	ObjectSlot(const ObjectSlot&) = delete;
	ObjectSlot& operator=(const ObjectSlot&) = delete;
	nosResult CopyFrom(ObjectRef&& obj)
	{
		Object = std::move(obj);
		return NOS_RESULT_SUCCESS;
	}
	bool IsDestinationCompatibleWith(nosObjectId obj) const
	{
		return true;
	}
	nosObjectId GetObject() const
	{
		return Object.GetObjectId();
	}
	
};

template <typename SlotType>
struct RingBufferNodeBase : NodeContext
{
	nosName TypeName = NSN_TypeNameGeneric;

	RingBuffer<std::unique_ptr<SlotType>> Ring;
	uint32_t Capacity = 1;

	bool CapacityUpdatedViaPathCommand = false;

	RingBufferNodeBase(RingBufferServeMode mode) : Ring(1, mode)
	{
		AddPinValueWatcher<uint32_t>(NOS_NAME("Capacity"), [this](const uint32_t* newCapacity, std::optional<const uint32_t*> oldCapacity)
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
			if (!updatedViaPathCommand)
			{
				nosPathCommand ringSizeChange{.Event = NOS_RING_SIZE_CHANGE, .RingSize = Capacity};
				nosEngine.SendPathCommand(*GetPinId(NSN_Input), ringSizeChange);
			}
			SendPathRestart(NSN_Input);
		});
	}

	void SendRingStats(std::string_view state) const
	{
		auto nodeName = NodeName.AsString();
		nosEngine.WatchLog((nodeName + " Size").c_str(), std::to_string(Ring.GetCurrentSize()).c_str());
		nosEngine.WatchLog((nodeName + " Capacity").c_str(), std::to_string(Ring.GetCapacity()).c_str());
		nosEngine.WatchLog((nodeName + " State").c_str(), state.data());
	}

	void OnPathStart() override
	{
		Ring.Reset(Capacity);
		SendScheduleRequest(Capacity);
	}

	void OnPathStop() override
	{
		Ring.Shutdown();
	}

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
		std::unique_ptr<SlotType>* srcSlot;
		{
			ScopedProfilerEvent _({ .Name = "Wait For Read" });
			srcSlot = Ring.BeginPop(100);
		}
		if (srcSlot && Ring.IsGarbage())
		{
			// A frame from before the restart, shown once more. It carries no frame number
			// and needs no replacement: the start already scheduled one producer run per slot.
			if (*srcSlot)
				if (auto object = (*srcSlot)->GetObject())
					SetPinObject(NSN_Output, object);
			Ring.EndPop();
			return NOS_RESULT_SUCCESS;
		}
		if (srcSlot && *srcSlot)
		{
			SendRingStats("Post Begin Pop");
			auto& slot = *srcSlot;
			SetPinObject(NSN_Output, slot->GetObject());
			cpy->ShouldSetSourceFrameNumber = true;
			cpy->FrameNumber = slot->FrameNumber; // TODO: Store frame number in ring buffer
			SendScheduleRequest(1);
			Ring.EndPop();
			return NOS_RESULT_SUCCESS;
		}
		if (Ring.IsShuttingDown())
			return NOS_RESULT_FAILED;
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
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		if (NSN_TypeNameGeneric == TypeName)
			return NOS_RESULT_FAILED;
		auto inputObject = ObjectRef(*params[NSN_Input].Object);
		if (!inputObject)
			return NOS_RESULT_FAILURE;
		auto capacity = *InterpretObject<uint32_t>(*params[NOS_NAME("Capacity")].Object);
		capacity = std::max(1u, capacity);

		SendRingStats("Pre Push");
		std::unique_ptr<SlotType>* dstSlot;
		{
			ScopedProfilerEvent _({ .Name = "Wait For Empty Slot" });
			dstSlot = Ring.BeginPush(100);
		}
		if (dstSlot)
		{
			if (!*dstSlot)
				*dstSlot = std::make_unique<SlotType>(inputObject);
			auto& slot = *dstSlot;
			if (!slot->IsDestinationCompatibleWith(inputObject))
			{
				SendPathRestart(NSN_Input);
				return NOS_RESULT_FAILURE;
			}
			slot->FrameNumber = params.FrameNumber;
			auto res = slot->CopyFrom(std::move(inputObject));
			Ring.EndPush();
			SendRingStats("Post Push");
			if (res != NOS_RESULT_SUCCESS)
				return res;
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
		return NOS_RESULT_SUCCESS;
	}
};

}