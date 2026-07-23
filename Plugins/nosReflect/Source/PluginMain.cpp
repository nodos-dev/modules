#include <Nodos/Plugin.hpp>

#include <vector>

#include <nosSysVariables/nosVariableSubsystem.h>
#include <nosTransfer/nosTransfer.h>

NOS_INIT()
NOS_SYS_VARIABLES_INIT()
NOS_TRANSFER_PLUGIN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_SYS_VARIABLES_IMPORT()
	NOS_TRANSFER_PLUGIN_IMPORT()
NOS_END_IMPORT_DEPS()

namespace nos::reflect
{

enum Nodes : size_t
{	// CPU nodes
	Make = 0,
	MakeDynamic,
	Break,
	Indexer,
	Array,
	Delay,
	Arithmetic,
	IndexOf,
	IsEqual,
	GreaterThan,
	LessThan,
	SetVariable,
	GetVariable,
	EnumToUnderlyingValue,
	EnumFromUnderlyingValue,
	CopyingRingBuffer,
	CopyingBoundedQueue,
	ObjectRingBuffer,
	BoundedObjectQueue,
	FrameRateConverter,
	Count
};

nosResult RegisterMake(nosNodeFunctions* node);
nosResult RegisterMakeDynamic(nosNodeFunctions* node);
nosResult RegisterBreak(nosNodeFunctions* node);
nosResult RegisterIndexer(nosNodeFunctions* node);
nosResult RegisterArray(nosNodeFunctions* node);
nosResult RegisterDelay(nosNodeFunctions* node);
nosResult RegisterArithmetic(nosNodeFunctions* node);
nosResult RegisterIndexOf(nosNodeFunctions* node);
nosResult RegisterIsEqual(nosNodeFunctions* node);
nosResult RegisterGreaterThan(nosNodeFunctions* node);
nosResult RegisterLessThan(nosNodeFunctions* node);
nosResult RegisterSetVariable(nosNodeFunctions* node);
nosResult RegisterGetVariable(nosNodeFunctions* node);
nosResult RegisterEnumToUnderlyingValue(nosNodeFunctions* node);
nosResult RegisterEnumFromUnderlyingValue(nosNodeFunctions* node);
nosResult RegisterCopyingRingBuffer(nosNodeFunctions* node);
nosResult RegisterCopyingBoundedQueue(nosNodeFunctions* node);
nosResult RegisterObjectRingBuffer(nosNodeFunctions* node);
nosResult RegisterBoundedObjectQueue(nosNodeFunctions* node);
nosResult RegisterFrameRateConverter(nosNodeFunctions* node);

// Preset generators for the type-driven dynamic nodes. The *ForTypes variants register presets for a
// given set of types (used with the newcomers queried on OnPostOtherPluginLoaded); the no-arg variants
// enumerate all currently-registered types once at load. All delta-based: only not-yet-emitted types.
void RegisterMakePresets();
void RegisterArithmeticNodePresets();
void RegisterMakePresetsForTypes(const nosName* typeNames, size_t count);
void RegisterArithmeticPresetsForTypes(const nosName* typeNames, size_t count);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outCount, nosNodeFunctions** outFunctions)
{
	*outCount = (size_t)(Nodes::Count);
	if (!outFunctions)
		return NOS_RESULT_SUCCESS;
	
#define GEN_CASE_NODE(name)					\
	case Nodes::name: {						\
		auto ret = Register##name(node);	\
		if (NOS_RESULT_SUCCESS != ret)		\
			return ret;						\
		break;								\
	}

	for (size_t i = 0; i < (size_t)Nodes::Count; ++i)
	{
		auto node = outFunctions[i];
		switch ((Nodes)i)
		{
			GEN_CASE_NODE(Make)
			GEN_CASE_NODE(MakeDynamic)
			GEN_CASE_NODE(Break)
			GEN_CASE_NODE(Indexer)
			GEN_CASE_NODE(Array)
			GEN_CASE_NODE(Delay)
			GEN_CASE_NODE(Arithmetic)
			GEN_CASE_NODE(IndexOf)
			GEN_CASE_NODE(IsEqual)
			GEN_CASE_NODE(GreaterThan)
			GEN_CASE_NODE(LessThan)
			GEN_CASE_NODE(SetVariable)
			GEN_CASE_NODE(GetVariable)
			GEN_CASE_NODE(EnumToUnderlyingValue)
			GEN_CASE_NODE(EnumFromUnderlyingValue)
			GEN_CASE_NODE(CopyingRingBuffer)
			GEN_CASE_NODE(CopyingBoundedQueue)
			GEN_CASE_NODE(ObjectRingBuffer)
			GEN_CASE_NODE(BoundedObjectQueue)
			GEN_CASE_NODE(FrameRateConverter)
		}
	}

#undef GEN_CASE_NODE
	return NOS_RESULT_SUCCESS;
}

extern "C"
{
/// Nodos calls this function to initialize the plugin & retrieve the plugin's functions.
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outPluginFunctions)
{
	outPluginFunctions->ExportNodeFunctions = ExportNodeFunctions;
	// When any other plugin finishes loading, ask the engine for just that plugin's data types (the
	// newcomers) - no full re-scan - and emit Make/Arithmetic quick-add presets for them. Delta-based, so
	// the engine firing once per loaded plugin only ever registers new types.
	outPluginFunctions->OnPostOtherPluginLoaded = [](nosPluginIdentifier pluginId) {
		size_t count = 0;
		if (nosEngine.GetPinDataTypeNames(&pluginId, nullptr, &count) == NOS_RESULT_FAILED || count == 0)
			return;
		std::vector<nosName> types(count);
		nosEngine.GetPinDataTypeNames(&pluginId, types.data(), &count);
		RegisterMakePresetsForTypes(types.data(), types.size());
		RegisterArithmeticPresetsForTypes(types.data(), types.size());
	};
	outPluginFunctions->GetRenamedTypes = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outCount) {
		*outCount = 1;
		if (!outRenamedFrom)
			return;
		outRenamedFrom[0] = NOS_NAME("nos.fb.BinaryOperator");
		outRenamedTo[0] = NOS_NAME("nos.reflect.BinaryOperator");
	};
	outPluginFunctions->GetRenamedNodeClasses = [](nosName* outRenamedFrom, nosName* outRenamedTo, size_t* outCount) {
		constexpr auto ops = std::array{"Add", "Mul", "Sub", "Div"};
		constexpr auto types = std::array{"f32",	"f64",	 "i8",	 "i16",	  "i32",   "i64",	"u8",	"u16",
							   "u32",	"u64",	 "vec2", "vec2i", "vec2u", "vec2d", "vec3", "vec3i",
							   "vec3u", "vec3d", "vec4", "vec4i", "vec4u", "vec4d"};
		// The scalar-broadcast and generic arithmetic nodes are folded into nos.reflect.Arithmetic; route
		// their saved instances there too. Arithmetic::MigrateNode fixes up pins/params afterwards.
		*outCount = ops.size() * types.size() + 2;
		if (!outRenamedFrom)
			return;
		auto idx = 0;
		for (const char* op : ops)
			for (const char* type : types)
			{
				outRenamedFrom[idx] = nos::Name("nos.math." + std::string(op) + "_" + std::string(type));
				outRenamedTo[idx] = NOS_NAME("nos.reflect.Arithmetic");
				idx++;
			}
		outRenamedFrom[idx] = NOS_NAME("nos.reflect.ScalarArithmetic");
		outRenamedTo[idx] = NOS_NAME("nos.reflect.Arithmetic");
		idx++;
		outRenamedFrom[idx] = NOS_NAME("nos.reflect.ArithmeticDynamic");
		outRenamedTo[idx] = NOS_NAME("nos.reflect.Arithmetic");
		idx++;
	};
	return NOS_RESULT_SUCCESS;
}
}

}