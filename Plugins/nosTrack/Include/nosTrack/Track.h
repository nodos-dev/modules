// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#pragma once

#define GLM_FORCE_SWIZZLE

#if defined(_WIN32)
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#endif

#include <Nodos/Utils/Thread.hpp>
#include <AppService_generated.h>
#include "Nodos/Plugin.hpp"
#include <nosTrack/Track_generated.h>
#include <glm/glm.hpp>
#include <glm/gtx/vec_swizzle.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <queue>
#include <unordered_map>
#include <vector>
#include <functional>
#include <algorithm>

// The track base (TrackNodeContext + NetworkJitter) is header-only: every consumer
// (nos.track's own nodes, zd.track's proprietary trackers) compiles the base into its
// OWN plugin DLL. This is required for correctness - all SDK access goes through the
// per-DLL `nosEngine`, which is only populated for the DLL the engine initializes, so
// the base must run in the consumer's DLL. It also keeps nos.track shippable as a header
// only (no .lib / .cpp to export).

using asio::ip::udp;
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint32_t uint32;
typedef int32_t int32;

NOS_REGISTER_NAME(UDP_Port);
NOS_REGISTER_NAME(Enable);
NOS_REGISTER_NAME_SPACED(TimeBased_Mode, "TimeBased Mode");
NOS_REGISTER_NAME_SPACED(TimeBased_Delay, "TimeBased Delay");
NOS_REGISTER_NAME(EncoderDelayOverride);
NOS_REGISTER_NAME(EncoderDelay);
NOS_REGISTER_NAME(Track);
NOS_REGISTER_NAME(NegateX);
NOS_REGISTER_NAME(NegateY);
NOS_REGISTER_NAME(NegateZ);
NOS_REGISTER_NAME(NegatePan);
NOS_REGISTER_NAME(NegateTilt);
NOS_REGISTER_NAME(NegateRoll);
NOS_REGISTER_NAME(CoordinateSystem);
NOS_REGISTER_NAME_SPACED(RotationSystem, "Pan_Tilt_Roll");
NOS_REGISTER_NAME(TransformScale);
NOS_REGISTER_NAME(EnableEffectiveFOV);
NOS_REGISTER_NAME(DevicePosition);
NOS_REGISTER_NAME(DeviceRotation);
NOS_REGISTER_NAME(CameraPosition);
NOS_REGISTER_NAME(CameraRotation);
NOS_REGISTER_NAME(ZoomRange);
NOS_REGISTER_NAME(FocusRange);
NOS_REGISTER_NAME(Sync);
NOS_REGISTER_NAME_SPACED(Spare_Count, "Spare Count");
NOS_REGISTER_NAME(NeverStarve);
NOS_REGISTER_NAME(CenterShiftRatio);
NOS_REGISTER_NAME_SPACED(Timing_Jitter, "Timing Jitter");

namespace nos::track
{

// --- Free math helpers (used by the member bodies below) ------------------------------

inline glm::mat3 MakeRotation(glm::vec3 rot)
{
	rot = glm::radians(rot);
	return (glm::mat3)glm::eulerAngleZYX(rot.z, -rot.y, -rot.x);
}

inline glm::vec3 GetEulers(glm::mat4 mat)
{
	float x, y, z;
	glm::extractEulerAngleZYX(mat, z, y, x);
	return glm::degrees(glm::vec3(-x, -y, z));
}

inline float LinearInterpolation(float y0, float y1, float mid)
{
	// this can both interpolate and also extrapolate
	if (abs((y1 - y0)) < 0.001f)
		return (y0 + y1) / 2.f;
	return y0 + (1.f - mid) * (y1 - y0);
}

inline float LinearAngleInterpolation(float angle1, float angle2, float mid)
{
	if (angle1 < -150.f && angle2 > +150.f)
		angle1 += 360.f;
	if (angle1 > +150.f && angle2 < -150.f)
		angle2 += 360.f;
	float result = LinearInterpolation(angle1, angle2, mid);
	if (result > 180.f)
		result -= 360.f;
	else if (result <= -180.f)
		result += 360.f;
	return result;
}

inline double CalculateR(double R, glm::dvec2 k1k2)
{
	double R2 = R * R;
	double R4 = R2 * R2;
	return k1k2.x * R2 + k1k2.y * R4 + 1;
}

inline double CalculateRoot(double TargetR, glm::dvec2 k1k2, double InitialR)
{
	double R = InitialR;
	for (int t = 0; t < 10; ++t)
	{
		double R2 = R * R;
		double R3 = R2 * R;
		double R4 = R2 * R2;
		double R5 = R3 * R2;
		double fR = k1k2.x * R3 + k1k2.y * R5 + R - TargetR;
		double dfR = 3 * k1k2.x * R2 + 5 * k1k2.y * R4 + 1;
		double hR = fR / dfR;
		R = R - hR;
	}
	return R;
}

inline float CalculateDistortionScale(float AspectRatio, glm::vec2 k1k2)
{
	auto AspectVector = glm::vec2(AspectRatio, 1);
	float X = sqrt(1.0f / (AspectVector.x * AspectVector.x + AspectVector.y * AspectVector.y));
	auto AspectRatioVector = AspectVector * X;
	glm::vec2 P = glm::vec2(0., 1.) * AspectRatioVector;
	float PLength = glm::length(P);
	float YMin = (float)CalculateRoot(PLength, k1k2, 1.0f) / PLength;
	glm::vec2 PMin = P;
	int32 IterCount = 1000;
	float IterStep = 1.f / IterCount;
	for (int32 Iter = 0; Iter < IterCount; ++Iter)
	{
		P = glm::vec2(Iter * IterStep, 1.0) * AspectRatioVector;
		PLength = glm::length(P);
		float Y = (float)CalculateRoot(PLength, k1k2, 1.0f) / PLength;
		if (Y < YMin)
		{
			YMin = Y;
			PMin = P;
		}
	}
	return (float)CalculateR(glm::length(PMin), k1k2);
}

// --- NetworkJitter (ported from zd.track) ---------------------------------------------
// Surfaced on the Timing Jitter pin and as node status messages.

enum JitterLevel { LOW, MODERATE, HIGH };

class NetworkJitter
{
public:
	void SetDeltaSeconds(double ds)
	{
		ExpectedTimingGapMs = (float)(ds * 1000.f);
	}

	void PacketArrived()
	{
		using namespace std::chrono;
		auto currentTime = steady_clock::now();

		if (!HasFirstPackageArrived)
		{
			LastReceivedPacketTime = currentTime;
			HasFirstPackageArrived = true;
			return;
		}

		float lastArrivalGap = duration_cast<microseconds>(currentTime - LastReceivedPacketTime).count() / 1000.0f;
		float newJitter = std::abs(lastArrivalGap - ExpectedTimingGapMs);
		MaxJitter = std::max(MaxJitter, newJitter);
		CurrentJitter = (JitterWeight * newJitter) + (1.0f - JitterWeight) * CurrentJitter;
		LastReceivedPacketTime = currentTime;

		if (CurrentJitter <= 1.0f)
			SetJitterStatus(JitterLevel::LOW);
		else if (CurrentJitter <= 5.0f)
			SetJitterStatus(JitterLevel::MODERATE);
		else
			SetJitterStatus(JitterLevel::HIGH);
	}

	std::function<void(JitterLevel status)> OnJitterStatusChanged = nullptr;

	float GetCurrentJitter()
	{
		return CurrentJitter;
	}

	void Reset()
	{
		CurrentJitter = 0.0f;
		MaxJitter = 0.0f;
		HasFirstPackageArrived = false;
		SetJitterStatus(JitterLevel::LOW);
	}

private:
	void SetJitterStatus(JitterLevel status)
	{
		if (JitterStatus == status)
			return;
		JitterStatus = status;
		if (OnJitterStatusChanged)
			OnJitterStatusChanged(status);
	}

	std::chrono::time_point<std::chrono::steady_clock> LastReceivedPacketTime;
	float ExpectedTimingGapMs = 0.0f;
	float CurrentJitter = 0.0f; // ms
	float MaxJitter = 0.0f; // ms
	JitterLevel JitterStatus = JitterLevel::LOW;
	std::atomic_bool HasFirstPackageArrived = false;
	float JitterWeight = 0.01f; // [0, 1]
};

// Time-stamped track sample used by the time-based (delay/interpolation) receiver mode.
struct TimedTrack
{
	track::TTrack track;
	std::chrono::high_resolution_clock::time_point time;
};

struct TrackNodeContext : public NodeContext, public nos::Thread
{
public:
	std::mutex QMutex;
	std::atomic_uint Port;
	std::atomic_uint SpareCount = 1;
	std::atomic_bool ShouldRestart = false;
	bool RestartPending = false; // Restart requested, waiting for enough fresh execute time samples
	std::atomic_bool NeverStarve = false;
	std::atomic_bool UDPConnected = false;
	std::deque<TimedTrack> DataQueue;
	static constexpr size_t TRACK_RECONCILE_SAMPLE_COUNT = 5;
	static constexpr size_t MAX_QUEUED_TRACKS = 512;
	std::deque<std::chrono::high_resolution_clock::time_point> ExecuteTimes;
	std::atomic_uint LastServedFrameNumber = 0;

	// Time-based receiver mode (ported from zd.track): instead of popping one queued
	// sample per frame, keep a ring of time-stamped samples and interpolate the one at
	// (now - delay). A separate encoder delay can drive fov/zoom/focus independently.
	std::atomic_bool UseTimedTrack = false;
	std::vector<TimedTrack> DataVector;
	std::atomic_uint DelayInMs = 1;
	std::atomic_bool EncoderDelayOverride = false;
	std::atomic_uint EncoderDelayInMs = 1;

	// Network-jitter measurement (ported from zd.track).
	NetworkJitter Jitter;
	std::chrono::time_point<std::chrono::steady_clock> JitterLastUpdatedTime = std::chrono::steady_clock::now();

	struct TransformMapping
	{
		glm::bvec3 NegatePos = {};
		glm::bvec3 NegateRot = {};
		bool EnableEffectiveFOV = true;
		float TransformScale = 1.f;
		nos::track::CoordinateSystem CoordinateSystem = track::CoordinateSystem::XYZ;
		nos::track::RotationSystem   RotationSystem = track::RotationSystem::PTR;
		glm::vec3 DevicePosition = {};
		glm::vec3 DeviceRotation = {};
		glm::vec3 CameraPosition = {};
		glm::vec3 CameraRotation = {};
		float CenterShiftRatio = 1.f;
	};
	TransformMapping Args = {};

	virtual ~TrackNodeContext() { Stop(); }

	// Derived receiver nodes implement the wire-protocol parse.
	virtual bool Parse(std::vector<uint8_t> const& data, track::TTrack& out) = 0;

	enum class StatusType
	{
		Jitter,
		Feed,
	};
	void SetStatus(StatusType statusType, fb::NodeStatusMessageType msgType, std::string text)
	{
		std::unique_lock lock(StatusMutex);
		auto it = StatusMessages.find(statusType);
		if (it != StatusMessages.end() && it->second.type == msgType && it->second.text == text)
			return; // Only send statuses to the Engine when they change
		StatusMessages[statusType] = fb::TNodeStatusMessage{{}, std::move(text), msgType};
		UpdateStatus();
	}

	void ClearStatus(StatusType statusType)
	{
		std::unique_lock lock(StatusMutex);
		if (StatusMessages.erase(statusType))
			UpdateStatus();
	}

	void UpdateStatus()
	{
		std::vector<fb::TNodeStatusMessage> messages;
		for (auto& [type, message] : StatusMessages)
			messages.push_back(message);
		SetNodeStatusMessages(messages);
	}

	std::mutex StatusMutex;
	std::unordered_map<StatusType, fb::TNodeStatusMessage> StatusMessages;

	nosResult OnCreate(nos::fb::Node const* node) override
	{
		bool enable = 0;
		for (auto* pin : *node->pins())
		{
			auto str = pin->name()->str();
			LoadField<uint32_t>(pin, NSN_UDP_Port, Port);
			LoadField<uint32_t>(pin, NSN_Spare_Count, SpareCount);
			LoadField<bool>(pin, NSN_NegateX, Args.NegatePos.x);
			LoadField<bool>(pin, NSN_NegateY, Args.NegatePos.y);
			LoadField<bool>(pin, NSN_NegateZ, Args.NegatePos.z);
			LoadField<bool>(pin, NSN_NegatePan, Args.NegateRot.z);
			LoadField<bool>(pin, NSN_NegateTilt, Args.NegateRot.y);
			LoadField<bool>(pin, NSN_NegateRoll, Args.NegateRot.x);
			LoadField<float>(pin, NSN_TransformScale, Args.TransformScale);
			LoadField<track::CoordinateSystem>(pin, NSN_CoordinateSystem, Args.CoordinateSystem);
			LoadField<track::RotationSystem>(pin, NSN_RotationSystem, Args.RotationSystem);
			LoadField<glm::vec3>(pin, NSN_DevicePosition, Args.DevicePosition);
			LoadField<glm::vec3>(pin, NSN_DeviceRotation, Args.DeviceRotation);
			LoadField<glm::vec3>(pin, NSN_CameraPosition, Args.CameraPosition);
			LoadField<glm::vec3>(pin, NSN_CameraRotation, Args.CameraRotation);
			LoadField<bool>(pin, NSN_Enable, enable);
			LoadField<bool>(pin, NSN_TimeBased_Mode, UseTimedTrack);
			LoadField<uint32_t>(pin, NSN_TimeBased_Delay, DelayInMs);
			LoadField<bool>(pin, NSN_EncoderDelayOverride, EncoderDelayOverride);
			LoadField<uint32_t>(pin, NSN_EncoderDelay, EncoderDelayInMs);
			LoadField<float>(pin, NSN_CenterShiftRatio, Args.CenterShiftRatio);
		}
		DataVector.resize(MAX_QUEUED_TRACKS);
		ShouldRestart = true;
		if (enable)
			Start();
		Jitter.OnJitterStatusChanged = std::bind(&TrackNodeContext::JitterStatusChanged, this, std::placeholders::_1);
		return NOS_RESULT_SUCCESS;
	}

	void JitterStatusChanged(JitterLevel jl)
	{
		switch (jl)
		{
		case JitterLevel::LOW:
			SetStatus(StatusType::Jitter, fb::NodeStatusMessageType::INFO, "Low Network Jitter (<1 ms)");
			break;
		case JitterLevel::MODERATE:
			SetStatus(StatusType::Jitter, fb::NodeStatusMessageType::WARNING, "Moderate Network Jitter (>1 ms)");
			break;
		case JitterLevel::HIGH:
			SetStatus(StatusType::Jitter, fb::NodeStatusMessageType::FAILURE, "High Network Jitter (>5 ms)");
			break;
		}
	}

	void UpdateJitterPinValue()
	{
		const int interval = 2; // Every 2 seconds update jitter pin value

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - JitterLastUpdatedTime).count();
		if (elapsed >= interval)
		{
			nos::Buffer jitterBuf = nos::Buffer::From(Jitter.GetCurrentJitter());
			nosEngine.SetPinValueByName(NodeId, NSN_Timing_Jitter, { .Data = jitterBuf.Data(), .Size = jitterBuf.Size() });
			JitterLastUpdatedTime = now;
		}
	}

	void OnPathStart() override
	{
		ShouldRestart = true;
	}

	void OnPathStop() override
	{
	}

	void OnPathCommand(const nosPathCommand* command) override
	{
		switch (command->Event)
		{
		case nosPathEvent::NOS_RING_SIZE_CHANGE:
			{
				ShouldRestart = true;
				nosEngine.LogW("Track queue will be reset", "");
				break;
			}
		}
	}

	// t0 = older data, t1 = newer data; 'time' shall be between t0.time and t1.time
	TimedTrack InterpolateTimedTrack(const TimedTrack& t0, const TimedTrack& t1, std::chrono::high_resolution_clock::time_point time)
	{
		using namespace std::chrono;
		float timeDiff = duration<float, std::milli>(t1.time - t0.time).count();
		float mid = duration<float, std::milli>(t1.time - time).count();

		if (timeDiff < 0.001f)
			return t0;

		float midPoint = glm::clamp(mid / timeDiff, 0.f, 1.f);

		track::TTrack t = t0.track;
		t.location.mutate_x(LinearInterpolation(t0.track.location.x(), t1.track.location.x(), midPoint));
		t.location.mutate_y(LinearInterpolation(t0.track.location.y(), t1.track.location.y(), midPoint));
		t.location.mutate_z(LinearInterpolation(t0.track.location.z(), t1.track.location.z(), midPoint));
		t.rotation.mutate_x(LinearAngleInterpolation(t0.track.rotation.x(), t1.track.rotation.x(), midPoint));
		t.rotation.mutate_y(LinearAngleInterpolation(t0.track.rotation.y(), t1.track.rotation.y(), midPoint));
		t.rotation.mutate_z(LinearAngleInterpolation(t0.track.rotation.z(), t1.track.rotation.z(), midPoint));
		t.fov = LinearInterpolation(t0.track.fov, t1.track.fov, midPoint);
		t.zoom = LinearInterpolation(t0.track.zoom, t1.track.zoom, midPoint);
		t.focus = LinearInterpolation(t0.track.focus, t1.track.focus, midPoint);

		TimedTrack res;
		res.track = t;
		res.time = time;
		return res;
	}

	track::TTrack GetTrack(int delay, int encoderDelay)
	{
		int indexFound = -1;
		if (DataVector.size() == 0)
		{
			auto defaultTrackData = GetDefaultValueOfType(NOS_NAME_STATIC(nos::track::Track::GetFullyQualifiedName()));
			track::TTrack t;
			if (defaultTrackData)
				flatbuffers::GetRoot<nos::track::Track>(defaultTrackData->Data())->UnPackTo(&t);
			return t;
		}

		auto now = std::chrono::high_resolution_clock::now();
		auto targetTime = now - std::chrono::milliseconds(delay * 1);
		auto targetEncoderTime = now - std::chrono::milliseconds(encoderDelay * 1);

		track::TTrack t;
		if (targetTime > DataVector[0].time)
		{
			t = DataVector[0].track;
		}
		else
		{
			for (int i = 1; i < (int)DataVector.size(); i++)
			{
				if (targetTime > DataVector[i].time && targetTime < DataVector[i - 1].time)
				{
					indexFound = i - 1;
					t = InterpolateTimedTrack(DataVector[i], DataVector[i - 1], targetTime).track;
					break;
				}
			}
		}

		if (delay != encoderDelay)
		{
			if (targetEncoderTime > DataVector[0].time)
			{
				t.fov = DataVector[0].track.fov;
				t.zoom = DataVector[0].track.zoom;
				t.focus = DataVector[0].track.focus;
			}
			else
			{
				for (int i = 1; i < (int)DataVector.size(); i++)
				{
					if (targetEncoderTime > DataVector[i].time && targetEncoderTime < DataVector[i - 1].time)
					{
						auto encoderTrack = InterpolateTimedTrack(DataVector[i], DataVector[i - 1], targetEncoderTime).track;
						t.fov = encoderTrack.fov;
						t.zoom = encoderTrack.zoom;
						t.focus = encoderTrack.focus;
						break;
					}
				}
			}
		}

		if (indexFound != -1)
			return t;
		return DataVector.front().track;
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		NodeExecuteParams p(params);
		if (ShouldRestart)
		{
			ShouldRestart = false;
			// Samples from before the restart would skew the average into the past; collect fresh
			// ones and reconcile the queue once enough frames have passed.
			ExecuteTimes.clear();
			RestartPending = true;
		}
		ExecuteTimes.push_back(std::chrono::high_resolution_clock::now());
		if (ExecuteTimes.size() > TRACK_RECONCILE_SAMPLE_COUNT)
			ExecuteTimes.pop_front();
		if (RestartPending && ExecuteTimes.size() >= TRACK_RECONCILE_SAMPLE_COUNT)
		{
			Restart(p.GetDeltaTime());
			RestartPending = false;
		}

		std::unique_lock<std::mutex> guard(QMutex);

		// Network-jitter tracking runs in both receiver modes.
		Jitter.SetDeltaSeconds(p.GetDeltaTime());
		UpdateJitterPinValue();

		// Time-based mode: interpolate the sample at (now - delay) from the ring buffer,
		// with an optional independent encoder delay for fov/zoom/focus.
		if (UseTimedTrack)
		{
			auto trackData = GetTrack(DelayInMs, EncoderDelayOverride ? EncoderDelayInMs : DelayInMs);
			nos::Buffer ttrackBuf = UpdateTrackOut(trackData);
			nosEngine.SetPinValueByName(NodeId, NSN_Track, { .Data = ttrackBuf.Data(), .Size = ttrackBuf.Size() });
			return NOS_RESULT_SUCCESS;
		}

		if (DataQueue.empty())
		{
			if (NeverStarve)
				return NOS_RESULT_SUCCESS;

			if (IsRunning())
			{
				SetStatus(StatusType::Feed, fb::NodeStatusMessageType::WARNING, "No track data");
				return NOS_RESULT_PENDING;
			}
			return NOS_RESULT_FAILED;
		}

		nos::Buffer trackBuf = UpdateTrackOut(DataQueue.front().track);
		nosEngine.SetPinValueByName(NodeId, NSN_Track, { .Data = trackBuf.Data(), .Size = trackBuf.Size() });
		DataQueue.pop_front();
		ClearStatus(StatusType::Feed);
		return NOS_RESULT_SUCCESS;
	}

	void SignalRestart()
	{
		nosEngine.SendPathRestart(NodeId);
	}

	void OnPinValueChanged(nos::Name pinName, uuid const& pinId, nosBuffer val) override
	{
#define SET_VALUE(ty, name, var) if(pinName == NOS_NAME_STATIC(#name)) Args.var = *(ty*)value;

		void* value = val.Data;
		SET_VALUE(bool, NegateX, NegatePos.x);
		SET_VALUE(bool, NegateY, NegatePos.y);
		SET_VALUE(bool, NegateZ, NegatePos.z);

		SET_VALUE(bool, NegatePan, NegateRot.z);
		SET_VALUE(bool, NegateTilt, NegateRot.y);
		SET_VALUE(bool, NegateRoll, NegateRot.x);

		SET_VALUE(bool, EnableEffectiveFOV, EnableEffectiveFOV);
		SET_VALUE(float, TransformScale, TransformScale);
		SET_VALUE(track::CoordinateSystem, CoordinateSystem, CoordinateSystem);
		SET_VALUE(track::RotationSystem, Pan_Tilt_Roll, RotationSystem);

		SET_VALUE(glm::vec3, DevicePosition, DevicePosition);
		SET_VALUE(glm::vec3, DeviceRotation, DeviceRotation);
		SET_VALUE(glm::vec3, CameraPosition, CameraPosition);
		SET_VALUE(glm::vec3, CameraRotation, CameraRotation);
		SET_VALUE(float, CenterShiftRatio, CenterShiftRatio);
#undef SET_VALUE

		if (pinName == NSN_Enable)
		{
			SignalRestart();
			auto enable = *(bool*)value;
			if (enable)
			{
				if (!IsRunning())
				{
					Stop();
					Start();
				}
			}
			else
			{
				SetPinOrphanState(NSN_Track, fb::PinOrphanStateType::PASSIVE, "Not enabled");
				Stop();
			}
			return;
		}

		if (pinName == NSN_UDP_Port)
		{
			auto newPort = *(uint16_t*)value;
			bool wasRunning = IsRunning();
			Stop();
			Port = newPort;
			if (wasRunning)
				Start();
			return;
		}

		if (pinName == NSN_NeverStarve)
		{
			NeverStarve = *(bool*)val.Data;
			return;
		}

		if (pinName == NSN_TimeBased_Mode)
		{
			UseTimedTrack = *(bool*)val.Data;
			SignalRestart();
			return;
		}
		if (pinName == NSN_TimeBased_Delay)
		{
			DelayInMs = *(uint32_t*)value;
			SignalRestart();
			return;
		}
		if (pinName == NSN_EncoderDelayOverride)
		{
			EncoderDelayOverride = *(bool*)val.Data;
			SignalRestart();
			return;
		}
		if (pinName == NSN_EncoderDelay)
		{
			EncoderDelayInMs = *(uint32_t*)value;
			SignalRestart();
			return;
		}

		if (pinName == NOS_NAME_STATIC("Spare Count"))
		{
			SpareCount = *(uint32_t*)value;
			SignalRestart();
			return;
		}
	}

	void Restart(double deltaSeconds)
	{
		using namespace std::chrono;
		if (deltaSeconds <= 0.0 || ExecuteTimes.empty())
			return;
		auto interval = duration_cast<high_resolution_clock::duration>(duration<double>(deltaSeconds));
		auto tolerance = interval / 2;

		// Anchor the frame grid to the node's execute times, since the queue is consumed once per
		// execute. Average the last few execute times, each projected forward to the latest frame,
		// to smooth per-frame scheduling jitter.
		auto base = ExecuteTimes.front();
		high_resolution_clock::duration sum{};
		for (size_t i = 0; i < ExecuteTimes.size(); i++)
			sum += (ExecuteTimes[i] - base) + interval * (int64_t)(ExecuteTimes.size() - 1 - i);
		auto executeTime = base + sum / (int64_t)ExecuteTimes.size();

		// Serve tracks SpareCount + 1 frames behind, so the queue keeps that many spares against jitter
		auto firstTrackTime = executeTime - interval * ((int64_t)SpareCount.load() + 1);

		std::unique_lock<std::mutex> guard(QMutex);

		if (!DataQueue.empty())
		{
			// Receive times carry network jitter too; smooth them like the execute times by averaging
			// the first few queued tracks, each projected back to the front slot.
			auto sampleCount = (int64_t)std::min(DataQueue.size(), TRACK_RECONCILE_SAMPLE_COUNT);
			auto frontBase = DataQueue.front().time;
			high_resolution_clock::duration frontSum{};
			for (int64_t i = 0; i < sampleCount; i++)
				frontSum += (DataQueue[i].time - frontBase) - interval * i;
			auto frontTime = frontBase + frontSum / sampleCount;

			// Tracks received before the first-track time are stale; drop them. A front within half a
			// frame of it remains as a suitable first track.
			auto stale = (firstTrackTime - frontTime + tolerance) / interval;
			for (int64_t i = 0; i < stale && !DataQueue.empty(); i++)
				DataQueue.pop_front();

			// If the oldest track was received after the first-track time, tracks for the frames in
			// between were missed; fill their slots with default track data.
			auto missing = std::min((frontTime - firstTrackTime + tolerance) / interval, (int64_t)MAX_QUEUED_TRACKS);
			if (missing > 0)
			{
				track::TTrack defaultTrack;
				auto defaultTrackData = GetDefaultValueOfType(NOS_NAME_STATIC(nos::track::Track::GetFullyQualifiedName()));
				if (defaultTrackData)
					flatbuffers::GetRoot<nos::track::Track>(defaultTrackData->Data())->UnPackTo(&defaultTrack);
				for (int64_t i = missing - 1; i >= 0; i--)
					DataQueue.push_front({ defaultTrack, firstTrackTime + interval * i });
			}
		}

		DataVector.resize(MAX_QUEUED_TRACKS);
		Jitter.Reset();
	}

	virtual nos::Buffer UpdateTrackOut(track::TTrack& outTrack)
	{
		// Guard against uninitialized/zero sensor size (would divide-by-zero on aspect
		// ratio below); emit a sane default track instead (ported from zd.track).
		if (outTrack.sensor_size.x() < 0.001f || outTrack.sensor_size.y() < 0.001f)
		{
			track::TTrack zeroTrack;
			zeroTrack.sensor_size.mutate_x(9.6f);
			zeroTrack.sensor_size.mutate_y(5.4f);
			zeroTrack.fov = 90.0f;
			zeroTrack.pixel_aspect_ratio = 1.0f;
			zeroTrack.lens_distortion.mutate_distortion_scale(1.0f);
			return nos::Buffer::From(zeroTrack);
		}

		auto xf = Args;

		glm::vec3 pos = Swizzle(reinterpret_cast<glm::vec3&>(outTrack.location), xf.NegatePos, (uint8_t)xf.CoordinateSystem);
		glm::vec3 rot = glm::zyx(Swizzle(glm::zyx(reinterpret_cast<glm::vec3&>(outTrack.rotation)), glm::zyx(xf.NegateRot), (uint8_t)xf.RotationSystem));

		auto CR = MakeRotation(Args.CameraRotation);
		auto TR = MakeRotation(rot);
		auto DR = MakeRotation(Args.DeviceRotation);

		glm::vec3 finalPos = DR * (TR * Args.CameraPosition + pos) + Args.DevicePosition;
		glm::vec3 finalRot = GetEulers(DR * TR * CR);
		reinterpret_cast<glm::vec3&>(outTrack.location) = finalPos * Args.TransformScale;
		reinterpret_cast<glm::vec3&>(outTrack.rotation) = finalRot;

		auto aspectRatio = outTrack.sensor_size.x() / outTrack.sensor_size.y();
		auto& outDistortion = outTrack.lens_distortion;
		outDistortion.mutate_distortion_scale(CalculateDistortionScale(aspectRatio, glm::vec2(outDistortion.k1k2().x(), outDistortion.k1k2().y())));

		(glm::vec2&)outTrack.lens_distortion.mutable_center_shift() *= Args.CenterShiftRatio;

		if (xf.EnableEffectiveFOV)
		{
			outTrack.fov = glm::degrees(2.0f * (atan((outDistortion.distortion_scale() / 2.0f) * 2.0f * tan(glm::radians(outTrack.fov / 2.0f)))));
		}

		return nos::Buffer::From(outTrack);
	}

	void Run() override
	{
		flatbuffers::FlatBufferBuilder fbb;
		HandleEvent(
			nos::CreateAppEvent(fbb, nos::app::CreateSetThreadNameDirect(fbb, (uint64_t)StdThread.native_handle(), "Track")));

		asio::io_service io_serv;
		nos::rc<udp::socket> sock;
		while (!ShouldStop && !sock)
		{
			try
			{
				sock = MakeShared<udp::socket>(io_serv, udp::v4());
				sock->set_option(udp::socket::reuse_address(true));
				sock->set_option(asio::detail::socket_option::integer<SOL_SOCKET, SO_RCVTIMEO>{1000});
				sock->bind(udp::endpoint(udp::v4(), Port));
				SetPinOrphanState(NSN_Track, fb::PinOrphanStateType::ACTIVE);
			}
			catch (const  asio::system_error& e)
			{
				SetPinOrphanState(NSN_Track, fb::PinOrphanStateType::PASSIVE, ("Could not open UDP socket " + std::to_string(Port.load()) + ": " + e.what()).c_str());
				nosEngine.LogW("could not open UDP socket %d: %s", Port.load(), e.what());
				std::this_thread::sleep_for(std::chrono::seconds(2));
				sock = nullptr;
			}
		}
		uint8_t buf[4096];
		auto defaultTrackData = GetDefaultValueOfType(NOS_NAME_STATIC(nos::track::Track::GetFullyQualifiedName()));
		auto defaultTrack = defaultTrackData->As<track::TTrack>();
		bool reviveFromOrphanOnFirstSuccess = false;
		while (!ShouldStop)
		{
			try
			{
				udp::endpoint sender_endpoint;
				size_t len = sock->receive_from(asio::buffer(buf, 4096), sender_endpoint);
				Jitter.PacketArrived();
				{
					track::TTrack data = defaultTrack;
					if (Parse(std::vector<uint8_t>{buf, buf + len}, data))
					{
						std::unique_lock<std::mutex> guard(QMutex);
						TimedTrack tt;
						tt.track = data;
						tt.time = std::chrono::high_resolution_clock::now();
						if (UseTimedTrack)
						{
							// Time-based mode: keep a time-stamped ring of the most recent samples.
							ShiftElementsRight(DataVector);
							DataVector[0] = tt;
						}
						else
						{
							// If no one is consuming (e.g. path stopped), drop oldest tracks to bound the queue
							while (DataQueue.size() >= MAX_QUEUED_TRACKS)
								DataQueue.pop_front();
							DataQueue.push_back(tt);
							nosEngine.WatchLog((NodeName.AsString() + " Track Queue Size").c_str(), std::to_string(DataQueue.size()).c_str());
							if (reviveFromOrphanOnFirstSuccess)
							{
								reviveFromOrphanOnFirstSuccess = false;
								SetPinOrphanState(NSN_Track, fb::PinOrphanStateType::ACTIVE);
							}
						}
					}
				}
			}
			catch (const  asio::system_error& e)
			{
				nosEngine.LogW("Exception when listening on port %d: %s", Port.load(), e.what());
				SetPinOrphanState(NSN_Track, fb::PinOrphanStateType::PASSIVE, ("Exception when listening on port " + std::to_string(Port.load()) + ": " + e.what()).c_str());
				reviveFromOrphanOnFirstSuccess = true;
			}
		}
		if (sock && sock->is_open())
		{
			// Shut down both directions first so a pending blocking receive_from() aborts
			// immediately (rather than waiting out the recv timeout) and the port is released
			// deterministically for a fast re-bind (ported from zd.track).
			sock->shutdown(asio::socket_base::shutdown_both);
			sock->close();
			sock = nullptr;
		}
		SetPinOrphanState(NSN_Track, fb::PinOrphanStateType::PASSIVE, "UDP thread is not active");
	}

	template<class T>
	static bool LoadField(nos::fb::Pin const* pin, nos::Name field, auto& dst)
	{
		if (field.Compare(pin->name()->c_str()) == 0)
			if (flatbuffers::IsFieldPresent(pin, fb::Pin::VT_DATA))
			{
				dst = *(T*)pin->data()->data();
				return true;
			}
		return false;
	}

	template <class T>
	static void ShiftElementsRight(std::vector<T>& v)
	{
		for (int i = (int)v.size() - 1; i > 0; i--)
			v[i] = v[i - 1];
	}

protected:
	glm::vec3 Swizzle(glm::vec3 v, glm::bvec3 n, uint8_t control)
	{
		// Free-function swizzles (glm::gtx/vec_swizzle) rather than the .zyx() member form,
		// so this header compiles regardless of whether GLM_FORCE_SWIZZLE was set before glm.
		if (control & 0b001) v = glm::zyx(v);
		if (control & 0b010) v = glm::yzx(v);
		if (control & 0b100) v = glm::zxy(v);
		return glm::mix(v, -v, n);
	}
};

}
