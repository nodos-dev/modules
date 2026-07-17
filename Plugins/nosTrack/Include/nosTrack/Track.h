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
#include "nosTrack/Track_generated.h"
#include <glm/glm.hpp>
#include <asio.hpp>
#include <atomic>
#include <cmath>


using asio::ip::udp;
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint32_t uint32;
typedef int32_t int32;

NOS_REGISTER_NAME(UDP_Port);
NOS_REGISTER_NAME(Enable);
NOS_REGISTER_NAME(Track);
NOS_REGISTER_NAME(Delay);
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
NOS_REGISTER_NAME(AutoSpare);
NOS_REGISTER_NAME(AutoSpareMaxJitter);
NOS_REGISTER_NAME(NeverStarve);
NOS_REGISTER_NAME(CenterShiftRatio);

namespace nos::track
{
glm::mat3 MakeRotation(glm::vec3 rot);
glm::vec3 GetEulers(glm::mat4 mat);
struct TrackNodeContext : public NodeContext, public nos::Thread
{
public:
	std::mutex QMutex;
	std::atomic_uint Port;
	std::atomic_uint Delay;
	std::atomic_uint SpareCount = 1;
	std::atomic_bool ShouldRestart = false;
	std::atomic_bool NeverStarve = false;
	std::atomic_bool UDPConnected = false;
	std::queue<std::pair<track::TTrack, uint64_t>> DataQueue;
	std::atomic_uint LastServedFrameNumber = 0;
	bool AutoSpare = true;
	float AutoSpareMaxJitter = 0.4999f;

	bool EffectiveAutoSpare = false;
	bool VBLReceived = false;
	uint64_t FramesSinceStart = 0;
	nosVec2u DeltaSeconds{};

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

public:
	nosResult OnCreate(nos::fb::Node const* node) override;
	virtual ~TrackNodeContext() {}
	virtual bool Parse(std::vector<uint8_t> const& data, track::TTrack& out) = 0;
	void OnPathStart() override;	
	void OnPathCommand(const nosPathCommand* command) override;
	void PerformAutoSpare(uint64_t firstVBLTime);
	nosResult ExecuteNode(NodeExecuteParams const& params) override;
	void SignalRestart();
	void OnPinValueChanged(nos::Name pinName, uuid const& pinId, nosBuffer val)  override;
	void Restart();
	track::TTrack GetDefaultOrFirstTrack();
	virtual nos::Buffer UpdateTrackOut(track::TTrack& outTrack);
	virtual void Run() override;
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
protected:
	glm::vec3 Swizzle(glm::vec3 v, glm::bvec3 n, uint8_t control);
};
}