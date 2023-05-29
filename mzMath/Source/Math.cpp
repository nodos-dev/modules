// Copyright MediaZ AS. All Rights Reserved.

#include <MediaZ/PluginAPI.h>

#include <Builtins_generated.h>
#include <glm/glm.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <chrono>

MZ_INIT();

namespace mz::math
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

#define DEF_OP0(o, n, t) mz::fb::vec##n##t operator o(mz::fb::vec##n##t l, mz::fb::vec##n##t r) { (glm::t##vec##n&)l += (glm::t##vec##n&)r; return (mz::fb::vec##n##t&)l; }
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
MzResult ScalarBinopExecute(void* ctx, const MzNodeExecuteArgs* args)
{
	auto X = reinterpret_cast<T*>(args->PinValues[0].Data);
	auto Y = reinterpret_cast<T*>(args->PinValues[1].Data);
	auto Z = reinterpret_cast<T*>(args->PinValues[2].Data);
	*Z = F(*X, *Y);
	return MZ_RESULT_SUCCESS;
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
MzResult VecBinopExecute(void* ctx, const MzNodeExecuteArgs* args)
{
	auto X = reinterpret_cast<Vec<T, Dim>*>(args->PinValues[0].Data);
	auto Y = reinterpret_cast<Vec<T, Dim>*>(args->PinValues[1].Data);
	auto Z = reinterpret_cast<Vec<T, Dim>*>(args->PinValues[2].Data);
	*Z = F(*X, *Y);
	return MZ_RESULT_SUCCESS;
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
		functions->TypeName = "mz.math." #op "_" #t #sz; \
		functions->ExecuteNode = ScalarBinopExecute<t ##sz, op<t ##sz>>; \
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
		functions->TypeName = "mz.math." #op "_vec" #dim #namePostfix; \
		functions->ExecuteNode = VecBinopExecute<t, dim, op>; \
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

enum class MathNodeTypes {
	ENUM_GEN_NODE_NAMES_ALL_OPS()
	U32ToString, // TODO: Generate other ToString nodes too.
	SineWave,
	Clamp,
	Absolute,
	Count
};

template<class T>
MzResult ToString(void* ctx, const MzNodeExecuteArgs* args)
{
	auto* in = reinterpret_cast<u32*>(args->PinValues[0].Data);
	auto s = std::to_string(*in);
	return mzEngine.SetPinValue(args->PinIds[1], MzBuffer { .Data = (void*)s.c_str(), .Size = s.size() + 1 });
}

extern "C"
{

MZAPI_ATTR MzResult MZAPI_CALL mzExportNodeFunctions(size_t* outCount, MzNodeFunctions* outFunctions)
{
	*outCount = (size_t)(MathNodeTypes::Count);
	if (!outFunctions)
		return MZ_RESULT_SUCCESS;

	for (int nodeTypeIndex = 0; nodeTypeIndex < (int)(MathNodeTypes::Count); ++nodeTypeIndex)
	{
		auto* functions = &outFunctions[nodeTypeIndex];
		switch ((MathNodeTypes)nodeTypeIndex)
		{
		GEN_ALL_CASES()
		case MathNodeTypes::U32ToString: {
			functions->TypeName = "mz.math.U32ToString";
			functions->ExecuteNode = ToString<u32>;
			break;
		}
		case MathNodeTypes::SineWave: {
			functions->TypeName = "mz.math.SineWave";
			functions->ExecuteNode = [](void* ctx, const MzNodeExecuteArgs* args) {
				constexpr uint32_t PIN_AMPLITUDE = 0;
				constexpr uint32_t PIN_FREQUENCY = 1;
				constexpr uint32_t PIN_OUT = 2;
				auto ampBuf = &args->PinValues[PIN_AMPLITUDE];
				auto freqBuf = &args->PinValues[PIN_FREQUENCY];
				auto outBuf = &args->PinValues[PIN_OUT];
				float frequency = *static_cast<float*>(freqBuf->Data);
				float amplitude = *static_cast<float*>(ampBuf->Data);
				auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
				float sec = millis / 1000.f;
				*(static_cast<float*>(outBuf->Data)) = amplitude * sin(frequency * sec);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		case MathNodeTypes::Clamp: {
			functions->TypeName = "mz.math.Clamp";
			functions->ExecuteNode = [](void* ctx, const MzNodeExecuteArgs* args) {
				constexpr uint32_t PIN_IN = 0;
				constexpr uint32_t PIN_MIN = 1;
				constexpr uint32_t PIN_MAX = 2;
				constexpr uint32_t PIN_OUT = 3;
				auto valueBuf = &args->PinValues[PIN_IN];
				auto minBuf = &args->PinValues[PIN_MIN];
				auto maxBuf = &args->PinValues[PIN_MAX];
				auto outBuf = &args->PinValues[PIN_OUT];
				float value = *static_cast<float*>(valueBuf->Data);
				float min = *static_cast<float*>(minBuf->Data);
				float max = *static_cast<float*>(maxBuf->Data);
				*(static_cast<float*>(outBuf->Data)) = std::clamp(value, min, max);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		case MathNodeTypes::Absolute: {
			functions->TypeName = "mz.math.Absolute";
			functions->ExecuteNode = [](void* ctx, const MzNodeExecuteArgs* args) {
				constexpr uint32_t PIN_IN = 0;
				constexpr uint32_t PIN_OUT = 1;
				auto valueBuf = &args->PinValues[PIN_IN];
				auto outBuf = &args->PinValues[PIN_OUT];
				float value = *static_cast<float*>(valueBuf->Data);
				*(static_cast<float*>(outBuf->Data)) = std::abs(value);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		default:
			break;
		}
	}
	return MZ_RESULT_SUCCESS;
}
}

}

/*

template<class T, u32 I, u32 F = I*2 + 4>
inline auto AddTrackField(flatbuffers::FlatBufferBuilder& fbb, flatbuffers::Table* X, flatbuffers::Table* Y)
{
	auto l = X->GetStruct<T*>(F);
	auto r = Y->GetStruct<T*>(F);
	auto c = (l ? *l : T{}) + (r ? *r : T{});
	fbb.AddStruct(F, &c);
}

template<u32 hi, class F, u32 i = 0>
void FOR(F&& f)
{
	if constexpr(i < hi)
	{
		f.template operator()<i>();
		FOR<hi, F, i +1>(std::move(f));
	}
}

template<class T, class F>
void FieldIterator(F&& f)
{
	FOR<T::Traits::fields_number>([f=std::move(f),ref=T::MiniReflectTypeTable()]<u32 i>() {
		using Type = std::remove_pointer_t<typename T::Traits::template FieldType<i>>;
		f.template operator()<i, Type>(ref->values ? ref->values[i] : 0);
	});
}

bool AddTrack(mz::Args& pins, void*)
{
	flatbuffers::FlatBufferBuilder fbb;
	fb::Track::Builder b(fbb);
	FieldIterator<fb::Track>([&fbb, X=pins.Get<flatbuffers::Table>("X"), Y = pins.Get<flatbuffers::Table>("Y")]<u32 i, class T>(auto){ AddTrackField<T, i>(fbb, X, Y); });
	fbb.Finish(b.Finish());
	*pins.GetBuffer("Z") = fbb.Release();
	return true;
}

bool AddTransform(mz::Args& pins, void*)
{
	FieldIterator<fb::Transform>([X=pins.Get<u8>("X"),Y=pins.Get<u8>("Y"),Z=pins.Get<u8>("Z")]<u32 i, class T>(auto O) { 
		if constexpr (i == 2) (T&)O[Z] = (T&)O[X] * (T&)O[Y]; 
		else (T&)O[Z] = (T&)O[X] + (T&)O[Y]; 
	});
	return true;
}

void RegisterMath(NodeActionsMap& functions)
{
	functions["mz.math.NodeWithFunction"].NodeCreated = [](mz::fb::Node const& node, mz::Args& args, void* ctx)
	{
		std::vector<std::string> demoList = { "Item1","Item2", "AAAAAAAAa", "a" };
		GServices.UpdateItemList("DemoItems", demoList);
	};

	functions["mz.math.U32ToString"].EntryPoint = ToString<u32>;
	functions["mz.math.NodeWithFunction"].NodeFunctions["NodeAsFunctionAddF64"] = SampleNodeAddFunc;
	functions["mz.math.NodeWithFunction"].NodeFunctions["NodeAsFunctionSubF64"] = SampleNodeAddFunc;
	
	functions["mz.math.Add_Track"].EntryPoint = AddTrack;
	functions["mz.math.Add_Transform"].EntryPoint = AddTransform;

	functions["mz.math.PerspectiveView"].EntryPoint = [](mz::Args& args, void*) {
		auto fov = *args.Get<f64>("FOV");

		// Sanity checks
		static_assert(alignof(glm::dvec3) == alignof(mz::fb::vec3d));
		static_assert(sizeof(glm::dvec3) == sizeof(mz::fb::vec3d));
		static_assert(alignof(glm::dmat4) == alignof(mz::fb::mat4d));
		static_assert(sizeof(glm::dmat4) == sizeof(mz::fb::mat4d));

		// glm::dvec3 is compatible with mz::fb::vec3d so it's safe to cast
		auto rot = *args.Get<glm::dvec3>("Rotation"); 
		auto pos = *args.Get<glm::dvec3>("Position"); 
		auto perspective = glm::perspective(fov, 16.0/9.0, 10.0, 10000.0);
		auto view = glm::eulerAngleXYZ(rot.x, rot.y, rot.z);
		*args.Get<glm::dmat4>("Transformation") = perspective * view;
		return true;
	};
}

}

*/
