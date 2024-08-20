// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginAPI.h>

#include <Builtins_generated.h>
#include <glm/glm.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <chrono>
#include <Nodos/PluginHelpers.hpp>

NOS_INIT()

NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

NOS_REGISTER_NAME(X);
NOS_REGISTER_NAME(Y);
NOS_REGISTER_NAME(Z);
NOS_REGISTER_NAME(Position);
NOS_REGISTER_NAME(Rotation);
NOS_REGISTER_NAME(Transformation);
NOS_REGISTER_NAME(FOV);

namespace nos::math
{

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;

#define NO_ARG

#define DEF_OP0(o, n, t) nos::fb::vec##n##t operator o(nos::fb::vec##n##t l, nos::fb::vec##n##t r) { (glm::t##vec##n&)l += (glm::t##vec##n&)r; return (nos::fb::vec##n##t&)l; }
#define DEF_OP1(n, t) DEF_OP0(+, n, t) DEF_OP0(-, n, t) DEF_OP0(*, n, t) DEF_OP0(/, n, t)
#define DEF_OP(t) DEF_OP1(2, t) DEF_OP1(3, t) DEF_OP1(4, t)

DEF_OP(u);
DEF_OP(i);
DEF_OP(d);
DEF_OP(NO_ARG);

template<class T> T Add(T x, T y) { return x + y; }
template<class T> T Sub(T x, T y) { return x - y; }
template<class T> T Mul(T x, T y) { return x * y; }
template<class T> T Div(T x, T y) { return x / y; }

template<class T, T F(T, T)>
nosResult ScalarBinopExecute(void* ctx, nosNodeExecuteParams* params)
{
	auto X = static_cast<T*>(params->Pins[0].Data->Data);
	auto Y = static_cast<T*>(params->Pins[1].Data->Data);
	auto Z = static_cast<T*>(params->Pins[2].Data->Data);
	*Z = F(*X, *Y);
	return NOS_RESULT_SUCCESS;
}

template<class T, int N>
struct Vec {
	T C[N] = {};

	Vec() = default;

	template<class P>
	Vec(const P* p)  : C{}
	{
		C[0] = p->x();
		C[1] = p->y();
		if constexpr(N > 2) C[2] = p->z();
		if constexpr(N > 3) C[3] = p->w();
	}
	
	template<T F(T,T)>
	Vec Binop(Vec r) const
	{
		Vec<T, N> result = {};
		for(int i = 0; i < N; i++)
			result.C[i] = F(C[i], r.C[i]);
		return result;  
	}
	
	Vec operator +(Vec r) const { return Binop<Add>(r); }
	Vec operator -(Vec r) const { return Binop<Sub>(r); }
	Vec operator *(Vec r) const { return Binop<Mul>(r); }
	Vec operator /(Vec r) const { return Binop<Div>(r); }
};

template<class T, int Dim, Vec<T,Dim>F(Vec<T,Dim>,Vec<T,Dim>)>
nosResult VecBinopExecute(void* ctx, nosNodeExecuteParams* params)
{
	auto X = static_cast<Vec<T, Dim>*>(params->Pins[0].Data->Data);
	auto Y = static_cast<Vec<T, Dim>*>(params->Pins[1].Data->Data);
	auto Z = static_cast<Vec<T, Dim>*>(params->Pins[2].Data->Data);
	*Z = F(*X, *Y);
	return NOS_RESULT_SUCCESS;
}

#define NODE_NAME(op, t, sz, postfix) \
	op ##_ ##t ##sz ##postfix

#define ENUM_GEN_INTEGER_NODE_NAMES(op, t) \
	NODE_NAME(op, t, 8, ) , \
	NODE_NAME(op, t, 16, ) , \
	NODE_NAME(op, t, 32, ) , \
	NODE_NAME(op, t, 64, ) ,

#define ENUM_GEN_FLOAT_NODE_NAMES(op) \
	NODE_NAME(op, f, 32, ) , \
	NODE_NAME(op, f, 64, ) ,

#define ENUM_GEN_VEC_NODE_NAMES_DIM(op, dim) \
	NODE_NAME(op, vec, dim, u), \
	NODE_NAME(op, vec, dim, i), \
	NODE_NAME(op, vec, dim, d), \
	NODE_NAME(op, vec, dim, ),

#define ENUM_GEN_VEC_NODE_NAMES(op) \
	ENUM_GEN_VEC_NODE_NAMES_DIM(op, 2) \
	ENUM_GEN_VEC_NODE_NAMES_DIM(op, 3) \
	ENUM_GEN_VEC_NODE_NAMES_DIM(op, 4)

#define ENUM_GEN_NODE_NAMES(op) \
	ENUM_GEN_INTEGER_NODE_NAMES(op, u) \
	ENUM_GEN_INTEGER_NODE_NAMES(op, i) \
	ENUM_GEN_FLOAT_NODE_NAMES(op) \
	ENUM_GEN_VEC_NODE_NAMES(op)

#define ENUM_GEN_NODE_NAMES_ALL_OPS() \
	ENUM_GEN_NODE_NAMES(Add) \
	ENUM_GEN_NODE_NAMES(Sub) \
	ENUM_GEN_NODE_NAMES(Mul) \
	ENUM_GEN_NODE_NAMES(Div)

#define GEN_CASE_SCALAR(op, t, sz) \
	case MathNodeTypes::NODE_NAME(op, t, sz, ): { \
		node->ClassName = NOS_NAME_STATIC("nos.math." #op "_" #t #sz); \
		node->ExecuteNode = ScalarBinopExecute<t ##sz, op<t ##sz>>; \
		break; \
	}

#define GEN_CASE_INTEGER(op, t) \
	GEN_CASE_SCALAR(op, t, 8) \
	GEN_CASE_SCALAR(op, t, 16) \
	GEN_CASE_SCALAR(op, t, 32) \
	GEN_CASE_SCALAR(op, t, 64)

#define GEN_CASE_INTEGERS(op) \
	GEN_CASE_INTEGER(op, u) \
	GEN_CASE_INTEGER(op, i)

#define GEN_CASE_FLOAT(op) \
	GEN_CASE_SCALAR(op, f, 32) \
	GEN_CASE_SCALAR(op, f, 64)

#define GEN_CASE_VEC(op, namePostfix, t, dim) \
	case MathNodeTypes::NODE_NAME(op, vec, dim, namePostfix): { \
		node->ClassName = NOS_NAME_STATIC("nos.math." #op "_vec" #dim #namePostfix); \
		node->ExecuteNode = VecBinopExecute<t, dim, op>; \
		break; \
	}

#define GEN_CASE_VEC_ALL_DIMS(op, namePostfix, t) \
	GEN_CASE_VEC(op, namePostfix, t, 2) \
	GEN_CASE_VEC(op, namePostfix, t, 3) \
	GEN_CASE_VEC(op, namePostfix, t, 4)

#define GEN_CASE_VEC_ALL_TYPES(op) \
	GEN_CASE_VEC_ALL_DIMS(op, u, u32) \
	GEN_CASE_VEC_ALL_DIMS(op, i, i32) \
	GEN_CASE_VEC_ALL_DIMS(op, d, f64) \
	GEN_CASE_VEC_ALL_DIMS(op, , f32)

#define GEN_CASES(op) \
	GEN_CASE_INTEGERS(op) \
	GEN_CASE_FLOAT(op) \
	GEN_CASE_VEC_ALL_TYPES(op)

#define GEN_ALL_CASES() \
	GEN_CASES(Add) \
	GEN_CASES(Sub) \
	GEN_CASES(Mul) \
	GEN_CASES(Div)

enum class MathNodeTypes : int {
	ENUM_GEN_NODE_NAMES_ALL_OPS()
	U32ToString, // TODO: Generate other ToString nodes too.
	SineWave,
	Clamp,
	Absolute,
	AddTrack,
	AddTransform,
	PerspectiveView,
	Eval,
	Transform,
	ToTransformMatrix,
	Inverse,
	Transpose,
	Count
};

template<class T>
nosResult ToString(void* ctx, nosNodeExecuteParams* params)
{
	auto* in = static_cast<u32*>(params->Pins[0].Data->Data);
	auto s = std::to_string(*in);
	return nosEngine.SetPinValue(params->Pins[1].Id, nosBuffer { .Data = (void*)s.c_str(), .Size = s.size() + 1 });
}

template<u32 hi, class F, u32 i = 0>
void For(F&& f)
{
	if constexpr (i < hi)
	{
		f.template operator() < i > ();
		For<hi, F, i + 1>(std::move(f));
	}
}

template<class T, class F>
void FieldIterator(F&& f)	
{
	For<T::Traits::fields_number>([f = std::move(f), ref = T::MiniReflectTypeTable()]<u32 i>() {
		using Type = std::remove_pointer_t<typename T::Traits::template FieldType<i>>;
		f.template operator() < i, Type > (ref->values ? ref->values[i] : 0);
	});
}

nosResult AddTrack(void* ctx, nosNodeExecuteParams* params)
{
	auto pins = GetPinValues(params);
	auto ids = GetPinIds(params);
	// TODO: Remove these once generic table aritmetic ops are supported
	auto* xTrack = flatbuffers::GetMutableRoot<fb::Track>(pins[NSN_X]);
	auto* yTrack = flatbuffers::GetMutableRoot<fb::Track>(pins[NSN_Y]);
	fb::TTrack sumTrack;
	xTrack->UnPackTo(&sumTrack);
	reinterpret_cast<glm::vec3&>(sumTrack.location) += reinterpret_cast<const glm::vec3&>(*yTrack->location());
	reinterpret_cast<glm::vec3&>(sumTrack.rotation) += reinterpret_cast<const glm::vec3&>(*yTrack->rotation());
	sumTrack.fov += yTrack->fov();
	sumTrack.focus += yTrack->focus();
	sumTrack.zoom += yTrack->zoom();
	sumTrack.render_ratio += yTrack->render_ratio();
	reinterpret_cast<glm::vec2&>(sumTrack.sensor_size) += reinterpret_cast<const glm::vec2&>(*yTrack->sensor_size());
	sumTrack.pixel_aspect_ratio += yTrack->pixel_aspect_ratio();
	sumTrack.nodal_offset += yTrack->nodal_offset();
	sumTrack.focus_distance += yTrack->focus_distance();
	auto& sumDistortion = sumTrack.lens_distortion;
	auto& yDistortion = *yTrack->lens_distortion();
	reinterpret_cast<glm::vec2&>(sumDistortion.mutable_center_shift()) += reinterpret_cast<const glm::vec2&>(yDistortion.center_shift());
	reinterpret_cast<glm::vec2&>(sumDistortion.mutable_k1k2()) += reinterpret_cast<const glm::vec2&>(yDistortion.k1k2());
	sumDistortion.mutate_distortion_scale(sumDistortion.distortion_scale() + yDistortion.distortion_scale());
	return nosEngine.SetPinValue(ids[NSN_Z], nos::Buffer::From(sumTrack));
}

nosResult AddTransform(void* ctx, nosNodeExecuteParams* params)
{
	auto pins = GetPinValues(params);
	auto xBuf = pins[NSN_X];
	auto yBuf = pins[NSN_Y];
	auto zBuf = pins[NSN_Z];
	FieldIterator<fb::Transform>([X = static_cast<uint8_t*>(xBuf), Y = static_cast<uint8_t*>(yBuf), Z = static_cast<uint8_t*>(zBuf)]<u32 i, class T>(auto O) {
		if constexpr (i == 2) (T&)O[Z] = (T&)O[X] * (T&)O[Y];
		else (T&)O[Z] = (T&)O[X] + (T&)O[Y];
	});
	return NOS_RESULT_SUCCESS;
}

struct SineWaveNodeContext : NodeContext
{
	using NodeContext::NodeContext;

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto ids = GetPinIds(params);
		auto pins = GetPinValues(params);
		auto amplitude = *GetPinValue<float>(pins, NOS_NAME_STATIC("Amplitude"));
		auto offset = *GetPinValue<float>(pins, NOS_NAME_STATIC("Offset"));
		auto frequency = *GetPinValue<float>(pins, NOS_NAME_STATIC("Frequency"));
		double time = (params->DeltaSeconds.x * frameCount++) / (double)params->DeltaSeconds.y;
		double sec = glm::mod(time * (double)frequency, glm::pi<double>() * 2.0);
		float result = (amplitude * glm::sin(sec)) + offset;
		*GetPinValue<float>(pins, NOS_NAME_STATIC("Out")) = result;
		//nosEngine.SetPinValue(ids[NOS_NAME_STATIC("Out")], {.Data = &result, .Size = sizeof(float)});
		return NOS_RESULT_SUCCESS;
	}

	uint64_t frameCount = 0;
};

void RegisterEval(nosNodeFunctions*);
void RegisterTransform(nosNodeFunctions*);
void RegisterToTransformMatrix(nosNodeFunctions*);
void RegisterInverse(nosNodeFunctions*);
void RegisterTranspose(nosNodeFunctions*);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outCount, nosNodeFunctions** outList)
{
	*outCount = (size_t)(MathNodeTypes::Count);
	if (!outList)
		return NOS_RESULT_SUCCESS;
	for (int i = 0; i < int(MathNodeTypes::Count); ++i)
	{
		auto node = outList[i];
		switch ((MathNodeTypes)i)
		{
			GEN_ALL_CASES()
		case MathNodeTypes::U32ToString: {
				node->ClassName = NOS_NAME_STATIC("nos.math.U32ToString");
				node->ExecuteNode = ToString<u32>;
				break;
			}
		case MathNodeTypes::SineWave: {
			NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.math.SineWave"), SineWaveNodeContext, node);
			break;
		}
		case MathNodeTypes::Clamp: {
			node->ClassName = NOS_NAME_STATIC("nos.math.Clamp");
			node->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params) {
				constexpr uint32_t PIN_IN = 0;
				constexpr uint32_t PIN_MIN = 1;
				constexpr uint32_t PIN_MAX = 2;
				constexpr uint32_t PIN_OUT = 3;
				auto valueBuf = params->Pins[PIN_IN].Data;
				auto minBuf = params->Pins[PIN_MIN].Data;
				auto maxBuf = params->Pins[PIN_MAX].Data;
				auto outBuf = params->Pins[PIN_OUT].Data;
				float value = *static_cast<float*>(valueBuf->Data);
				float min = *static_cast<float*>(minBuf->Data);
				float max = *static_cast<float*>(maxBuf->Data);
				*(static_cast<float*>(outBuf->Data)) = std::clamp(value, min, max);
				return NOS_RESULT_SUCCESS;
				};
			break;
		}
		case MathNodeTypes::Absolute: {
			node->ClassName = NOS_NAME_STATIC("nos.math.Absolute");
			node->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params) {
				constexpr uint32_t PIN_IN = 0;
				constexpr uint32_t PIN_OUT = 1;
				auto valueBuf = params->Pins[PIN_IN].Data;
				auto outBuf = params->Pins[PIN_OUT].Data;
				float value = *static_cast<float*>(valueBuf->Data);
				*(static_cast<float*>(outBuf->Data)) = std::abs(value);
				return NOS_RESULT_SUCCESS;
				};
			break;
		}
		case MathNodeTypes::AddTrack: {
			node->ClassName = NOS_NAME_STATIC("nos.math.Add_Track");
			node->ExecuteNode = AddTrack;
			break;
		}
		case MathNodeTypes::AddTransform: {
			node->ClassName = NOS_NAME_STATIC("nos.math.Add_Transform");
			node->ExecuteNode = AddTransform;
			break;
		}
		case MathNodeTypes::PerspectiveView: {
			node->ClassName = NOS_NAME_STATIC("nos.math.PerspectiveView");
			node->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params)
				{
					auto pins = GetPinValues(params);

					auto fov = *static_cast<float*>(pins[NSN_FOV]);

					// Sanity checks
					static_assert(alignof(glm::vec3) == alignof(nos::fb::vec3));
					static_assert(sizeof(glm::vec3) == sizeof(nos::fb::vec3));
					static_assert(alignof(glm::mat4) == alignof(nos::fb::mat4));
					static_assert(sizeof(glm::mat4) == sizeof(nos::fb::mat4));

					// glm::dvec3 is compatible with nos::fb::vec3d so it's safe to cast
					auto const& rot = *static_cast<glm::vec3*>(pins[NSN_Rotation]);
					auto const& pos = *static_cast<glm::vec3*>(pins[NSN_Position]);
					auto perspective = glm::perspective(fov, 16.f / 9.f, 10.f, 10000.f);
					auto view = glm::eulerAngleXYZ(rot.x, rot.y, rot.z);
					auto& out = *static_cast<glm::mat4*>(pins[NSN_Transformation]);
					out = perspective * view;
					return NOS_RESULT_SUCCESS;
				};
			break;
		}
		case MathNodeTypes::Eval: {
			RegisterEval(node);
			break;
		}
		case MathNodeTypes::Transform: {
			RegisterTransform(node);
			break;
		}
		case MathNodeTypes::ToTransformMatrix: {
			RegisterToTransformMatrix(node);
			break;
		}
		case MathNodeTypes::Inverse: {
			RegisterInverse(node);
			break;
		}
		case MathNodeTypes::Transpose: {
			RegisterTranspose(node);
			break;
		}
		default:
			break;
		}
	}
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outFunctions)
{
	outFunctions->ExportNodeFunctions = ExportNodeFunctions;
	return NOS_RESULT_SUCCESS;
}
} // extern "C"

} // namespace nos::math
