#pragma once

#include "AJAClient.h"
#include "glm/glm.hpp"
#include "Ring.h"
#include <mzUtil/Thread.h>

namespace mz
{

using Clock = std::chrono::high_resolution_clock;
using Milli = std::chrono::duration<double, std::milli>;
using Micro = std::chrono::duration<double, std::micro>;

// almost all scheduler thread
struct TrackSync 
{
    // worker thread access
	std::mutex DroppedFramesMutex;
	std::vector<u32> DroppedFrames;
    std::atomic<u32> FrameIDCounter = 0; 
    // worker thread access

	u32 LastFrameID = 0;
	u32 FramesSinceLastDrop = 0;
	
    bool ResetTrackNow = false;
	bool ResetTrackWhenStable = false;


    virtual u32 GetRingSize() = 0;
    virtual std::string Name() const = 0;

	void UpdateLastFrameID() // called from scheduler thread
	{
		ResetTrackNow = true;
		ResetTrackWhenStable = false;
		
        FramesSinceLastDrop = 0;
		LastFrameID = FrameIDCounter.load() - GetRingSize();

		//if (true)
		{
			mzEngine.Log(("Resetting aja " + Name()).c_str(), "");
		}
	}
	void UpdateDropCount() // called from scheduler thread
	{
		std::vector<u32> dropCounts;
		DequeueDroppedFrames(dropCounts);
		ResetTrackNow = false;
		if (!dropCounts.empty())
		{
			ResetTrackWhenStable = true;
			u32 totalDrops = 0;
			for (const auto& dropCount : dropCounts)
			{
				totalDrops += dropCount;
			}

			mzEngine.Log(("Drop on " + Name() + " : " + std::to_string(totalDrops)).c_str(), "");
		}
		else
		{
			FramesSinceLastDrop++;
			if (ResetTrackWhenStable && FramesSinceLastDrop > 50)
			{
				UpdateLastFrameID();
			}
		}
	}

	void DequeueDroppedFrames(std::vector<u32>& dropCounts) // called from scheduler thread
	{
		std::lock_guard<std::mutex> ScopedLock(DroppedFramesMutex);
		dropCounts = std::move(DroppedFrames);
	}

	void EnqueueDroppedFrames(u32 dropCount) // called from worker thread
	{
		std::lock_guard<std::mutex> ScopedLock(DroppedFramesMutex);
        DroppedFrames.push_back(dropCount);
	}

};


struct CopyThread : TrackSync
{
    UUID id;
    std::atomic_bool run = true;
    mz::fb::ShowAs kind;
    rc<GPURing> gpuRing;
	MzResourceShareInfo CompressedTex = {};
    rc<CPURing> cpuRing;
	// The ring objects above are overwritten on path restart.
	// TODO: Find out other synchronization issues and fix them all
    std::atomic_uint32_t SpareCount = 0;
    std::thread th;
    NTV2Channel Channel;
    u32 DropCount = 0;
    struct AJAClient *client = 0;
    NTV2VideoFormat Format = NTV2_FORMAT_UNKNOWN;
    AJADevice::Mode mode = AJADevice::SL;
    std::atomic<Colorspace> Colorspace = Colorspace::REC709;
    std::atomic<GammaCurve> GammaCurve = GammaCurve::REC709;
    std::atomic_bool NarrowRange = true;
    MzResourceShareInfo SSBO;

	struct
	{
		std::chrono::nanoseconds Time;
		u32 Counter;
	} DebugInfo;

	struct Parameters
	{
		u32 FieldIdx = 0;
		u32 FrameNumber;
		Clock::time_point T0;
		Clock::time_point T1;
	};

	struct ConversionThread : ConsumerThread<Parameters>
	{
		virtual ~ConversionThread();
		std::thread Handle;
		CopyThread* Cpy;
	};
	struct InputConversionThread : ConversionThread
	{
		void Consume(const Parameters& item) override;
	};
	struct OutputConversionThread : ConversionThread
	{
		void Consume(const Parameters& item) override;
	};

	ru<ConversionThread> Worker;

    CopyThread(mz::fb::UUID id, struct AJAClient *client, u32 ringSize, u32 spareCount, mz::fb::ShowAs kind, NTV2Channel channel, 
                NTV2VideoFormat initalFmt, 
                AJADevice::Mode mode,
                enum class Colorspace colorspace, enum class GammaCurve curve, bool narrowRange, const fb::Texture* tex);

    virtual u32 GetRingSize() override;
    virtual std::string Name() const override;
	MzVec2u GetSuitableDispatchSize() const;
	MzVec2u Extent() const;
    bool IsInput() const;
    void AJAInputProc();
    void AJAOutputProc();
    void CreateRings(u32 size);
    void SendDeleteRequest();
    void InputUpdate(AJADevice::Mode &prevMode);
    void Refresh();
    bool IsQuad() const;
    bool Interlaced() const;

    void StartThread();

    void Orphan(bool);
    void Live(bool);
    void PinUpdate(Action orphan, Action live);

    void Stop();
    void Resize(u32 size);
    void SetFrame(u32 FB);
    ~CopyThread();

    u32 BitWidth() const;

    void UpdateCurve(enum GammaCurve curve);

    std::array<f64, 2> GetCoeffs() const;

    template<class T>
    glm::mat<4,4,T> GetMatrix() const;
};

} // namespace mz