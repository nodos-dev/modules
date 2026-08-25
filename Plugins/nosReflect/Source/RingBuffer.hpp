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
		Head = (Head + count) % Capacity;
		CurrentSize += count;
		NOS_SOFT_CHECK(CurrentSize <= Capacity, "Push count cannot exceed ring capacity!");
		if (State != RingState::Filling || CurrentSize == Capacity)
		{
			State = RingState::Serving;
			ReadyForPopCV.notify_all();
		}
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
				if (ExitRequested)
					return true;
				if (State == RingState::Filling)
					return CurrentSize == Capacity;
				return CurrentSize >= count;
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
		Head = 0;
		Tail = 0;
		CurrentSize = 0;
		Buffer.clear();
		if (newCapacity && *newCapacity != Capacity)
			Capacity = *newCapacity;
		Buffer.resize(Capacity);
		if (newMode)
			Mode = *newMode;
		switch (Mode)
		{
		case RingBufferServeMode::ServeImmediately:
			State = RingState::Serving;
			break;
		case RingBufferServeMode::WaitUntilFull:
			State = RingState::Filling;
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
	size_t Capacity;
	std::vector<T> Buffer;
	size_t Head;
	size_t Tail;
	size_t CurrentSize;

	enum class RingState
	{
		Filling,
		Serving
	};

	mutable std::mutex Mutex;
	std::condition_variable ReadyForPopCV;
	std::condition_variable ReadyForPushCV;
	std::atomic_bool ExitRequested;
	RingState State = RingState::Filling;
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
	uint32_t RemainingRepeatCount = 0;

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
		RemainingRepeatCount = Capacity - 1;
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
		if (Ring.GetMode() == RingBufferServeMode::WaitUntilFull)
		{
			if (RemainingRepeatCount > 0)
			{
				--RemainingRepeatCount;
				return NOS_RESULT_SUCCESS;
			}
		}
		std::unique_ptr<SlotType>* srcSlot;
		{
			ScopedProfilerEvent _({ .Name = "Wait For Read" });
			srcSlot = Ring.BeginPop(100);
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