// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

namespace nos::reflect
{
NOS_REGISTER_NAME(Arithmetic)
NOS_REGISTER_NAME(A)
NOS_REGISTER_NAME(B)

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
	
static void MapScalarToT(nosTypeInfo ty, auto&& f)
{
    switch(ty.BaseType)
    {
    case NOS_BASE_TYPE_FLOAT:
        switch(ty.BitWidth)
        {
            case 32: return f.template operator()<f32>();
            case 64: return f.template operator()<f64>();
        }
        return;
    case NOS_BASE_TYPE_INT:
        switch(ty.BitWidth)
        {
            case 8:  return f.template operator()<i8>();
            case 16: return f.template operator()<i16>();
            case 32: return f.template operator()<i32>();
            case 64: return f.template operator()<i64>();
        }
        return;
    case NOS_BASE_TYPE_UINT:
        switch(ty.BitWidth)
        {
            case 8:  return f.template operator()<u8>();
            case 16: return f.template operator()<u16>();
            case 32: return f.template operator()<u32>();
            case 64: return f.template operator()<u64>();
        }
    }
    return;
}

static void DoOp(fb::BinaryOperator op, nosTypeInfo const& ty, u8* lhs, u8* rhs, u8* dst)
{
    switch(ty.BaseType)
    {
        case NOS_BASE_TYPE_STRUCT:
        {
            if (!ty.ByteSize) break;
            for (int i = 0; i < ty.FieldCount; ++i)
            {
                auto off = ty.Fields[i].Offset;
                DoOp(op, *ty.Fields[i].Type, lhs + off, rhs + off, dst + off);
            }
        }
        case NOS_BASE_TYPE_ARRAY:
        case NOS_BASE_TYPE_STRING:
        // (TODO)
        return;
    }

    MapScalarToT(ty, [&]<class T>() -> void { 
        switch(op)
        {
            case fb::BinaryOperator::ADD: *(T*)dst = *(T*)lhs + *(T*)rhs; break;
            case fb::BinaryOperator::SUB: *(T*)dst = *(T*)lhs - *(T*)rhs; break;
            case fb::BinaryOperator::MUL: *(T*)dst = *(T*)lhs * *(T*)rhs; break;
            case fb::BinaryOperator::DIV: 
            {
                if (T(0) != *(T*)rhs)
                    *(T*)dst = *(T*)lhs / *(T*)rhs; 
                else nosEngine.LogW("Division by zero!");
                break;
            }
            case fb::BinaryOperator::EXP: *(T*)dst = (T)std::pow(*(T*)lhs, *(T*)rhs); break;
            case fb::BinaryOperator::LOG: *(T*)dst = (T)(std::log(*(T*)lhs) / std::log(*(T*)rhs)); break;
            default:
                *(T*)dst = 0;
        }
    });
}

const char* BinaryOpToDisplayName(fb::BinaryOperator op)
{
	static std::unordered_map<fb::BinaryOperator, std::string> names;
	auto it = names.find(op);
	if (it == names.end())
		names[op] = CapitalizeFirstLetter(ToLower(fb::EnumNameBinaryOperator(op)).c_str());
	return names[op].c_str();
}

struct ArithmeticNodeContext : NodeContext
{
    std::optional<nos::TypeInfo> Type = std::nullopt;
    std::optional<fb::BinaryOperator> Operator;

    ArithmeticNodeContext(const fb::Node* node) : NodeContext(node)
    {
		std::optional<nosName> newTypeName;

		if (flatbuffers::IsFieldPresent(node, fb::Node::VT_TEMPLATE_PARAMETERS))
		{
			for (auto p : *node->template_parameters())
			{
				if ("string" == p->type_name()->str())
				{
					newTypeName = nos::Name((const char*)p->value()->Data());
					continue;
				}
				if ("nos.fb.BinaryOperator" == p->type_name()->str())
				{
					Operator = *(fb::BinaryOperator*)p->value()->Data();
					continue;
				}
			}
		}
		if (newTypeName) {
			Type = nos::TypeInfo(*newTypeName);
		}
		if (Operator)
			SetOperator(*Operator, false);
	}

	void SetOperator(fb::BinaryOperator op, bool addTemplateParams)
	{
		Operator = op;
		flatbuffers::FlatBufferBuilder fbb;
		auto displayName = fbb.CreateString(BinaryOpToDisplayName(op));
		std::vector<u8> opData = nos::Buffer::From(*Operator);
		std::vector params = {
			fb::CreateTemplateParameterDirect(fbb, "nos.fb.BinaryOperator", &opData)
		};
		auto templateParamsOffset = fbb.CreateVector(params);
		PartialNodeUpdateBuilder update(fbb);
		update.add_node_id(&NodeId);
		if(addTemplateParams)
			update.add_template_parameters(templateParamsOffset);
		update.add_display_name(displayName);
		HandleEvent(CreateAppEvent(fbb, update.Finish()));
	}

	void SetType(nosName typeName)
	{
		Type = nos::TypeInfo(typeName);
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<u8> typeData = nos::Buffer(nos::Name(Type->TypeName).AsCStr(), 1 + nos::Name(Type->TypeName).AsString().size());
		std::vector params = {
			fb::CreateTemplateParameterDirect(fbb, "string", &typeData)
		};
		auto templateParamsOffset = fbb.CreateVector(params);
		PartialNodeUpdateBuilder update(fbb);
		update.add_node_id(&NodeId);
		update.add_template_parameters(templateParamsOffset);
		HandleEvent(CreateAppEvent(fbb, update.Finish()));
	}

	void OnPinUpdated(const nosPinUpdate* update) override
	{
		if (update->UpdatedField == NOS_PIN_FIELD_TYPE_NAME)
		{
			if (!Type || Type->TypeName != update->TypeName)
				SetType(update->TypeName);
		}
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		nosTypeInfo* incomingType = nullptr;
		nosEngine.GetTypeInfo(params->IncomingTypeName, &incomingType);
		if (incomingType->AttributeCount == 0) {
			nosEngine.FreeTypeInfo(incomingType);
			return NOS_RESULT_SUCCESS;
		}
		for (int i = 0; i < incomingType->AttributeCount; ++i)
		{
			if (incomingType->Attributes[i].Name == NOS_NAME_STATIC("resource")) {
				strcpy(params->OutErrorMessage, "Resource types are not supported");
				nosEngine.FreeTypeInfo(incomingType);
				return NOS_RESULT_FAILED;
			}
		}
		nosEngine.FreeTypeInfo(incomingType);
		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		if (!Type || NSN_VOID == Type->TypeName || !Operator)
			return NOS_RESULT_SUCCESS;

		flatbuffers::FlatBufferBuilder fbb;
		NodeExecuteParams pins(params);

		nos::Buffer buf(*pins[NSN_Output].Data);
		// TODO: we can directly set execute node function ptr instead of switch case etc.
		DoOp(*Operator, *Type, (u8*)pins[NSN_A].Data->Data, (u8*)pins[NSN_B].Data->Data, (u8*)buf.Data());
		nosEngine.SetPinValue(pins[NSN_Output].Id, buf);

		return NOS_RESULT_SUCCESS;
	}

	void OnMenuRequested(const nosContextMenuRequest* request) override
	{
		if (Operator)
			return;
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<nos::ContextMenuItem>> ops;

		for (auto op : fb::EnumValuesBinaryOperator())
			ops.push_back(nos::CreateContextMenuItemDirect(fbb, BinaryOpToDisplayName(op), u32(op)));

		HandleEvent(CreateAppEvent(fbb, CreateAppContextMenuUpdateDirect(fbb, &NodeId, request->pos(), request->instigator(), &ops)));
	}

	void OnMenuCommand(nosUUID itemId, uint32_t cmd) override
	{
		if (Operator)
			return;
		SetOperator(fb::BinaryOperator(cmd), true);
	}
};

nosResult RegisterArithmetic(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Arithmetic, ArithmeticNodeContext, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos