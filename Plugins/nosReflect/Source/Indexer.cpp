// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

namespace nos::reflect
{
NOS_REGISTER_NAME(Index)
NOS_REGISTER_NAME(Indexer)

struct Indexer : NodeContext
{
	std::optional<nos::TypeInfo> Type = std::nullopt;

    u32 Index = 0;
    u32 ArraySize = 0;
    
    Indexer(const nosFbNode* inNode) : NodeContext(inNode)
    {
        for (auto pin : *inNode->pins())
        {
			if(pin->name()->string_view() == NSN_Output)
            {
                if (pin->type_name()->string_view() != NSN_VOID)
                {
					Type = nos::TypeInfo(nos::Name(pin->type_name()->string_view()));
                }
            }
			else if (pin->name()->string_view() == NSN_Index) 
			{
				if (flatbuffers::IsFieldPresent(pin, fb::Pin::VT_DATA))
			        Index = *(u32*)pin->data()->Data();
            }
        }
    }

    nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
    {
		if (params->InstigatorPinName == NSN_Input)
		{
            nos::TypeInfo info(params->IncomingTypeName);
			if (info->BaseType != NOS_BASE_TYPE_ARRAY)
			{
                strcpy(params->OutErrorMessage, "Input pin must be an array type");
				return NOS_RESULT_FAILED;
			}
            
            nos::Name elementName = info->ElementType->TypeName;

            for (size_t i = 0; i < params->PinCount; i++)
            {
                auto& pinInfo = params->Pins[i];
				if (pinInfo.Name == NSN_Output)
                {
					pinInfo.OutResolvedTypeName = elementName;
					break;
				}
            }

            return NOS_RESULT_SUCCESS;
        }
        else if (params->InstigatorPinName == NSN_Output)
        {
            nos::TypeInfo info(params->IncomingTypeName);
            if (info->BaseType == NOS_BASE_TYPE_ARRAY)
            {
				strcpy(params->OutErrorMessage, "Output pin must not be an array type");
				return NOS_RESULT_FAILED;
            }
			nos::Name arrayName = nos::Name("[" + nos::Name(info.TypeName).AsString() + "]");
            for (size_t i = 0; i < params->PinCount; i++)
			{
				auto& pinInfo = params->Pins[i];
				if (pinInfo.Name == NSN_Input)
				{
					pinInfo.OutResolvedTypeName = arrayName;
					break;
				}
			}

            return NOS_RESULT_SUCCESS;
        }
		return NOS_RESULT_FAILED;
	}
    
	void OnPinUpdated(nosPinUpdate const* update) override
	{
		if (Type || update->UpdatedField != NOS_PIN_FIELD_TYPE_NAME)
			return;
		if (update->PinName == NSN_Output)
		{
			Type = nos::TypeInfo(update->TypeName);
		}
		else if (update->PinName == NSN_Input)
		{
			auto newTypeName = nos::Name(update->TypeName);
			auto typeInfo = nos::TypeInfo(newTypeName);
			if (typeInfo->BaseType == NOS_BASE_TYPE_ARRAY)
			{
				newTypeName = typeInfo->ElementType->TypeName;
			}
			Type = nos::TypeInfo(newTypeName);
		}
		UpdateInputVectorSize();
	}

	bool SetIndex(u32 newIndex)
    {
		Index = newIndex;
    	if (Index >= ArraySize)
    	{
    		SetNodeStatusMessage("Array index out of bounds", fb::NodeStatusMessageType::FAILURE);
    		return false;
    	}
    	ClearNodeStatusMessages();
    	return true;
    }

	bool UpdateInputVectorSize() {
		nosBuffer value;
		std::vector<u8> data;

		if (NOS_RESULT_SUCCESS == nosEngine.GetDefaultValueOfType(Type->TypeName, &value))
		{
			data = std::vector<u8>{ (u8*)value.Data, (u8*)value.Data + value.Size };
		}

		std::vector<const void*> datas = { data.data() };

		auto inPin = GetPin(NSN_Input);
		if (!inPin || !Type)
			return false;

		auto outval = GenerateVector(*Type, datas);

		auto vec = (flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*)(outval.data());
		nosEngine.SetPinValue(inPin->Id, { outval.data(), outval.size() });
		return true;
	}
	
    nosResult ExecuteNode(nosNodeExecuteParams* params) override
    {
		if (!Type)
			return NOS_RESULT_FAILED;

		auto pins = NodeExecuteParams(params);
		if (!pins[NSN_Input].Data)
		{
			UpdateInputVectorSize();
		}

		auto vec = (flatbuffers::Vector<u8>*)(pins[NSN_Input].Data->Data);
    	ArraySize = vec->size();
		if (!SetIndex(*(u32*)pins[NSN_Index].Data->Data))
			return NOS_RESULT_FAILED;
		auto ID = pins[NSN_Output].Id;
		auto& type = *Type;
		if (type->ByteSize)
		{
			auto data = vec->data() + Index * type->ByteSize;
			nosEngine.SetPinValue(ID, {(void*)data, type->ByteSize});
		}
		else
		{
			flatbuffers::FlatBufferBuilder fbb;
			auto vect = (flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*)(pins[NSN_Input].Data->Data);
			auto elem = vect->Get(Index);
			fbb.Finish(flatbuffers::Offset<flatbuffers::Table>(CopyTable(fbb, type, elem)));
			nos::Buffer buf = fbb.Release();
			nosEngine.SetPinValue(ID, buf);
		}
		return NOS_RESULT_SUCCESS;
    }

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
    {
        if (pinName == NSN_Index)
        {
        	SetIndex(*(u32*)value.Data);
		}
        else if (pinName == NSN_Input)
        {
			if (!Type)
				return;
            ArraySize = ((flatbuffers::Vector<u8>*)(value.Data))->size();
			SetIndex(Index);
		}
	}
};

nosResult RegisterIndexer(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Indexer, Indexer, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos