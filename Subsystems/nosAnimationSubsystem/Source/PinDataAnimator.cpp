#include "PinDataAnimator.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/ext/quaternion_common.hpp>


namespace nos::sys::anim
{

/// https://probablymarcus.com/blocks/2015/02/26/using-bezier-curves-as-easing-functions.html
/// Adapted from https://greweb.me/2012/02/bezier-curve-based-easing-functions-from-concept-to-implementation
/// MIT License - Copyright (c) 2012 Gaetan Renaudeau <renaudeau.gaetan@gmail.com>
struct CubicBezierEasing
{
	CubicBezierEasing(glm::vec2 p1, glm::vec2 p2) : P1(p1), P2(p2) {}

	/// x monotonically increasing from 0 to 1 with constant velocity (time)
	double Get(double x)
	{
		if (P1.x == P1.y && P2.x == P2.y)
			return x; // linear
		return Bezier(GetPercent(x), P1.y, P2.y);
	}
   
protected:
	glm::vec2 P1, P2; // Control points

	// Horner's method
	double A(double c1, double c2) { return 1.0 - 3.0 * c2 + 3.0 * c1; }
	double B(double c1, double c2) { return 3.0 * c2 - 6.0 * c1; }
	double C(double c1) { return 3.0 * c1; }

	// Return x(d) given d, x1, and x2, or y(d) given d, y1, and y2.
	// d, percentage of the distance along the curve
	double Bezier(double d, double c1, double c2) { return ((A(c1, c2) * d + B(c1, c2)) * d + C(c1)) * d; }

	double GetSlope(double d, double c1, double c2) { return 3.0 * A(c1, c2) * d * d + 2.0 * B(c1, c2) * d + C(c1); }

	double GetPercent(double x)
	{
		// Newton Raphson iteration
		// TODO: Optimize
		double guess = x;
		for (int i = 0; i < 32; ++i)
		{
			double currentSlope = GetSlope(guess, P1.x, P2.x);
			if (currentSlope == 0.0)
				return guess;
			double currentX = Bezier(guess, P1.x, P2.x) - x;
			guess -= currentX / currentSlope;
		}
		return guess;
	}
};

template <typename T>
requires std::is_arithmetic_v<T>
T Lerp(T start, T end, const double t)
{
	return start + (end - start) * t;
}

template <typename T>
requires std::is_arithmetic_v<T>
T Ease(T start, T end, nos::fb::vec2 const& control1, nos::fb::vec2 const& control2, const double t)
{
	return start + (end - start) * CubicBezierEasing(reinterpret_cast<glm::vec2 const&>(control1), reinterpret_cast<glm::vec2 const&>(control2)).Get(t);
}

template <typename T, size_t Dim>
requires (Dim == 2 || Dim == 3 || Dim == 4)
T LerpVec(T start, T end, const double t)
{
	T newData{};
	newData.mutate_x(Lerp(start.x(), end.x(), t));
	newData.mutate_y(Lerp(start.y(), end.y(), t));
	if constexpr (Dim >= 3)
		newData.mutate_z(Lerp(start.z(), end.z(), t));
	if constexpr (Dim == 4)
		newData.mutate_w(Lerp(start.w(), end.w(), t));
	return newData;
}

template <typename T, size_t Dim>
requires (Dim == 2 || Dim == 3 || Dim == 4)
T EaseVec(T start, T end, nos::fb::vec2 const& control1, nos::fb::vec2 const& control2, const double t)
{
	T newData{};
	newData.mutate_x(Ease(start.x(), end.x(), control1, control2, t));
	newData.mutate_y(Ease(start.y(), end.y(), control1, control2, t));
	if constexpr (Dim >= 3)
		newData.mutate_z(Ease(start.z(), end.z(), control1, control2, t));
	if constexpr (Dim == 4)
		newData.mutate_w(Ease(start.w(), end.w(), control1, control2, t));
	return newData;
}

template <editor::Interpolation Mode, typename T>
nos::Buffer ScalarInterpolator(editor::InterpolationUnion const& interp, const double t)
{
	T newData{};
	if constexpr (Mode == editor::Interpolation::Lerp)
	{
		auto* lerp = interp.AsLerp();
		T start = *reinterpret_cast<T const*>(lerp->start.data());
		T end = *reinterpret_cast<T const*>(lerp->end.data());
		newData = Lerp(start, end, t);
	}
	else if constexpr (Mode == editor::Interpolation::CubicBezier)
	{
		auto* bezier = interp.AsCubicBezier();
		T start = *reinterpret_cast<T const*>(bezier->start.data());
		T end = *reinterpret_cast<T const*>(bezier->end.data());
		newData = Ease(start, end, bezier->control1, bezier->control2, t);
	}
	return nos::Buffer(&newData, sizeof(T));
}

template <editor::Interpolation Mode, typename T, size_t Dim>
	requires(Mode == editor::Interpolation::Lerp || Mode == editor::Interpolation::CubicBezier)
nos::Buffer VectorInterpolator(editor::InterpolationUnion const& interp, const double t)
{
	T newData{};
	if constexpr (Mode == editor::Interpolation::Lerp)
	{
		auto* lerp = interp.AsLerp();
		T start = *reinterpret_cast<T const*>(lerp->start.data());
		T end = *reinterpret_cast<T const*>(lerp->end.data());
		newData = LerpVec<T, Dim>(start, end, t);
	}
	else if constexpr (Mode == editor::Interpolation::CubicBezier)
	{
		auto* bezier = interp.AsCubicBezier();
		T start = *reinterpret_cast<T const*>(bezier->start.data());
		T end = *reinterpret_cast<T const*>(bezier->end.data());
		newData = EaseVec<T, Dim>(start, end, bezier->control1, bezier->control2, t);
	}
	return nos::Buffer(&newData, sizeof(T));
}

nos::fb::vec3 InterpolateRotation(nos::fb::vec3 const& start, nos::fb::vec3 const& end, const double t)
{
	glm::vec3 glmStart(start.x(), start.y(), start.z());
	glm::vec3 glmEnd(end.x(), end.y(), end.z());
	glm::quat startQuat = glm::quat(glm::radians(glmStart));
	glm::quat endQuat = glm::quat(glm::radians(glmEnd));
	glm::quat newQuat = glm::slerp(startQuat, endQuat, (float) t);
	glm::vec3 newEuler = glm::degrees(glm::eulerAngles(newQuat));
	return {newEuler.x, newEuler.y, newEuler.z};
}

static nos::Buffer LerpTrack(const fb::Track* start, const fb::Track* end, const double t)
{
	nos::fb::TTrack interm;
	interm.location = LerpVec<fb::vec3, 3>(*start->location(), *end->location(), t);
	interm.rotation = InterpolateRotation(*start->rotation(), *end->rotation(), t); // quat
	interm.fov = Lerp(start->fov(), end->fov(), t);
	interm.focus = Lerp(start->focus(), end->focus(), t);
	interm.zoom = Lerp(start->zoom(), end->zoom(), t);
	interm.render_ratio = Lerp(start->render_ratio(), end->render_ratio(), t);
	interm.sensor_size = LerpVec<fb::vec2, 2>(*start->sensor_size(), *end->sensor_size(), t);
	interm.pixel_aspect_ratio = Lerp(start->pixel_aspect_ratio(), end->pixel_aspect_ratio(), t);
	interm.nodal_offset = Lerp(start->nodal_offset(), end->nodal_offset(), t);
	interm.focus_distance = Lerp(start->focus_distance(), end->focus_distance(), t);
	auto distortionStart = *start->lens_distortion();
	auto distortionEnd = *end->lens_distortion();
	auto& distortion = interm.lens_distortion;
	distortion.mutable_center_shift() = LerpVec<fb::vec2, 2>(distortionStart.center_shift(), distortionEnd.center_shift(), t);
	distortion.mutable_k1k2() = LerpVec<fb::vec2, 2>(distortionStart.k1k2(), distortionEnd.k1k2(), t);
	distortion.mutate_distortion_scale(Lerp(distortionStart.distortion_scale(), distortionEnd.distortion_scale(), t));
	return nos::Buffer::From(interm);
}

template <editor::Interpolation Mode>
nos::Buffer TrackInterpolator(editor::InterpolationUnion const& interp, const double t)
{
	if constexpr (Mode == editor::Interpolation::Lerp)
	{
		auto* lerp = interp.AsLerp();
		auto start = flatbuffers::GetRoot<fb::Track>(lerp->start.data());
		auto end = flatbuffers::GetRoot<fb::Track>(lerp->end.data());
		return LerpTrack(start, end, t);
	}
	else if constexpr (Mode == editor::Interpolation::CubicBezier)
	{
		auto* ease = interp.AsCubicBezier();
		auto start = flatbuffers::GetRoot<fb::Track>(ease->start.data());
		auto end = flatbuffers::GetRoot<fb::Track>(ease->end.data());
		return LerpTrack(start, end, CubicBezierEasing((glm::vec2&)ease->control1, (glm::vec2&)ease->control2).Get(t));
	}
}

template <typename T, tmp::StrLiteral TypeName>
void AddScalarInterpolators(PinDataAnimator* animator)
{
	animator->AddInterpolator<editor::Interpolation::Lerp, TypeName>(ScalarInterpolator<editor::Interpolation::Lerp, T>);
	animator->AddInterpolator<editor::Interpolation::CubicBezier, TypeName>(ScalarInterpolator<editor::Interpolation::CubicBezier, T>);
}

template <typename T, size_t Dim, tmp::StrLiteral TypeName>
void AddVectorInterpolators(PinDataAnimator* animator)
{
	animator->AddInterpolator<editor::Interpolation::Lerp, TypeName>(VectorInterpolator<editor::Interpolation::Lerp, T, Dim>);
	animator->AddInterpolator<editor::Interpolation::CubicBezier, TypeName>(VectorInterpolator<editor::Interpolation::CubicBezier, T, Dim>);
}

PinDataAnimator::PinDataAnimator()
{
	AddScalarInterpolators<int, "int">(this);
	AddScalarInterpolators<float, "float">(this);
	AddScalarInterpolators<double, "double">(this);
	AddScalarInterpolators<char, "byte">(this);
	AddScalarInterpolators<short, "short">(this);
	AddScalarInterpolators<long, "long">(this);
	AddScalarInterpolators<long long, "ulong">(this);
	AddScalarInterpolators<unsigned char, "ubyte">(this);
	AddScalarInterpolators<unsigned short, "ushort">(this);
	AddVectorInterpolators<fb::vec2, 2, "nos.fb.vec2">(this);
	AddVectorInterpolators<fb::vec3, 3, "nos.fb.vec3">(this);
	AddVectorInterpolators<fb::vec4, 4, "nos.fb.vec4">(this);
	AddVectorInterpolators<fb::vec2d, 2, "nos.fb.vec2d">(this);
	AddVectorInterpolators<fb::vec3d, 3, "nos.fb.vec3d">(this);
	AddVectorInterpolators<fb::vec4d, 4, "nos.fb.vec4d">(this);
	AddVectorInterpolators<fb::vec2u, 2, "nos.fb.vec2u">(this);
	AddVectorInterpolators<fb::vec3u, 3, "nos.fb.vec3u">(this);
	AddVectorInterpolators<fb::vec4u, 4, "nos.fb.vec4u">(this);
	AddVectorInterpolators<fb::vec2i, 2, "nos.fb.vec2i">(this);
	AddVectorInterpolators<fb::vec3i, 3, "nos.fb.vec3i">(this);
	AddVectorInterpolators<fb::vec4i, 4, "nos.fb.vec4i">(this);
	AddVectorInterpolators<fb::vec4u8, 4, "nos.fb.vec4u8">(this);
	AddInterpolator<editor::Interpolation::Lerp, "nos.fb.Track">(TrackInterpolator<editor::Interpolation::Lerp>);
	AddInterpolator<editor::Interpolation::CubicBezier, "nos.fb.Track">(TrackInterpolator<editor::Interpolation::CubicBezier>);
}

bool PinDataAnimator::AddAnimation(nosUUID const& pinId,
								   editor::AnimatePin const& buf)
{
	editor::TAnimatePin animate;
	buf.UnPackTo(&animate);
	auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					 std::chrono::high_resolution_clock::now().time_since_epoch())
					 .count();
	AnimationData data;
	data.PinId = pinId;
	data.Duration = animate.duration;
	data.StartTime = nowMs + animate.delay;
	
	data.Interp = animate.interpolate;
	nos::TypeInfo typeInfo(pinId);
	data.TypeName = typeInfo.TypeName;

	// If constant, do not look for interpolator.
	if (data.Interp.type == editor::Interpolation::Constant)
	{
		std::unique_lock lock(AnimationsMutex);
		Animations[pinId].push(std::move(data));
		return true;
	}
	if (!Interpolators.contains({data.Interp.type, data.TypeName}))
	{
		nosEngine.LogE("No interpolator found for %s and %s.", nos::Name(typeInfo.TypeName).AsCStr(), editor::EnumNameInterpolation(data.Interp.type));
		return false;
	}
	std::unique_lock lock(AnimationsMutex);
	Animations[pinId].push(std::move(data));
	return true;
}

void PinDataAnimator::UpdatePin(nosUUID const& pinId, 
								nosVec2u const& deltaSeconds,
								uint64_t curFSM,
								const nosBuffer* currentData)
{	
	std::shared_lock lock(AnimationsMutex);
	auto it = Animations.find(pinId);
	if (it == Animations.end())
		return;
	auto& animQueue = it->second;
	if (animQueue.empty())
	{
		lock.unlock();
		std::unique_lock ulock(AnimationsMutex);
		Animations.erase(pinId);
		return;
	}
	const AnimationData& animData = animQueue.top();
	int64_t diff = curFSM - MillisecondsToFrameNumber(animData.StartTime, deltaSeconds);
	if (diff < 0)
		return;
	if (animData.Started == false)
	{

		switch (animData.Interp.type)
		{
		case editor::Interpolation::Lerp: {
			auto* lerp = const_cast<editor::TLerp*>(animData.Interp.AsLerp());
			if (lerp->start.empty())
				lerp->start = std::vector<uint8_t>(reinterpret_cast<uint8_t const*>(currentData->Data), reinterpret_cast<uint8_t const*>(currentData->Data) + currentData->Size);
			break;
		}
		case editor::Interpolation::CubicBezier: {
			auto* cubic = const_cast<editor::TCubicBezier*>(animData.Interp.AsCubicBezier());
			if (cubic->start.empty())
				cubic->start = std::vector<uint8_t>(reinterpret_cast<uint8_t const*>(currentData->Data), reinterpret_cast<uint8_t const*>(currentData->Data) + currentData->Size);
			break;
		}
		default: break;
		}
		const_cast<AnimationData&>(animData).Started = true;
	}
	const double t = glm::clamp(static_cast<double>(diff) / MillisecondsToFrameNumber(animData.Duration, deltaSeconds), 0.0, 1.0);
	if (t >= 0.0)
	{
		nos::Buffer buf;
		if (animData.Interp.type == editor::Interpolation::Constant)
			buf = nos::Buffer(animData.Interp.AsConstant()->value);
		else
			buf = Interpolators[{animData.Interp.type, animData.TypeName}](animData.Interp, t);
		nosEngine.SetPinValue(pinId, buf);
	}
	if (t >= 1.0)
	{
		lock.unlock();
		std::unique_lock ulock(AnimationsMutex);
		it = Animations.find(pinId);
		if (it == Animations.end())
			return;
		auto& animQueue = it->second;
		animQueue.pop();
		if (animQueue.empty())
			Animations.erase(it);
	}
}

// TODO: AnimSys need to use this to check for should execute
bool PinDataAnimator::IsPinAnimating(nosUUID const& pinId)
{
	std::shared_lock lock(AnimationsMutex);
	return Animations.contains(pinId);
}

void PinDataAnimator::OnPinDeleted(nosUUID const& pinId)
{
	std::unique_lock lock(AnimationsMutex);
	Animations.erase(pinId);
}

std::unordered_set<nos::Name> PinDataAnimator::GetAnimatableTypes()
{ 
	std::unordered_set<nos::Name> types;
	for (auto const& [key, _] : Interpolators)
		types.insert(key.TypeName);
	return types;
}

std::optional<PathInfo> PinDataAnimator::GetPathInfo(nosUUID const& scheduledNodeId) 
{
	std::shared_lock lock(AnimationsMutex);
	auto it = PathInfos.find(scheduledNodeId);
	if (it == PathInfos.end())
		return std::nullopt;
	return it->second;
}

void anim::PinDataAnimator::CreatePathInfo(nosUUID const& scheduledNodeId, nosVec2u const& deltaSec)
{
	std::unique_lock lock(PathInfosMutex);
	uint64_t startFSM = MillisecondsToFrameNumber(std::chrono::duration_cast<std::chrono::milliseconds>(
													  std::chrono::high_resolution_clock::now().time_since_epoch())
														  .count(),
														  deltaSec);
	PathInfos[scheduledNodeId] = {.StartFSM = startFSM};
}

void anim::PinDataAnimator::DeletePathInfo(nosUUID const& scheduledNodeId)
{
	std::unique_lock lock(PathInfosMutex);
	PathInfos.erase(scheduledNodeId);
}

void anim::PinDataAnimator::PathExecutionFinished(nosUUID const& scheduledNodeId)
{
	std::unique_lock lock(PathInfosMutex);
	PathInfos[scheduledNodeId].CurFrame++;
}

} // namespace nos::sys::anim
