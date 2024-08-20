// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include "Track.h"
#include <glm/gtx/euler_angles.hpp>

NOS_INIT()
NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

namespace nos::track
{

TrackNodeContext::TrackNodeContext(nos::fb::Node const* node) : NodeContext(node)
{
	bool enable = 0;
	for (auto* pin : *node->pins())
	{
		auto str = pin->name()->str();
		LoadField<u32>(pin, NSN_UDP_Port, Port);
		LoadField<u32>(pin, NSN_Delay, Delay);
		LoadField<u32>(pin, NSN_Spare_Count, SpareCount);
		LoadField<bool>(pin, NSN_NegateX, Args.NegatePos.x);
		LoadField<bool>(pin, NSN_NegateY, Args.NegatePos.y);
		LoadField<bool>(pin, NSN_NegateZ, Args.NegatePos.z);
		LoadField<bool>(pin, NSN_NegatePan, Args.NegateRot.z);
		LoadField<bool>(pin, NSN_NegateTilt, Args.NegateRot.y);
		LoadField<bool>(pin, NSN_NegateRoll, Args.NegateRot.x);
		LoadField<f32>(pin, NSN_TransformScale, Args.TransformScale);
		LoadField<fb::CoordinateSystem>(pin, NSN_CoordinateSystem, Args.CoordinateSystem);
		LoadField<fb::RotationSystem>(pin, NSN_RotationSystem, Args.RotationSystem);
		LoadField<glm::vec3>(pin, NSN_DevicePosition, Args.DevicePosition);
		LoadField<glm::vec3>(pin, NSN_DeviceRotation, Args.DeviceRotation);
		LoadField<glm::vec3>(pin, NSN_CameraPosition, Args.CameraPosition);
		LoadField<glm::vec3>(pin, NSN_CameraRotation, Args.CameraRotation);
		LoadField<bool>(pin, NSN_Enable, enable);
		LoadField<f32>(pin, NSN_CenterShiftRatio, Args.CenterShiftRatio);
	}
	Restart();
	if (enable)
		Start();
}

void TrackNodeContext::OnPathCommand(const nosPathCommand* command)
{
	switch (command->Event)
	{
	case nosPathEvent::NOS_RING_SIZE_CHANGE:
		{
			ShouldRestart = true;
			nosEngine.LogW("Track queue will be reset", "");
			break;
		}
 	case nosPathEvent::NOS_FIRST_VBL_AFTER_START:
		{
			if(!AutoSpare)
				return;
			PerformAutoSpare(command->VBLTimestampNs);
			break;
		}
	}
}

/*
Do not pop anything from DataQueue until PerformAutoSpare is called if AutoSpare is true
If a track data's received time is close to VBL time(frametime * AutoSpareMaxJitter), it is considered as a suitable first track
If a track data's received time is newer than VBL time, it is considered we missed some track data, fill those missing tracks with default track data
If a track data's received time is older than VBL time, it is considered as an old track and should be removed from the queue
*/
void TrackNodeContext::PerformAutoSpare(uint64_t firstVBLTime)
{
	if(DeltaSeconds.y == 0)
		return;
	EffectiveAutoSpare = true;
	int64_t frameTime = uint64_t(DeltaSeconds.x) * 1'000'000'000ull / uint64_t(DeltaSeconds.y);
	if (firstVBLTime == 0 || frameTime == 0)
		return;
	VBLReceived = true;
	uint64_t realVBLNanosecs = firstVBLTime - (SpareCount - FramesSinceStart) * frameTime;
	uint64_t firstNewerTrackTime = 0;
	int64_t maxJitterTime = frameTime * double(AutoSpareMaxJitter);
	{
		std::unique_lock guard(QMutex);
		while (!DataQueue.empty())
		{
			uint64_t trackNanoSec = DataQueue.front().second;
			if (trackNanoSec == 0)
			{
				DataQueue.pop();
				continue;
			}
			int64_t diff = trackNanoSec - realVBLNanosecs;
			if (std::abs(diff) <= maxJitterTime)
				return; //Suitable track found, no need to change DataQueue
			if (diff > maxJitterTime)
			{
				firstNewerTrackTime = trackNanoSec;
				break;
			}
			else
				DataQueue.pop();
		}
		auto queue = DataQueue;
		while (!queue.empty())
		{
			queue.pop();
		}
	}
	if (firstNewerTrackTime)
	{
		int64_t diff = firstNewerTrackTime - realVBLNanosecs;
		if (diff > maxJitterTime)
		{
			//Add missing tracks to the front of the queue
			uint32_t missingCount = (diff + (frameTime - maxJitterTime - 1)) / frameTime;
			missingCount = std::min(missingCount, SpareCount.load());
			decltype(DataQueue) queue;
			for (int i = 0; i < missingCount; ++i)
				queue.push({ DataQueue.front().first, 0 });
			std::unique_lock guard(QMutex);
			while (!DataQueue.empty())
			{
				queue.push({ DataQueue.front().first, 0 });
				DataQueue.pop();
			}
			DataQueue = std::move(queue);
		}
	}
	else
	{
		nosEngine.LogI("No suitable track found in track queue");
		//Assume 1 track is coming
		auto defaultTrack = GetDefaultOrFirstTrack();
		std::unique_lock guard(QMutex);
		while (DataQueue.size() < SpareCount - 1)
			DataQueue.push({ defaultTrack, 0 });
	}
}

void TrackNodeContext::OnPathStart()
{
	VBLReceived = false;
	if (EffectiveAutoSpare)
		ShouldRestart = false;
	else
		Restart();
	FramesSinceStart = 0;
}

nosResult TrackNodeContext::ExecuteNode(nosNodeExecuteParams* params)
{
	DeltaSeconds = params->DeltaSeconds;
	FramesSinceStart++;
	if (EffectiveAutoSpare && !VBLReceived)
	{
		if (FramesSinceStart > 5)
			EffectiveAutoSpare = false;
		else
			return NOS_RESULT_SUCCESS;
	}
	if (!EffectiveAutoSpare && ShouldRestart)
	{
		Restart();
		ShouldRestart = false;
	}
	fb::TTrack track;
#if _DEBUG
	size_t queueSize = 0;
#endif
	{
		std::unique_lock<std::mutex> guard(QMutex);
		if (DataQueue.size() <= Delay)
		{
			if (NeverStarve)
			{
				return NOS_RESULT_SUCCESS;
			}

			if (IsRunning())
			{
				nosEngine.LogI("Thread active but no data in track queue");
				return NOS_RESULT_PENDING;
			}
			return NOS_RESULT_FAILED;
		}
		track = DataQueue.front().first;
		DataQueue.pop();
#if _DEBUG
		queueSize = DataQueue.size();
#endif
	}

#if _DEBUG
	// Try to detect if track queue is not being filled properly, this is only correct if graph is running without any problems & there is no network jitter
	if (FramesSinceStart > 5 && (queueSize == SpareCount - 1 || queueSize == SpareCount + 1))
	{
		nosEngine.LogI("Track queue size not spare count: %d", queueSize);
	}
#endif

	// Get data from queue and resize the fixed-size buffer coming from udp listener thread
	nos::Buffer trackBuf = UpdateTrackOut(track);
	nosEngine.SetPinValueByName(NodeId, NSN_Track, { .Data = trackBuf.Data(), .Size = trackBuf.Size() });
	return NOS_RESULT_SUCCESS;
}

void TrackNodeContext::SignalRestart()
{
	nosEngine.SendPathRestart(NodeId);
}

void TrackNodeContext::OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer val)
{
#define SET_VALUE(ty, name, var) if(pinName == NOS_NAME_STATIC(#name)) Args.##var = *(ty*)value;

	void* value = val.Data;
	SET_VALUE(bool, NegateX, NegatePos.x);
	SET_VALUE(bool, NegateY, NegatePos.y);
	SET_VALUE(bool, NegateZ, NegatePos.z);

	SET_VALUE(bool, NegatePan, NegateRot.z);
	SET_VALUE(bool, NegateTilt, NegateRot.y);
	SET_VALUE(bool, NegateRoll, NegateRot.x);

	SET_VALUE(bool, EnableEffectiveFOV, EnableEffectiveFOV);
	SET_VALUE(f32, TransformScale, TransformScale);
	SET_VALUE(fb::CoordinateSystem, CoordinateSystem, CoordinateSystem);
	SET_VALUE(fb::RotationSystem, Pan_Tilt_Roll, RotationSystem);

	SET_VALUE(glm::vec3, DevicePosition, DevicePosition);
	SET_VALUE(glm::vec3, DeviceRotation, DeviceRotation);
	SET_VALUE(glm::vec3, CameraPosition, CameraPosition);
	SET_VALUE(glm::vec3, CameraRotation, CameraRotation);
	SET_VALUE(f32, CenterShiftRatio, CenterShiftRatio);

	if (pinName == NOS_NAME_STATIC("Delay"))
	{
		Delay = *(u32*)value;
		SignalRestart();
		return;
	}

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
			Stop();
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

	if (pinName == NOS_NAME_STATIC("Spare Count"))
	{
		SpareCount = *(uint32_t*)value;
		SignalRestart();
		return;
	}
	if (pinName == NSN_AutoSpare)
	{
		AutoSpare = *(bool*)value;
		EffectiveAutoSpare = AutoSpare;
		SignalRestart();
		return;
	}
	if (pinName == NSN_AutoSpareMaxJitter)
	{
		AutoSpareMaxJitter = *(f32*)value;
		return;
	}
}

void TrackNodeContext::Restart()
{
	fb::TTrack defaultTrack = GetDefaultOrFirstTrack();
	std::unique_lock<std::mutex> guard(QMutex);
	while (DataQueue.size() > Delay + SpareCount)
		DataQueue.pop();
	if (EffectiveAutoSpare)
		return;
	DataQueue = {};
	while (DataQueue.size() < Delay + SpareCount + 1)
		DataQueue.push({ defaultTrack, 0 });
}

fb::TTrack TrackNodeContext::GetDefaultOrFirstTrack()
{
	{
		std::unique_lock guard(QMutex);
		if (!DataQueue.empty())
			return DataQueue.front().first;
	}
	fb::TTrack track;
	nosBuffer defaultTrackData;
	nosEngine.GetDefaultValueOfType(NOS_NAME_STATIC("nos.fb.Track"), &defaultTrackData);
	flatbuffers::GetRoot<fb::Track>(defaultTrackData.Data)->UnPackTo(&track);
	return track;
}

f64 CalculateR(f64 R, glm::dvec2 k1k2)
{
	f64 R2 = R * R;
	f64 R4 = R2 * R2;
	return k1k2.x * R2 + k1k2.y * R4 + 1;
}

f64 CalculateRoot(f64 TargetR, glm::dvec2 k1k2, f64 InitialR)
{
	f64 R = InitialR;
	for (int t = 0; t < 10; ++t)
	{
		f64 R2 = R * R;
		f64 R3 = R2 * R;
		f64 R4 = R2 * R2;
		f64 R5 = R3 * R2;
		f64 fR = k1k2.x * R3 + k1k2.y * R5 + R - TargetR; // (K1 * R2 + K2 * R4 + 1) * R - TargetR;
		f64 dfR = 3 * k1k2.x * R2 + 5 * k1k2.y * R4 + 1;
		f64 hR = fR / dfR;
		R = R - hR;
	}
	return R;
}

f32 CalculateDistortionScale(f32 AspectRatio, glm::vec2 k1k2)
{
	auto AspectVector = glm::vec2(AspectRatio, 1);
	f32 X = sqrt(1.0f / (AspectVector.x * AspectVector.x + AspectVector.y * AspectVector.y));
	auto AspectRatioVector = AspectVector * X;
	glm::vec2 P = glm::vec2(0., 1.) * AspectRatioVector;
	f32 PLength = glm::length(P);
	f32 YMin = (f32)CalculateRoot(PLength, k1k2, 1.0f) / PLength;
	glm::vec2 PMin = P;
	int32 IterCount = 1000;
	f32 IterStep = 1.f / IterCount;
	for (int32 Iter = 0; Iter < IterCount; ++Iter)
	{
		P = glm::vec2(Iter * IterStep, 1.0) * AspectRatioVector;
		PLength = glm::length(P);
		f32 Y = (f32)CalculateRoot(PLength, k1k2, 1.0f) / PLength;
		if (Y < YMin)
		{
			YMin = Y;
			PMin = P;
		}
	}
	auto PMinLength = glm::length(PMin);
	auto ScaleMinRoot = CalculateRoot(PMinLength, k1k2, 1.0);
	auto ScaleMin = ScaleMinRoot / PMinLength;
	auto SMin = (f32)CalculateR(PMinLength, k1k2);
	return SMin;
}

glm::vec3 TrackNodeContext::Swizzle(glm::vec3 v, glm::bvec3 n, u8 control)
{
	if (control & 0b001) v = v.zyx;
	if (control & 0b010) v = v.yzx;
	if (control & 0b100) v = v.zxy;
	return glm::mix(v, -v, n);
}

nos::Buffer TrackNodeContext::UpdateTrackOut(fb::TTrack& outTrack)
{
	auto xf = Args;

	glm::vec3 pos = Swizzle(reinterpret_cast<glm::vec3&>(outTrack.location), xf.NegatePos, (u8)xf.CoordinateSystem);
	glm::vec3 rot = Swizzle(reinterpret_cast<glm::vec3&>(outTrack.rotation).zyx, xf.NegateRot.zyx, (u8)xf.RotationSystem).zyx;

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
		outTrack.fov = glm::degrees(2.0f * (atan((outDistortion.distortion_scale() / 2.0f) * 2.0f * tan(glm::radians(outTrack.fov / 2.0f)))));;
	}

	return nos::Buffer::From(outTrack);
}

void TrackNodeContext::Run()
{
	flatbuffers::FlatBufferBuilder fbb;
	HandleEvent(
		nos::CreateAppEvent(fbb, nos::app::CreateSetThreadNameDirect(fbb, (u64)StdThread.native_handle(), "Track")));

	asio::io_service io_serv;
	{
		nos::rc<udp::socket> sock;
		while (!ShouldStop && !sock)
		{
			fb::UUID trackPinId = *GetPinId(NSN_Track);
			try
			{
				sock = MakeShared<udp::socket>(io_serv, udp::v4());
				sock->set_option(udp::socket::reuse_address(true));
				sock->set_option(asio::detail::socket_option::integer<SOL_SOCKET, SO_RCVTIMEO>{1000});
				sock->bind(udp::endpoint(udp::v4(), Port));
				fb::TOrphanState nonOrphanState = { .is_orphan = false };
				flatbuffers::FlatBufferBuilder fbb;
				std::vector<flatbuffers::Offset< nos::PartialPinUpdate>> updatePins = { nos::CreatePartialPinUpdate(fbb, &trackPinId, 0, nos::fb::OrphanState::Pack(fbb, &nonOrphanState)) };

				HandleEvent(CreateAppEvent(
					fbb,
					nos::CreatePartialNodeUpdateDirect(fbb, &NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &updatePins)));
			}
			catch (const  asio::system_error& e)
			{
				nos::fb::TOrphanState orphanState{ .is_orphan = true, .message = "Could not open UDP socket " + std::to_string(Port.load()) + ": " + e.what() };
				flatbuffers::FlatBufferBuilder fbb;
				std::vector<flatbuffers::Offset< nos::PartialPinUpdate>> updatePins = { nos::CreatePartialPinUpdate(fbb, &trackPinId, 0, nos::fb::OrphanState::Pack(fbb, &orphanState)) };

				HandleEvent(CreateAppEvent(fbb,
					nos::CreatePartialNodeUpdateDirect(fbb, &NodeId, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &updatePins)));

				nosEngine.LogW("could not open UDP socket %d: %s", Port.load(), e.what());

				std::this_thread::sleep_for(std::chrono::seconds(2));
				sock = nullptr;
			}
		}
		u8 buf[4096];
		nosBuffer defaultTrackData;
		nosEngine.GetDefaultValueOfType(NOS_NAME_STATIC("nos.fb.Track"), &defaultTrackData);
		nos::Buffer defaultTrackBuffer = nos::Buffer((uint8_t*)defaultTrackData.Data, defaultTrackData.Size);
		fb::TTrack defaultTrack = defaultTrackBuffer.As<fb::TTrack>();
		while (!ShouldStop)
		{
			try
			{
				udp::endpoint sender_endpoint;
				size_t len = sock->receive_from(asio::buffer(buf, 4096), sender_endpoint);
				uint64_t nanoSeconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
				{
					fb::TTrack data = defaultTrack;
					if (Parse(std::vector<u8>{buf, buf + len}, data))
					{
						std::unique_lock<std::mutex> guard(QMutex);
						DataQueue.push({ data, nanoSeconds });

						// Queue sisiyo mu?
						// while (DataQueue.size() > Delay) DataQueue.pop();
						nosEngine.WatchLog("Track Queue Size", std::to_string(DataQueue.size()).c_str());
					}
				}
			}
			catch (const  asio::system_error& e)
			{
				nosEngine.LogW("Exception when listening on port %d: %s", Port.load(), e.what());
			}
		}
		if (sock)
		{
			sock->shutdown(asio::socket_base::shutdown_both);
			sock->close();
			sock = nullptr;
		}
	}
}
glm::mat3 MakeRotation(glm::vec3 rot)
{
	rot = glm::radians(rot);
	return (glm::mat3)glm::eulerAngleZYX(rot.z, -rot.y, -rot.x);
}
glm::vec3 GetEulers(glm::mat4 mat)
{
	f32 x, y, z;
	glm::extractEulerAngleZYX(mat, z, y, x);
	return glm::degrees(glm::vec3(-x, -y, z));
}
};
