// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#pragma once

#define GLM_FORCE_SWIZZLE

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <nosUtil/Thread.h>
#include <AppService_generated.h>
#include "Nodos/PluginHelpers.hpp"
#include <glm/glm.hpp>
#include <asio.hpp>
#include <atomic>
#include <cmath>


using asio::ip::udp;
typedef uint8_t uint8;
typedef int8_t int8;
typedef uint32_t uint32;
typedef int32_t int32;

inline static NOS_REGISTER_NAME(UDP_Port);
inline static NOS_REGISTER_NAME(Enable);
inline static NOS_REGISTER_NAME(Track);
inline static NOS_REGISTER_NAME(Delay);
inline static NOS_REGISTER_NAME(NegateX);
inline static NOS_REGISTER_NAME(NegateY);
inline static NOS_REGISTER_NAME(NegateZ);
inline static NOS_REGISTER_NAME(NegatePan);
inline static NOS_REGISTER_NAME(NegateTilt);
inline static NOS_REGISTER_NAME(NegateRoll);
inline static NOS_REGISTER_NAME(CoordinateSystem);
inline static NOS_REGISTER_NAME_SPACED(RotationSystem, "Pan_Tilt_Roll");
inline static NOS_REGISTER_NAME(TransformScale);
inline static NOS_REGISTER_NAME(EnableEffectiveFOV);
inline static NOS_REGISTER_NAME(DevicePosition);
inline static NOS_REGISTER_NAME(DeviceRotation);
inline static NOS_REGISTER_NAME(CameraPosition);
inline static NOS_REGISTER_NAME(CameraRotation);
inline static NOS_REGISTER_NAME(ZoomRange);
inline static NOS_REGISTER_NAME(FocusRange);
inline static NOS_REGISTER_NAME(Sync);
inline static NOS_REGISTER_NAME_SPACED(Spare_Count, "Spare Count");
inline static NOS_REGISTER_NAME(AutoSpare);
inline static NOS_REGISTER_NAME(AutoSpareMaxJitter);
inline static NOS_REGISTER_NAME(NeverStarve);
inline static NOS_REGISTER_NAME(CenterShiftRatio);

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
	std::queue<std::pair<fb::TTrack, uint64_t>> DataQueue;
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
		f32 TransformScale = 1.f;
		nos::fb::CoordinateSystem CoordinateSystem = fb::CoordinateSystem::XYZ;
		nos::fb::RotationSystem   RotationSystem = fb::RotationSystem::PTR;
		glm::vec3 DevicePosition = {};
		glm::vec3 DeviceRotation = {};
		glm::vec3 CameraPosition = {};
		glm::vec3 CameraRotation = {};
		f32 CenterShiftRatio = 1.f;
	};
  TransformMapping Args = {};

public:
	TrackNodeContext(nos::fb::Node const* node);
	virtual ~TrackNodeContext() {}
	virtual bool Parse(std::vector<u8> const& data, fb::TTrack& out) = 0;
	void OnPathStart() override;	
	void OnPathCommand(const nosPathCommand* command) override;
	void PerformAutoSpare(uint64_t firstVBLTime);
	nosResult ExecuteNode(nosNodeExecuteParams* params) override;
	void SignalRestart();
	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer val)  override;
	void Restart();
	fb::TTrack GetDefaultOrFirstTrack();
	virtual nos::Buffer UpdateTrackOut(fb::TTrack& outTrack);
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
	glm::vec3 Swizzle(glm::vec3 v, glm::bvec3 n, u8 control);
};
}