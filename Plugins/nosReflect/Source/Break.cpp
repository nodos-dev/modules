// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

namespace nos::reflect
{
NOS_REGISTER_NAME(Break)

struct BreakNode : NodeContext
{
	std::optional<nos::TypeInfo> Type = std::nullopt;
	size_t ArraySize = 0;

	BreakNode(const nosFbNode* node) : NodeContext(node)
	{
		for (auto* pin : *node->pins())
		{
			if (pin->show_as() != fb::ShowAs::INPUT_PIN)
				continue;
            auto typeName = nos::Name(pin->type_name()->string_view());
			if (NSN_VOID == typeName)
				break;
			Type = nos::TypeInfo(typeName);
			LoadPins(false);
            break;
        }
	}

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{ 
		if (!Type || (*Type)->BaseType != NOS_BASE_TYPE_ARRAY || pinName != NSN_Input)
			return;
		auto pin = GetPin(pinName);
		flatbuffers::Vector<u8>* vec = (flatbuffers::Vector<u8>*)value.Data;
		if (vec->size() != ArraySize)
		{
			ArraySize = vec->size();
			SetOutputCount();
		}
	}

    void OnPinUpdated(const nosPinUpdate* update) override
	{
		if (Type)
			return;
		if (update->UpdatedField == NOS_PIN_FIELD_TYPE_NAME)
		{
			if (update->PinName != NSN_Input)
				return;
			Type = nos::TypeInfo(update->TypeName);
			LoadPins(true);
		}
	}

    nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		auto info = nos::TypeInfo(params->IncomingTypeName);
		switch (info->BaseType)
		{
		case NOS_BASE_TYPE_ARRAY:
		case NOS_BASE_TYPE_STRUCT: return NOS_RESULT_SUCCESS;
		default: return NOS_RESULT_FAILED;
		}
	};

    void LoadPins(bool setDisplayName)
    {
        flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<PartialPinUpdate>> pinsToUpdate;
		std::vector<::flatbuffers::Offset<nos::fb::Pin>> pinsToCreate;
        std::vector<fb::UUID> pinsToDelete;

		std::unordered_set<nos::Name> acceptedPins;
	
		if (auto* in = GetPin(NSN_Input))
		{
			pinsToUpdate.push_back(CreatePartialPinUpdate(fbb, &in->Id, 0, fb::CreateOrphanState(fbb, false)));
			acceptedPins.insert(in->Name);
		}
		auto& type = *Type;
        if (type->BaseType == NOS_BASE_TYPE_STRUCT)
		{
			for (int i = 0; i < type->FieldCount; ++i)
			{
				auto& field = type->Fields[i];
				acceptedPins.insert(field.Name);
				if (auto pin = GetPin(field.Name))
				{
					if (!pin->IsOrphan && pin->TypeName == field.Type->TypeName)
						continue;
					pinsToUpdate.push_back(CreatePartialPinUpdateDirect(fbb,
																		&pin->Id,
																		0,
																		nos::fb::CreateOrphanStateDirect(fbb, false),
																		nos::Name(field.Type->TypeName).AsCStr()));
				}
				else
				{
					nosUUID newPinId = nosEngine.GenerateID();

					std::vector<uint8_t> data =
						std::vector((const uint8_t*)field.DefaultValue.Data,
									(const uint8_t*)field.DefaultValue.Data + field.DefaultValue.Size);

					pinsToCreate.push_back(fb::CreatePinDirect(fbb,
															   &newPinId,
															   nos::Name(field.Name).AsCStr(),
															   nos::Name(field.Type->TypeName).AsCStr(),
															   fb::ShowAs::OUTPUT_PIN,
															   fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY,
															   0,
															   0,
															   &data,
															   0,
															   0,
															   0,
															   &data));
				}
			}
		}
        else if (type->BaseType == NOS_BASE_TYPE_ARRAY)
        {
        	size_t i = 0;
        	while (auto pin = GetPin(nos::Name("Output " + std::to_string(i))))
        	{
        		acceptedPins.insert(pin->Name);
        		i++;
        		if (pin->IsOrphan)
        		{
        			pinsToUpdate.push_back(CreatePartialPinUpdate(fbb, &pin->Id, 0, fb::CreateOrphanState(fbb, false)));
        		}
        	}
        	ArraySize = i;
        }
		auto typeName = "Break " + nos::Name(Type->TypeName).AsString();
		for (auto& [id, pin] : Pins)
			if (!acceptedPins.contains(pin.Name))
				pinsToDelete.push_back(id);

        HandleEvent(CreateAppEvent(
			fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, &pinsToDelete, &pinsToCreate, 0, 0, 0, 0, 0, &pinsToUpdate, 0, 0, 0, setDisplayName ? typeName.c_str() : 0)));
    }

	void SetOutputCount()
    { 
    	size_t i = 0;
    	std::vector<fb::UUID> pinsToDelete;
    	std::vector<::flatbuffers::Offset<nos::fb::Pin>> pinsToCreate;
    	while (true)
    	{
    		auto inputName = nos::Name("Output " + std::to_string(i));
    		auto input = GetPin(inputName);
    		if (!input)
    			break;
    		if (i >= ArraySize)
    			pinsToDelete.push_back(input->Id);
    		i++;
    	}
    	flatbuffers::FlatBufferBuilder fbb;
    	if (ArraySize > i)
    	{
    		for (size_t a = 0; a < ArraySize - i; a++)
    		{
    			nosUUID newPinId = nosEngine.GenerateID();
    			std::vector<u8> vec{};
    			nosBuffer defVal{};
				auto& type = *Type;
    			if (nosEngine.GetDefaultValueOfType(type->ElementType->TypeName, &defVal) == NOS_RESULT_SUCCESS)
    			{
    				vec = std::vector((const u8*)defVal.Data, (const u8*)defVal.Data + defVal.Size);
    			}

    			std::vector<uint8_t> data = std::vector<uint8_t>(type->ByteSize);
    			pinsToCreate.push_back(fb::CreatePinDirect(fbb,
															&newPinId,
															nos::Name("Output " + std::to_string(i + a)).AsCStr(),
															nos::Name(type->ElementType->TypeName).AsCStr(),
															fb::ShowAs::OUTPUT_PIN,
															fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY,
															0,
															0,
															&data,
															0,
															0,
															0,
															&data));
    		}
    	}
    	if (!pinsToDelete.empty() || !pinsToCreate.empty())
    		HandleEvent(CreateAppEvent(
				fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, &pinsToDelete, &pinsToCreate)));
    }


	std::unordered_map<nosUUID, nos::Buffer> LastServedPinValues;

	void SetPinValueCached(const nosUUID& pinId, nosBuffer value)
	{
		auto it = LastServedPinValues.find(pinId);
		if (it != LastServedPinValues.end() && it->second == value)
			return;
        nosEngine.SetPinValueDirect(pinId, value);
		LastServedPinValues[pinId] = value;
	}
	
    void SetOutputValues(const nosBuffer* buf)
    {
        if(!buf)
			return;

		auto data = (const u8*)buf->Data;
        auto& type = *Type;
        switch (type->BaseType)
        {
        case NOS_BASE_TYPE_ARRAY: {
        	const flatbuffers::Vector<u8>* vec = (flatbuffers::Vector<u8>*)(data);
        	auto tableVec = (const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*)vec;
        	for (int i = 0; i < vec->size(); ++i)
        	{
        		auto pinId = GetPinId(nos::Name("Output " + std::to_string(i)));
        		if (!pinId)
        			continue;
        		if (type->ElementType->ByteSize)
        		{
        			auto data = vec->data() + i * type->ElementType->ByteSize;
        			SetPinValueCached(*pinId, {(void*)data, type->ElementType->ByteSize});
        		}
        		else
        		{
        			// TODO: Strings
        			flatbuffers::FlatBufferBuilder fbb;
        			fbb.Finish(
						flatbuffers::Offset<flatbuffers::Table>(CopyTable(fbb, type->ElementType, tableVec->Get(i))));
        			nos::Buffer buf = fbb.Release();
        			SetPinValueCached(*pinId, {(void*)buf.Data(), buf.Size()});
        		}
        	}
        	break;
        }
        case NOS_BASE_TYPE_STRUCT:
        {
            auto root = type->ByteSize ? (flatbuffers::Table*)data : flatbuffers::GetRoot<flatbuffers::Table>(data);
            for (int i = 0; i < type->FieldCount; ++i)
            {
				auto& field = type->Fields[i];
				auto pin = GetPin(field.Name);
				if (!pin)
					continue;
				if (!type->ByteSize && !root->CheckField(field.Offset)) 
				{
					if (field.DefaultValue.Size)
						SetPinValueCached(pin->Id, field.DefaultValue);
					continue;
				}
				
				if (field.Type->ByteSize)
				{
					auto data = !type->ByteSize ? root->GetStruct<u8*>(field.Offset) : ((u8*)root + field.Offset);
					SetPinValueCached(pin->Id, { (void*)data, field.Type->ByteSize });
				}
                else
                {
                	nos::Buffer buf;
                    if (field.Type->BaseType == NOS_BASE_TYPE_STRING)
                    {
	                    auto str = root->GetPointer<const ::flatbuffers::String *>(field.Offset)->string_view();
						buf = Buffer(str.data(), str.size() + 1);
                    }
                	else
                	{
						flatbuffers::FlatBufferBuilder fbb;
                		fbb.Finish(flatbuffers::Offset<flatbuffers::Table>(CopyTable(fbb, field.Type, root->GetPointer<flatbuffers::Table*>(field.Offset))));
						buf = fbb.Release();
                	}
					SetPinValueCached(pin->Id, buf);
                }
            }
        }
        }
    }

    nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		if(!Type)
			return NOS_RESULT_SUCCESS;
		auto pins = NodeExecuteParams(params);
		SetOutputValues(pins[NSN_Input].Data);
		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterBreak(nosNodeFunctions* fn)
{ 
    NOS_BIND_NODE_CLASS(NSN_Break, BreakNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos