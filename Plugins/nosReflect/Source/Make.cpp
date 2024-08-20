// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

// Nodos SDK
#include <PluginConfig_generated.h>

namespace nos::reflect
{
NOS_REGISTER_NAME(Value)
NOS_REGISTER_NAME(Make)
NOS_REGISTER_NAME(MakeDynamic)

struct MakeNode : NodeContext
{
    std::optional<nos::TypeInfo> Type = {};
    nos::Name VisualizerName = {};

    MakeNode(const fb::Node* node) : NodeContext(node)
    {
        if (flatbuffers::IsFieldPresent(node, fb::Node::VT_TEMPLATE_PARAMETERS) && 1 == node->template_parameters()->size())
        {
            auto p = node->template_parameters()->Get(0);
			nos::Name typeName = nos::Name((const char*)p->value()->Data());
			Type = nos::TypeInfo(typeName);
			std::optional<std::string> updateDisplayName = std::nullopt;
			
            if(flatbuffers::IsFieldPresent(node, fb::Node::VT_DISPLAY_NAME) && node->display_name()->str().empty())
                updateDisplayName = "Make " + nos::Name(Type->TypeName).AsString();
            LoadPins(updateDisplayName ? updateDisplayName->c_str() : nullptr);
        }
    }

    void OnPinConnected(nos::Name pinName, nosUUID connectedPin) override
    {
        if (pinName == NSN_Value && Type)
        {
			if (Type->TypeName == NOS_NAME_STATIC("string"))
            {
				nosName visualizerName{};
				nosEngine.GetPinVisualizerName(connectedPin, &visualizerName);
				UpdateVisualizer(visualizerName);
			}
		}
    }

    nosResult ExecuteNode(nosNodeExecuteParams* params) override
    {
		if (!Type)
			return NOS_RESULT_SUCCESS;

		flatbuffers::FlatBufferBuilder fbb;
		NodeExecuteParams pins(params);
		auto& type = *Type;
		switch (type->BaseType)
		{
		case NOS_BASE_TYPE_FLOAT:
		case NOS_BASE_TYPE_INT:
		case NOS_BASE_TYPE_UINT:
		case NOS_BASE_TYPE_STRING:
		case NOS_BASE_TYPE_UNION:
			nosEngine.SetPinValue(pins[NSN_Output].Id, *pins[NSN_Value].Data);
			return NOS_RESULT_SUCCESS;
		}

		flatbuffers::uoffset_t offset = CopyArgs(fbb, type, pins);
		fbb.Finish(flatbuffers::Offset<flatbuffers::Vector<uint8_t>>(offset));
		nos::Buffer buf = fbb.Release();

		auto root = flatbuffers::GetRoot<flatbuffers::Table>(buf.Data());

		SetPinValue(NSN_Output, type->ByteSize ? nosBuffer{(void*)root, type->ByteSize}
											   : nosBuffer{(void*)(buf.Data()), buf.Size()});
		return NOS_RESULT_SUCCESS;
    }

	std::vector<nosName> AllTypeNames;
    
    void OnMenuRequested(const nosContextMenuRequest* request) override
    {
        if(Type) 
            return;
		flatbuffers::FlatBufferBuilder fbb;
    	size_t count = 0;
		auto res = nosEngine.GetPinDataTypeNames(nullptr, &count);
    	if (NOS_RESULT_FAILED == res)
    		return;
		AllTypeNames.resize(count);
    	res = nosEngine.GetPinDataTypeNames(AllTypeNames.data(), &count);
    	if (NOS_RESULT_FAILED == res)
    		return;
    	std::vector<flatbuffers::Offset<nos::ContextMenuItem>> types;
    	u32 index = 0;
    	for (auto ty : AllTypeNames)
    		types.push_back(nos::CreateContextMenuItemDirect(fbb, nos::Name(ty).AsCStr(), index++));
    	HandleEvent(CreateAppEvent(fbb, app::CreateAppContextMenuUpdateDirect(fbb, &NodeId, request->pos(), request->instigator(), &types)));
    }

    void OnMenuCommand(nosUUID itemID, uint32_t cmd) override
    {
		if(Type) 
			return;
    	if (cmd >= AllTypeNames.size())
			return;
    	auto tyName = AllTypeNames[cmd];
    	auto typeInfo = nos::TypeInfo(tyName);
    	SetType(typeInfo);
	}

    // Set the template parameter, update pin type
    void SetType(nosTypeInfo const* typeInfo)
    {
	    // Set template parameter
        flatbuffers::FlatBufferBuilder fbb;
        
        std::vector<u8> data = nos::Buffer(nos::Name(typeInfo->TypeName).AsCStr(), 1 + nos::Name(typeInfo->TypeName).AsString().size());
        std::vector <flatbuffers::Offset<fb::TemplateParameter>> params = { fb::CreateTemplateParameterDirect(fbb, "string", &data) };
        auto paramsOffset = fbb.CreateVector(params);
		auto typeNameOffset = fbb.CreateString(nos::Name(typeInfo->TypeName).AsCStr());
        
        PinResolveRequest(NSN_Output, typeInfo->TypeName);
        PartialNodeUpdateBuilder update(fbb);
        update.add_node_id(&NodeId);
        update.add_template_parameters(paramsOffset);
        HandleEvent(CreateAppEvent(fbb, update.Finish()));
    }

    void OnPinUpdated(const nosPinUpdate* update) override
    {
		if (Type)
			return;
        if (update->UpdatedField == NOS_PIN_FIELD_TYPE_NAME)
        {
			if (update->PinName != NSN_Output)
				return;
			Type = nos::TypeInfo(update->TypeName);
            LoadPins();
		}
	}

    nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
    { 
        nos::TypeInfo incomingType(params->IncomingTypeName);
        if (incomingType->BaseType == NOS_BASE_TYPE_NONE)
        {
            strcpy(params->OutErrorMessage, "Type not supported for make.");
            return NOS_RESULT_FAILED;
        }
		return NOS_RESULT_SUCCESS;
    }

    void LoadPins(const char* updatedDisplayName = nullptr)
    {
		assert((*Type)->BaseType != NOS_BASE_TYPE_NONE);
    	nosBuffer defBuf{};
		auto& type = *Type;
    	nosEngine.GetDefaultValueOfType(type->TypeName, &defBuf); // TODO: This can be freed after type is unloaded, so beware.
		if (!defBuf.Data)
			return;
    	auto buf = nos::Buffer(defBuf);
    	std::vector<u8> data = buf;
        flatbuffers::FlatBufferBuilder fbb;
        std::vector<flatbuffers::Offset<nos::fb::Pin>> pinsToAdd = {};
        std::vector<::flatbuffers::Offset<PartialPinUpdate>> pinsToUpdate = {};
        std::vector<fb::UUID> pinsToDelete = {};

        std::set<nosName> pinNames = { NSN_Output };

        if (auto out = GetPin(NSN_Output))
		{
			if (out->TypeName != type->TypeName || out->IsOrphan)
			{
				pinsToUpdate.push_back(CreatePartialPinUpdateDirect(fbb,
																	&out->Id,
																	0,
																	nos::fb::CreateOrphanStateDirect(fbb, false),
																	nos::Name(type->TypeName).AsCStr(),
																	nos::Name(type->TypeName).AsCStr()));
			}
		}
		else
		{
			nosUUID id = nosEngine.GenerateID();
			nos::fb::TPin outPin{};
            outPin.id = id;
            outPin.name = nos::Name(NSN_Output).AsCStr();
            outPin.type_name = nos::Name(Type->TypeName).AsCStr();
            outPin.show_as = nos::fb::ShowAs::OUTPUT_PIN;
            outPin.can_show_as = nos::fb::CanShowAs::OUTPUT_PIN_ONLY;
            outPin.data = data;
            outPin.display_name = nos::Name(Type->TypeName).AsCStr();
			pinsToAdd.push_back(fb::CreatePin(fbb, &outPin));
        }

        // If the type is a primitive, then it will be constructed from a single pin named "Value"
        switch (type->BaseType)
        {
        case NOS_BASE_TYPE_INT:   
        case NOS_BASE_TYPE_UINT:  
        case NOS_BASE_TYPE_FLOAT: 
        case NOS_BASE_TYPE_STRING:
		case NOS_BASE_TYPE_UNION:
			pinNames.insert(NSN_Value);
			if (auto pin = GetPin(NSN_Value))
            {
                if (pin->IsOrphan)
                {
                    pinsToUpdate.push_back(CreatePartialPinUpdateDirect(fbb,
                        &pin->Id,
                        0,
                        nos::fb::CreateOrphanStateDirect(fbb, false),
                        nos::Name(type->TypeName).AsCStr(),
                        nos::Name(NSN_Value).AsCStr()));
                }
                if (type->BaseType == NOS_BASE_TYPE_STRING)
                {
					nosName visName{};
					nosEngine.GetPinVisualizerName(pin->Id, &visName);
                    VisualizerName = visName;
                }
            }
            else
            {
                nosUUID id = nosEngine.GenerateID();
                std::vector<u8> data(type->ByteSize);
                if (type->BaseType == NOS_BASE_TYPE_STRING)
                {
                    data = std::vector<u8>(1, 0);
                }
                pinsToAdd.push_back(fb::CreatePinDirect(fbb, &id, nos::Name(NSN_Value).AsCStr(), nos::Name(type->TypeName).AsCStr(), nos::fb::ShowAs::INPUT_PIN, nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, 0, 0, &data));
            }
            break;
        case NOS_BASE_TYPE_NONE: break;
        case NOS_BASE_TYPE_ARRAY: break;
        case NOS_BASE_TYPE_STRUCT:
        {
			auto rootIftable = type->ByteSize ? nullptr : buf.As<flatbuffers::Table>();
            for (int i = 0; i < type->FieldCount; ++i)
            {
                auto field = type->Fields[i];
                pinNames.insert(field.Name);
                if (auto f = GetPin(field.Name))
                {
                    if (f->TypeName != field.Type->TypeName || f->IsOrphan)
                    {
                        pinsToUpdate.push_back(CreatePartialPinUpdateDirect(fbb,
                            &f->Id,
                            0,
                            nos::fb::CreateOrphanStateDirect(fbb, false),
                            nos::Name(field.Type->TypeName).AsCStr(),
                            nos::Name(field.Name).AsCStr()));
                    }
                }
                else
                {
                    nosUUID id = nosEngine.GenerateID();
					std::vector<u8> data;
					if (type->ByteSize)
					{
						auto* fieldStart = buf.As<u8>() + field.Offset;
						data = std::vector<u8>(fieldStart,
											   fieldStart + field.Type->ByteSize);
                    }
					else
					{
                        data = GenerateBuffer(field.Type, rootIftable->GetStruct<u8*>(field.Offset));
                    }
                    pinsToAdd.push_back(fb::CreatePinDirect(fbb, &id, nos::Name(field.Name).AsCStr(), nos::Name(field.Type->TypeName).AsCStr(), nos::fb::ShowAs::INPUT_PIN, nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY, 0, 0, &data));
                }
            }
        }
            break;
        }

        for (auto& [name, id]: PinName2Id )
            if (!pinNames.contains(name))
                pinsToDelete.push_back(id);

        if (!pinsToAdd.empty() ||
            !pinsToDelete.empty() ||
            !pinsToUpdate.empty())
		{
			std::vector<u8> data =
				nos::Buffer(nos::Name(Type->TypeName).AsCStr(), 1 + nos::Name(Type->TypeName).AsString().size());
			std::vector<flatbuffers::Offset<fb::TemplateParameter>> params = {
				fb::CreateTemplateParameterDirect(fbb, "string", &data)};
			HandleEvent(CreateAppEvent(fbb,
												  CreatePartialNodeUpdateDirect(fbb,
																				&NodeId,
																				ClearFlags::CLEAR_TEMPLATE_PARAMETERS,
																				&pinsToDelete,
																				&pinsToAdd,
																				0,
																				0,
																				0,
																				0,
																				0,
																				&pinsToUpdate,
																				0,
																				0,
																				&params,
																				updatedDisplayName)));
		}
    }

	void UpdateVisualizer(nos::Name newVisualizerName)
	{
		if (Type->TypeName != NOS_NAME_STATIC("string"))
			return;
		if (newVisualizerName == VisualizerName)
			return;
		VisualizerName = newVisualizerName;
		nos::fb::TVisualizer visualizer{};
		if (VisualizerName)
		{
			visualizer.type = nos::fb::VisualizerType::COMBO_BOX;
			visualizer.name = VisualizerName.AsCStr();
		}
		else
		{
			visualizer.type = nos::fb::VisualizerType::NONE;
		}
		SetPinVisualizer(NSN_Value, visualizer);
	}
};

nosResult RegisterMake(nosNodeFunctions* fn)
{
    nosNodeFunctions* make = fn;
    nosNodeFunctions* makeDynamic = fn + 1;
	NOS_BIND_NODE_CLASS(NSN_Make, MakeNode, make);
	NOS_BIND_NODE_CLASS(NSN_MakeDynamic, MakeNode, makeDynamic);
	
	std::vector<nosName> typeNames;
	size_t count = 0;
	auto res = nosEngine.GetPinDataTypeNames(0, &count);
	if (NOS_RESULT_FAILED != res)
	{
		typeNames.resize(count);
		nosEngine.GetPinDataTypeNames(typeNames.data(), &count);
	}
	std::vector<nos::Buffer> nodeInfos;
	for (auto& typeName : typeNames)
	{
		nos::TypeInfo typeInfo(typeName);
		if (typeInfo->BaseType == NOS_BASE_TYPE_NONE)
			continue;
		// If has 'skip_make' attribute
		if (typeInfo->BaseType == NOS_BASE_TYPE_STRUCT || typeInfo->BaseType == NOS_BASE_TYPE_UNION
			|| typeInfo->BaseType == NOS_BASE_TYPE_ARRAY)
		{

			bool skip = true;
			for (int i = 0; i < typeInfo->AttributeCount; ++i)
			{
				if (typeInfo->Attributes[i].Name == NOS_NAME_STATIC("builtin"))
					skip = false;
				else if (typeInfo->Attributes[i].Name == NOS_NAME_STATIC("skip_make"))
				{
					skip = true;
					break;
				}
			}
			if (skip)
				continue;
		}
		std::string name = nos::Name(typeInfo.TypeName).AsString();
		auto idx = name.find_last_of(".");
		idx = idx == std::string::npos ? 0 : 1+idx;
		fb::TNodeInfo info;
		info.category = "Type";
		info.class_name = "nos.reflect.Make";
		info.display_name = "Make " + name.substr(idx);
		std::vector<u8> data(1 + name.size());
		memcpy(data.data(), name.data(), name.size());
		info.params.emplace_back(new fb::TTemplateParameter{ {},"string", std::move(data) });
		flatbuffers::FlatBufferBuilder fbb;
		fbb.Finish(CreateNodeInfo(fbb, &info));
		nos::Buffer buf = fbb.Release();
		nodeInfos.push_back(std::move(buf));
	}
	std::vector<const nosFbNodeInfo*> fbNodeInfos;
	for (auto& buf : nodeInfos)
		fbNodeInfos.push_back(flatbuffers::GetMutableRoot<nosFbNodeInfo>(buf.Data()));
	nosEngine.RegisterNodeInfos(nosEngine.Module->Id, fbNodeInfos.size(), fbNodeInfos.data());
	return NOS_RESULT_SUCCESS;
}

} // namespace nos