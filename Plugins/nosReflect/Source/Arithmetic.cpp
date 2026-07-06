// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"
#include "nosReflect/Reflect_generated.h"

#include <map>
#include <sstream>

namespace nos::reflect
{
NOS_REGISTER_NAME(Type)
NOS_REGISTER_NAME(Operator)
std::string CapitalizeFirstLetter(const char* str)
{
	std::string copy = str;
	for (int i = 0; i < copy.length(); i++)
	{
		if (i == 0)
			copy[i] = ::toupper(str[i]);
		else if (str[i - 1] == ' ')
			copy[i] = ::toupper(str[i]);
		else
			copy[i] = str[i];
	}
	return copy;
}

std::string ToLower(const char* str)
{
	std::string s(str);
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return s;
}

static bool IsScalarBaseType(nosTypeInfo const& ty)
{
	return ty.BaseType == NOS_BASE_TYPE_INT || ty.BaseType == NOS_BASE_TYPE_UINT || ty.BaseType == NOS_BASE_TYPE_FLOAT;
}

static void MapScalarToT(nosTypeInfo ty, auto&& f)
{
    switch(ty.BaseType)
    {
    case NOS_BASE_TYPE_FLOAT:
        switch(ty.BitWidth)
        {
            case 32: return f.template operator()<float>();
            case 64: return f.template operator()<double>();
        }
        return;
    case NOS_BASE_TYPE_INT:
        switch(ty.BitWidth)
        {
            case 8:  return f.template operator()<int8_t>();
            case 16: return f.template operator()<int16_t>();
            case 32: return f.template operator()<int32_t>();
            case 64: return f.template operator()<int64_t>();
        }
        return;
    case NOS_BASE_TYPE_UINT:
        switch(ty.BitWidth)
        {
            case 8:  return f.template operator()<uint8_t>();
            case 16: return f.template operator()<uint16_t>();
            case 32: return f.template operator()<uint32_t>();
            case 64: return f.template operator()<uint64_t>();
        }
    }
    return;
}

template<typename RightHandSide>
requires std::is_scalar_v<RightHandSide> || std::is_same_v<RightHandSide, void>
static void DoOp(reflect::BinaryOperator op, nosTypeInfo const& ty, const uint8_t* lhs, std::conditional_t<std::is_same_v<RightHandSide, void>, uint8_t, RightHandSide> const* rhs, uint8_t* dst)
{
	constexpr bool isRightHandVoid = std::is_same_v<RightHandSide, void>;
    switch(ty.BaseType)
    {
        case NOS_BASE_TYPE_STRUCT:
        {
            if (!ty.ByteSize) break;
            for (int i = 0; i < ty.FieldCount; ++i)
            {
                auto off = ty.Fields[i].Offset;
				if constexpr (isRightHandVoid)
					DoOp<RightHandSide>(op, *ty.Fields[i].Type, lhs + off, rhs + off, dst + off);
				else
					DoOp<RightHandSide>(op, *ty.Fields[i].Type, lhs + off, rhs, dst + off);
			}
        }
        case NOS_BASE_TYPE_ARRAY:
        case NOS_BASE_TYPE_STRING:
        // (TODO)
        return;
    }

    MapScalarToT(ty, [&]<class T>() -> void {
		using RightHandSideT = std::conditional_t<isRightHandVoid, T, RightHandSide>;
        switch(op)
        {
            case reflect::BinaryOperator::ADD: *(T*)dst = *(const T*)lhs + *(RightHandSideT*)rhs; break;
            case reflect::BinaryOperator::SUB: *(T*)dst = *(const T*)lhs - *(RightHandSideT*)rhs; break;
            case reflect::BinaryOperator::MUL: *(T*)dst = *(const T*)lhs * *(RightHandSideT*)rhs; break;
            case reflect::BinaryOperator::DIV:
            {
                if (T(0) != *(RightHandSideT*)rhs)
                    *(T*)dst = *(T*)lhs / *(RightHandSideT*)rhs;
                else nosEngine.LogW("Division by zero!");
                break;
            }
            case reflect::BinaryOperator::EXP: *(T*)dst = (T)std::pow(*(const T*)lhs, *(RightHandSideT*)rhs); break;
            case reflect::BinaryOperator::LOG: *(T*)dst = (T)(std::log(*(const T*)lhs) / std::log(*(RightHandSideT*)rhs)); break;
            default:
                *(T*)dst = 0;
        }
    });
}

void DoScalarOp(reflect::BinaryOperator op, nosTypeInfo const& ty, const uint8_t* lhs, nosTypeInfo const& scalarTy, const void* rhs, uint8_t* dst)
{
	MapScalarToT(scalarTy, [&]<class T>() -> void {
		DoOp<T>(op, ty, lhs, static_cast<const T*>(rhs), dst);
	});
}

const char* BinaryOpToDisplayName(reflect::BinaryOperator op)
{
	static std::unordered_map<reflect::BinaryOperator, std::string> names;
	auto it = names.find(op);
	if (it == names.end())
		names[op] = CapitalizeFirstLetter(ToLower(reflect::EnumNameBinaryOperator(op)).c_str());
	return names[op].c_str();
}

// Arithmetic: computes A (op) B. A and Output share one type; B either matches A (elementwise) or is a
// scalar that broadcasts across A's fields. The type flows through the pins - there is no manual pin
// (re)creation. The operator is chosen from the node's context menu (or baked in by a preset) and persists
// as a template parameter. Scalar-broadcast and per-type math (formerly ScalarArithmetic / ArithmeticDynamic
// and the legacy nos.math.* nodes) are all folded into this one node; see MigrateNode.
struct ArithmeticNode : NodeContext
{
	std::optional<nos::TypeInfo> Type = std::nullopt; // type of A / Output
	std::optional<reflect::BinaryOperator> Operator = std::nullopt;

	nosResult OnCreate(nosFbNodePtr node) override
	{
		std::optional<nos::Name> typeFromTemplate;
		if (flatbuffers::IsFieldPresent(node, fb::Node::VT_TEMPLATE_PARAMETERS))
		{
			for (auto* tp : *node->template_parameters())
			{
				auto name = tp->name()->string_view();
				if ("string" == name || NSN_Type == name)
					typeFromTemplate = nos::Name((const char*)tp->value()->Data());
				else if ("nos.reflect.BinaryOperator" == name || NSN_Operator == name)
					Operator = *InterpretObjectData<reflect::BinaryOperator>((const void*)tp->value()->Data());
			}
		}

		// A preset supplies the type via a template parameter; a saved/generic node carries it on the pins.
		nos::Name typeName = typeFromTemplate.value_or(NSN_TypeNameGeneric);
		if (typeName == NSN_TypeNameGeneric)
			if (auto* a = GetPin(NSN_A))
				typeName = a->TypeName;

		if (typeName != NSN_TypeNameGeneric && nos::TypeInfo(typeName))
		{
			// Give still-generic pins the concrete type (typed presets start generic). Pins that already
			// carry a type are left as-is, so a migrated node's scalar B pin keeps driving broadcast.
			for (nos::Name pinName : {nos::Name(NSN_A), nos::Name(NSN_B), nos::Name(NSN_Output)})
				if (auto* pin = GetPin(pinName); pin && pin->TypeName == NSN_TypeNameGeneric)
					SetPinType(pinName, typeName);
			OnTypeUpdated(typeName);
		}

		if (Operator)
			SetOperator(*Operator, false);
		return NOS_RESULT_SUCCESS;
	}

	// Types this node can operate on: reject arrays, unions, table (byte-size-less) structs and resources.
	static bool IsTypeSupported(nos::TypeInfo& info)
	{
		if (info->BaseType == NOS_BASE_TYPE_STRUCT && !info->ByteSize)
			return false;
		if (info->BaseType == NOS_BASE_TYPE_ARRAY || info->BaseType == NOS_BASE_TYPE_UNION)
			return false;
		for (int i = 0; i < info->AttributeCount; ++i)
			if (info->Attributes[i].Name == NOS_NAME_STATIC("resource"))
				return false;
		return true;
	}

	// Types worth offering in the type menus / presets: supported scalars, strings, and builtin structs
	// (vecN etc.) - not raw tables, enums or engine-internal types.
	static bool IsArithmeticType(nos::TypeInfo& info)
	{
		if (info->BaseType == NOS_BASE_TYPE_NONE || IsEnumType(info) || !IsTypeSupported(info))
			return false;
		if (info->BaseType == NOS_BASE_TYPE_STRUCT)
		{
			bool builtin = false;
			for (int i = 0; i < info->AttributeCount; ++i)
				if (info->Attributes[i].Name == NOS_NAME_STATIC("builtin"))
					builtin = true;
			if (!builtin)
				return false;
		}
		return true;
	}

	// Output takes A's type (A defines the result shape for both elementwise and broadcast); until A is
	// known it previews B's type.
	static nos::Name DeriveOutputType(nos::Name aType, nos::Name bType)
	{
		return aType != NSN_TypeNameGeneric ? aType : bType;
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		nos::Name instigator(params->InstigatorPinName);
		nos::TypeInfo incoming(params->IncomingTypeName);
		if (!IsTypeSupported(incoming))
		{
			strcpy(params->OutErrorMessage, "Type not supported for arithmetic.");
			return NOS_RESULT_FAILED;
		}
		auto* aPin = GetPin(NSN_A);
		auto* bPin = GetPin(NSN_B);
		nos::Name curA = aPin ? aPin->TypeName : nos::Name(NSN_TypeNameGeneric);
		nos::Name curB = bPin ? bPin->TypeName : nos::Name(NSN_TypeNameGeneric);

		// A connection to B that neither matches A nor is a scalar cannot be combined.
		if (instigator == NSN_B && curA != NSN_TypeNameGeneric &&
			curA != nos::Name(incoming->TypeName) && !IsScalarBaseType(*incoming))
		{
			strcpy(params->OutErrorMessage, "B must match A's type or be a scalar.");
			return NOS_RESULT_FAILED;
		}

		// Resolve only the connected pin; the other input keeps its type. A connection on Output types A
		// (Output has no input type to inherit). Output is then derived from the resulting A/B.
		nos::Name aEff = (instigator == NSN_B) ? curA : nos::Name(params->IncomingTypeName);
		nos::Name bEff = (instigator == NSN_B) ? nos::Name(params->IncomingTypeName) : curB;
		for (size_t i = 0; i < params->PinCount; ++i)
		{
			auto& pin = params->Pins[i];
			nos::Name name(pin.Name);
			if (name == NSN_B)
				pin.OutResolvedTypeName = bEff;
			else if (name == NSN_Output)
				pin.OutResolvedTypeName = DeriveOutputType(aEff, bEff);
			else
				pin.OutResolvedTypeName = aEff;
		}
		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(const nosPinUpdate* update) override
	{
		if (update->UpdatedField != NOS_PIN_FIELD_TYPE_NAME)
			return;
		nos::Name pinName(update->PinName);
		if (pinName == NSN_A) // A governs the node type
			OnTypeUpdated(update->TypeName);
		if (pinName == NSN_A || pinName == NSN_B) // an input changed: recompute Output
			RefreshOutputType();
	}

	void OnTypeUpdated(nos::Name typeName)
	{
		Type = (typeName == NSN_TypeNameGeneric) ? std::nullopt : std::optional(nos::TypeInfo(typeName));
	}

	void RefreshOutputType()
	{
		auto* a = GetPin(NSN_A);
		auto* b = GetPin(NSN_B);
		auto* out = GetPin(NSN_Output);
		if (!a || !b || !out)
			return;
		nos::Name derived = DeriveOutputType(a->TypeName, b->TypeName);
		if (out->TypeName != derived)
			SetPinType(NSN_Output, derived);
	}

	void SetOperator(reflect::BinaryOperator op, bool persist)
	{
		Operator = op;
		SetNodeDisplayName(BinaryOpToDisplayName(op));
		if (!persist)
			return;
		// Persist the chosen operator as a template parameter so it survives save/load.
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<uint8_t> opData = PackObjectData(op);
		std::vector params = {
			fb::CreateTemplateParameterDirect(fbb, NSN_Operator.AsCStr(), "nos.reflect.BinaryOperator", &opData)
		};
		auto templateParamsOffset = fbb.CreateVector(params);
		PartialNodeUpdateBuilder update(fbb);
		update.add_node_id(&NodeId);
		update.add_template_parameters(templateParamsOffset);
		HandleEvent(CreateAppEvent(fbb, update.Finish()));
	}

	// --- Type / operator menus ---------------------------------------------------------------------

	enum MenuCommand : uint32_t
	{
		RESET_PIN_TYPE = 1,
		SET_PIN_TYPE_BASE = 0x10000,  // + index into AvailableTypes: set the instigating pin's type
		SET_NODE_TYPE_BASE = 0x20000, // + index into AvailableTypes: set every pin's type
		SET_OPERATOR_BASE = 0x30000,  // + BinaryOperator value: set the operator
	};

	std::vector<nos::Name> AvailableTypes;

	std::vector<nos::Name> const& GetAvailableTypes()
	{
		if (!AvailableTypes.empty())
			return AvailableTypes;
		size_t count = 0;
		if (nosEngine.GetPinDataTypeNames(nullptr, &count) == NOS_RESULT_FAILED)
			return AvailableTypes;
		std::vector<nosName> names(count);
		nosEngine.GetPinDataTypeNames(names.data(), &count);
		for (auto n : names)
			if (nos::TypeInfo info(n); info && IsArithmeticType(info))
				AvailableTypes.push_back(n);
		std::sort(AvailableTypes.begin(), AvailableTypes.end(),
				  [](nos::Name a, nos::Name b) { return a.AsString() < b.AsString(); });
		return AvailableTypes;
	}

	// Build a type-picker menu, grouping namespaced types (e.g. nos.fb.*) under nested submenus.
	void BuildTypeMenu(ContextMenuBuilder& menu, std::vector<nos::Name> const& types, uint32_t commandBase)
	{
		std::map<std::string, std::vector<size_t>> byNamespace;
		std::vector<size_t> topLevel;
		for (size_t i = 0; i < types.size(); ++i)
		{
			auto name = types[i].AsString();
			auto pos = name.rfind('.');
			if (pos != std::string::npos)
				byNamespace[name.substr(0, pos)].push_back(i);
			else
				topLevel.push_back(i);
		}
		for (size_t i : topLevel)
			menu.Item(types[i].AsString(), commandBase + uint32_t(i));
		for (auto const& [ns, indices] : byNamespace)
			menu.Submenu(ns, [&](ContextMenuBuilder& sub) {
				for (size_t i : indices)
					sub.Item(types[i].AsString(), commandBase + uint32_t(i));
			});
	}

	void OnNodeMenuRequested(nosContextMenuRequestPtr request) override
	{
		auto const& types = GetAvailableTypes();
		SendContextMenu(request, [&](ContextMenuBuilder& menu) {
			menu.Submenu("Set Operator", [](ContextMenuBuilder& sub) {
				for (auto op : reflect::EnumValuesBinaryOperator())
					sub.Item(BinaryOpToDisplayName(op), SET_OPERATOR_BASE + uint32_t(op));
			});
			menu.Submenu("Set Data Type For Node", [&](ContextMenuBuilder& sub) {
				BuildTypeMenu(sub, types, SET_NODE_TYPE_BASE);
			});
		});
	}

	void OnPinMenuRequested(nos::Name pinName, nosContextMenuRequestPtr request) override
	{
		if (pinName == NSN_Output)
			return; // Output type is derived from the inputs
		auto const& types = GetAvailableTypes();
		SendContextMenu(request, [&](ContextMenuBuilder& menu) {
			menu.Submenu("Set Data Type", [&](ContextMenuBuilder& sub) {
				BuildTypeMenu(sub, types, SET_PIN_TYPE_BASE);
			});
			menu.Item("Reset Type", RESET_PIN_TYPE);
		});
	}

	void OnMenuCommand(uuid const& itemId, uint32_t cmd) override
	{
		auto const& types = GetAvailableTypes();
		if (cmd >= SET_OPERATOR_BASE)
			SetOperator(reflect::BinaryOperator(cmd - SET_OPERATOR_BASE), true);
		else if (cmd >= SET_NODE_TYPE_BASE)
		{
			if (size_t idx = cmd - SET_NODE_TYPE_BASE; idx < types.size())
				SetNodeType(types[idx]);
		}
		else if (cmd >= SET_PIN_TYPE_BASE)
		{
			if (size_t idx = cmd - SET_PIN_TYPE_BASE; idx < types.size() && itemId != NodeId)
				SetPinType(itemId, types[idx]);
		}
		else if (cmd == RESET_PIN_TYPE && itemId != NodeId)
			SetPinType(itemId, NSN_TypeNameGeneric);
	}

	void SetNodeType(nos::Name typeName)
	{
		SetPinType(NSN_A, typeName);
		SetPinType(NSN_B, typeName);
		SetPinType(NSN_Output, typeName);
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		if (!Type || !Operator || NSN_TypeNameGeneric == (*Type)->TypeName)
			return NOS_RESULT_SUCCESS;

		auto* bPin = GetPin(NSN_B);
		if (!bPin || bPin->TypeName == NSN_TypeNameGeneric)
			return NOS_RESULT_SUCCESS;
		nos::TypeInfo bType(bPin->TypeName);

		if ((*Type)->BaseType == NOS_BASE_TYPE_STRING)
		{
			std::stringstream ss;
			ss << params.GetPinValue<const char*>(NSN_A) << params.GetPinValue<const char*>(NSN_B);
			auto str = ss.str();
			SetPinValue(NSN_Output, str.c_str());
			return NOS_RESULT_SUCCESS;
		}

		nos::Buffer outputBuf = params.GetPinBuffer(NSN_Output);
		bool broadcast = nos::Name(bType->TypeName) != nos::Name((*Type)->TypeName) && IsScalarBaseType(*bType);
		if (broadcast)
			DoScalarOp(*Operator,
					   *Type,
					   params.GetPinValue<uint8_t>(NSN_A),
					   *bType,
					   params.GetPinValue<void>(NSN_B),
					   outputBuf.As<uint8_t>());
		else
			DoOp<void>(*Operator,
					   *Type,
					   params.GetPinValue<uint8_t>(NSN_A),
					   params.GetPinValue<uint8_t>(NSN_B),
					   outputBuf.As<uint8_t>());
		SetPinValue(NSN_Output, outputBuf);
		return NOS_RESULT_SUCCESS;
	}

	// Fold older nodes into this one: the legacy nos.math.<Op>_<type> nodes, the scalar-broadcast
	// ScalarArithmetic, and the generic ArithmeticDynamic all migrate to Arithmetic. The engine routes them
	// here via GetRenamedNodeClasses; this sees the original class name and rewrites the node buffer.
	static nosResult MigrateNode(nosFbNodePtr nodePtr, nosBuffer* outBuffer)
	{
		fb::TNode tNode;
		nodePtr->UnPackTo(&tNode);
		const std::string cls = tNode.class_name;
		bool fromScalar = cls == "nos.reflect.ScalarArithmetic";
		bool fromDynamic = cls == "nos.reflect.ArithmeticDynamic";

		if (!fromScalar && !fromDynamic)
		{
			// Legacy nos.math.<Op>_<type>: derive the operator, capture the type, rename the X/Y/Z pins.
			std::optional<reflect::BinaryOperator> op{};
			if (cls.starts_with("nos.math.Add_"))
				op = reflect::BinaryOperator::ADD;
			else if (cls.starts_with("nos.math.Sub_"))
				op = reflect::BinaryOperator::SUB;
			else if (cls.starts_with("nos.math.Mul_"))
				op = reflect::BinaryOperator::MUL;
			else if (cls.starts_with("nos.math.Div_"))
				op = reflect::BinaryOperator::DIV;
			if (!op)
				return NOS_RESULT_SUCCESS; // already an Arithmetic node (or nothing we migrate)

			auto& opParam = tNode.template_parameters.emplace_back(std::make_unique<fb::TTemplateParameter>());
			opParam->name = NSN_Operator.AsString();
			opParam->value = nos::Buffer::From(op);
			opParam->type_name = "nos.reflect.BinaryOperator";

			std::string type = "float";
			for (auto& pin : tNode.pins)
			{
				type = pin->type_name;
				if (pin->name == "X")
					pin->name = NSN_A.AsString();
				else if (pin->name == "Y")
					pin->name = NSN_B.AsString();
				else if (pin->name == "Z")
					pin->name = NSN_Output.AsString();
			}
			auto& typeParam = tNode.template_parameters.emplace_back(std::make_unique<fb::TTemplateParameter>());
			typeParam->name = NSN_Type.AsString();
			typeParam->value = std::vector<uint8_t>(PackObjectData(type.c_str()));
			typeParam->type_name = "string";
		}
		else if (fromScalar)
		{
			// The scalar operand pin becomes B; broadcast is inferred from B's scalar type at execute time.
			for (auto& pin : tNode.pins)
				if (pin->name == NSN_Scalar.AsString())
					pin->name = NSN_B.AsString();
		}
		// fromDynamic: only the class name changes.

		tNode.class_name = "nos.reflect." + NSN_Arithmetic.AsString();
		*outBuffer = EngineBuffer::CopyFrom(tNode).Release();
		return NOS_RESULT_SUCCESS;
	}
};

void RegisterArithmeticNodePresets() {
	std::vector<nosName> typeNames;
	size_t count = 0;
	auto res = nosEngine.GetPinDataTypeNames(0, &count);
	if (NOS_RESULT_FAILED != res)
	{
		typeNames.resize(count);
		nosEngine.GetPinDataTypeNames(typeNames.data(), &count);
	}
	std::vector<nos::Buffer> nodePresets;
	for (uint32_t binaryOpIndx = 0; binaryOpIndx < uint32_t(BinaryOperator::MAX) + 1; binaryOpIndx++) {
		auto binaryOp = BinaryOperator(binaryOpIndx);
		std::string binaryOpStr = BinaryOpToDisplayName(binaryOp);
		for (auto& typeName : typeNames)
		{
			nos::TypeInfo typeInfo(typeName);
			if (!ArithmeticNode::IsArithmeticType(typeInfo))
				continue;
			// Non-ADD operators don't apply to strings (only concatenation is defined).
			if (binaryOp != BinaryOperator::ADD && typeInfo->BaseType == NOS_BASE_TYPE_STRING)
				continue;

			std::string name = nos::Name(typeInfo.TypeName).AsString();
			auto idx = name.find_last_of(".");
			idx = idx == std::string::npos ? 0 : 1 + idx;
			fb::TNodePreset preset;
			fb::TNodeMenuInfo info;
			info.category = "Math|" + binaryOpStr;
			info.display_name = binaryOpStr + " " + name.substr(idx);
			preset.menu_info = std::make_unique<fb::TNodeMenuInfo>(std::move(info));
			std::vector<uint8_t> data = PackObjectData(name.c_str());
			preset.params.emplace_back(new fb::TTemplateParameter{ {}, NSN_Type.AsString(), "string", std::move(data) });
			preset.params.emplace_back(new fb::TTemplateParameter{
				{}, NSN_Operator.AsString(), "nos.reflect.BinaryOperator", PackObjectData(binaryOp)});
			flatbuffers::FlatBufferBuilder fbb;
			fbb.Finish(CreateNodePreset(fbb, &preset));
			nos::Buffer buf = fbb.Release();
			nodePresets.push_back(std::move(buf));
		}
	}
	std::vector<nosFbNodePresetPtr> fbNodePresets;
	for (auto& buf : nodePresets)
		fbNodePresets.push_back(flatbuffers::GetMutableRoot<nos::fb::NodePreset>(buf.Data()));
	nosEngine.RegisterNodePresets(NSN_Arithmetic, fbNodePresets.size(), fbNodePresets.data());
}

nosResult RegisterArithmetic(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Arithmetic, ArithmeticNode, fn);
	RegisterArithmeticNodePresets();
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::reflect
