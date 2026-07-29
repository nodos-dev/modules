/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include <algorithm>

#include "Ring.h"

namespace nos
{

// Ring that holds N independent channels under a single mutex / CV pair.
// Each channel still owns its own slot pools and Resources, but every push,
// pop, resize and reset goes through the shared synchronization, so an
// N-channel batch push is a single lock acquisition, not N.
//
// Channels and slots are handed out as shared pointers. A node's output pins
// can live on different paths, so a consumer may be popping a channel while
// another thread removes that channel or resizes the ring; sharing ownership
// keeps the channel and its in-flight slot alive until the consumer is done.
// Detaching a channel marks it Removed and wakes its waiters, so nobody blocks
// on a channel that can never be pushed to again.
struct MultiRing
{
	using SlotPtr = rc<ResourceInterface::ResourceBase>;

	struct Channel
	{
		// Set once, before the channel is published. AddChannel replaces the whole
		// channel rather than reassigning this, so a holder of the previous one
		// keeps a consistent object instead of watching it change underneath.
		std::shared_ptr<ResourceInterface> ResInterface;

		// Everything below is guarded by MultiRing::Mutex.
		std::vector<SlotPtr> Resources;
		std::deque<SlotPtr> WritePool;
		std::deque<SlotPtr> ReadPool;
		bool Removed = false;
	};

	using ChannelPtr = std::shared_ptr<Channel>;
	using SlotPair = std::pair<ChannelPtr, SlotPtr>;

	// Snapshot of a channel's pool occupancy, taken under the lock.
	struct Stats
	{
		size_t ReadCount = 0;
		size_t WriteCount = 0;
		size_t TotalFrameCount = 0;
	};

	std::map<char, ChannelPtr> Channels;
	std::mutex Mutex;
	std::condition_variable WriteCV;
	std::condition_variable ReadCV;
	std::atomic_bool Exit = true;
	std::atomic_uint32_t Size = 0;

	~MultiRing() { Stop(); }

	void Stop()
	{
		{
			std::unique_lock lock(Mutex);
			Exit = true;
		}
		WriteCV.notify_all();
		ReadCV.notify_all();
	}

	void Start()
	{
		std::unique_lock lock(Mutex);
		Exit = false;
	}

	void AllocateChannelResourcesUnlocked(Channel& ch)
	{
		ch.WritePool.clear();
		ch.ReadPool.clear();
		ch.Resources.clear();
		for (uint32_t i = 0, size = Size; i < size; ++i)
		{
			auto res = ch.ResInterface->CreateResource();
			if (!res)
			{
				nosEngine.LogE("Failed to create resource for multi ring buffer.");
				ch.Resources.clear();
				ch.WritePool.clear();
				ch.ReadPool.clear();
				Exit = true;
				return;
			}
			ch.Resources.push_back(res);
			ch.WritePool.push_back(std::move(res));
		}
	}

	// A slot taken before a resize/recreation no longer belongs to the channel.
	// Returning it to a pool would grow the pool past the resource count and
	// hand a freed-from-the-ring resource back out, so such slots are dropped.
	static bool OwnsSlotUnlocked(Channel const& ch, SlotPtr const& slot)
	{
		return std::find(ch.Resources.begin(), ch.Resources.end(), slot) != ch.Resources.end();
	}

	ChannelPtr AddChannel(char key, std::shared_ptr<ResourceInterface> resInterface)
	{
		ChannelPtr ch;
		{
			std::unique_lock lock(Mutex);
			auto it = Channels.find(key);
			if (it != Channels.end())
			{
				it->second->Removed = true;
				Channels.erase(it);
			}
			ch = std::make_shared<Channel>();
			ch->ResInterface = std::move(resInterface);
			if (Size == 0)
				Size = 1;
			AllocateChannelResourcesUnlocked(*ch);
			Channels[key] = ch;
		}
		WriteCV.notify_all();
		ReadCV.notify_all();
		return ch;
	}

	void RemoveChannel(char key)
	{
		{
			std::unique_lock lock(Mutex);
			auto it = Channels.find(key);
			if (it == Channels.end())
				return;
			it->second->Removed = true;
			Channels.erase(it);
		}
		// Wake whoever is blocked on the channel we just detached, otherwise they
		// sit until timeout waiting for a push that can never come.
		WriteCV.notify_all();
		ReadCV.notify_all();
	}

	void RecreateChannelResources(Channel& ch)
	{
		{
			std::unique_lock lock(Mutex);
			if (ch.Removed)
				return;
			AllocateChannelResourcesUnlocked(ch);
		}
		// Both CVs: allocation failure sets Exit, which readers wait on too.
		WriteCV.notify_all();
		ReadCV.notify_all();
	}

	void ResizeAll(uint32_t newSize)
	{
		{
			std::unique_lock lock(Mutex);
			Size = newSize;
			for (auto& [_, ch] : Channels)
				AllocateChannelResourcesUnlocked(*ch);
		}
		WriteCV.notify_all();
		ReadCV.notify_all();
	}

	bool HasResources(Channel const& ch)
	{
		std::unique_lock lock(Mutex);
		return !ch.Resources.empty();
	}

	// Owning reference, so the caller survives a concurrent resize.
	SlotPtr FirstResource(Channel const& ch)
	{
		std::unique_lock lock(Mutex);
		if (ch.Resources.empty())
			return nullptr;
		return ch.Resources.front();
	}

	// One acquisition for all three, so the counts agree with each other and
	// TotalFrameCount cannot underflow against a separately-read Size.
	Stats GetStats(Channel const& ch)
	{
		std::unique_lock lock(Mutex);
		size_t size = Size;
		size_t write = ch.WritePool.size();
		return {.ReadCount = ch.ReadPool.size(),
				.WriteCount = write,
				.TotalFrameCount = size > write ? size - write : 0};
	}

	// Move slots between pools for every channel. fill=false: read→write.
	void ResetAll(bool fill)
	{
		{
			std::unique_lock lock(Mutex);
			for (auto& [_, ch] : Channels)
			{
				auto& from = fill ? ch->WritePool : ch->ReadPool;
				auto& to = fill ? ch->ReadPool : ch->WritePool;
				while (!from.empty())
				{
					auto slot = from.front();
					from.pop_front();
					ch->ResInterface->Reset(slot.get());
					to.push_back(std::move(slot));
				}
			}
		}
		WriteCV.notify_all();
		ReadCV.notify_all();
	}

	// If this channel is full and its read pool is non-empty, hand one slot
	// back to the write pool so the producer can start pushing again.
	void MoveOneReadToWriteIfFull(Channel& ch)
	{
		{
			std::unique_lock lock(Mutex);
			if (ch.ReadPool.size() != ch.Resources.size() || ch.ReadPool.empty())
				return;
			auto slot = ch.ReadPool.front();
			ch.ReadPool.pop_front();
			ch.WritePool.push_back(std::move(slot));
		}
		WriteCV.notify_all();
	}

	size_t WritePoolSize(Channel const& ch)
	{
		std::unique_lock lock(Mutex);
		return ch.WritePool.size();
	}

	// Atomically pop one slot from each requested channel's WritePool.
	// Waits until every requested channel has at least one slot, or
	// timeout/exit/removal. The caller-supplied list typically excludes channels
	// that don't have valid input data this frame.
	bool BeginPushSubset(uint64_t timeoutMs,
						 std::vector<ChannelPtr> const& wanted,
						 std::vector<SlotPair>& outSlots)
	{
		std::unique_lock lock(Mutex);
		auto pred = [&] {
			if (Exit)
				return true;
			if (wanted.empty())
				return false;
			for (auto const& ch : wanted)
			{
				if (ch->Removed)
					return true;
				if (ch->WritePool.empty())
					return false;
			}
			return true;
		};
		if (!WriteCV.wait_for(lock, std::chrono::milliseconds(timeoutMs), pred))
			return false;
		// The predicate returns true on removal to stop waiting, which is not the
		// same as success, so re-check what it actually found.
		if (Exit)
			return false;
		for (auto const& ch : wanted)
			if (ch->Removed || ch->WritePool.empty())
				return false;
		outSlots.clear();
		outSlots.reserve(wanted.size());
		for (auto const& ch : wanted)
		{
			auto slot = ch->WritePool.front();
			ch->WritePool.pop_front();
			outSlots.emplace_back(ch, std::move(slot));
		}
		return true;
	}

	void EndPushAll(std::vector<SlotPair> const& slots)
	{
		{
			std::unique_lock lock(Mutex);
			for (auto const& [ch, slot] : slots)
				if (!ch->Removed && OwnsSlotUnlocked(*ch, slot))
					ch->ReadPool.push_back(slot);
		}
		ReadCV.notify_all();
	}

	SlotPtr BeginPop(Channel& ch, uint64_t timeoutMs)
	{
		std::unique_lock lock(Mutex);
		if (!ReadCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
							 [&] { return !ch.ReadPool.empty() || ch.Removed || Exit; }))
			return nullptr;
		if (Exit || ch.Removed || ch.ReadPool.empty())
			return nullptr;
		auto slot = ch.ReadPool.front();
		ch.ReadPool.pop_front();
		return slot;
	}

	void EndPop(Channel& ch, SlotPtr slot)
	{
		{
			std::unique_lock lock(Mutex);
			if (ch.Removed || !OwnsSlotUnlocked(ch, slot))
				return;
			slot->FrameNumber = 0;
			ch.WritePool.push_back(std::move(slot));
		}
		WriteCV.notify_all();
	}
};

} // namespace nos
