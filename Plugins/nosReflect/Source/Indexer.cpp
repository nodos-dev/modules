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
    
	bool IsOrphan = false;

    Indexer(const nosFbNode* inNode) : NodeContext(inNode)
    {
        u32 arraySize = 0;

        for (auto pin : *inNode->pins())
        {
			if (pin->name()->string_view() == NSN_Output)
            {
                if(flatbuffers::IsFieldPresent(pin, fb::Pin::VT_DATA))
                    arraySize = ((flatbuffers::Vector<u8>*)(pin->data()->Data()))->size();
				IsOrphan = pin->orphan_state() ? pin->orphan_state()->is_orphan() : false;
            }
            else if(pin->name()->string_view() == NSN_Input)
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
        if(Type)
		{
			ArraySize = arraySize;
			ManageOrphan();
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
			ManageOrphan();
		}
	}

    nosResult ExecuteNode(nosNodeExecuteParams* params) override
    {
		if (!Type)
			return NOS_RESULT_FAILED;

		auto pins = NodeExecuteParams(params);
		auto vec = (flatbuffers::Vector<u8>*)(pins[NSN_Input].Data->Data);
		Index = *(u32*)pins[NSN_Index].Data->Data;
		ArraySize = vec->size();
		if (Index < ArraySize)
		{
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
				nosEngine.SetPinValueDirect(ID, buf);
			}
		}
		return NOS_RESULT_SUCCESS;
    }

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
    {
        if (pinName == NSN_Index)
        {
			Index = *(u32*)value.Data;
			ManageOrphan();
		}
        else if (pinName == NSN_Input)
        {
			if (!Type)
				return;
            ArraySize = ((flatbuffers::Vector<u8>*)(value.Data))->size();
			ManageOrphan();
		}
	}

    void ManageOrphan()
    {
		static fb::TOrphanState indexBigger{.is_orphan = true, .message = "Index is bigger than array size"};
		static fb::TOrphanState nonOrphan{.is_orphan = false};
		bool shouldOrphan = Index >= ArraySize;
        auto out = GetPin(NSN_Output);
		if (shouldOrphan == IsOrphan)
			return;
		IsOrphan = shouldOrphan;
        flatbuffers::FlatBufferBuilder fbb;
        std::vector<::flatbuffers::Offset<PartialPinUpdate>> updates = {
            CreatePartialPinUpdateDirect(fbb, &out->Id, 0, nos::fb::OrphanState::Pack(fbb, shouldOrphan ? &indexBigger : &nonOrphan))
        };
        HandleEvent(CreateAppEvent(fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &updates)));
    }
};

nosResult RegisterIndexer(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Indexer, Indexer, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos