// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

namespace nos::reflect
{
NOS_REGISTER_NAME(Array)

struct ArrayNode : NodeContext
{
	std::optional<nos::TypeInfo> Type = std::nullopt;
	size_t PinCount = 0;
	ArrayNode(const nosFbNode* inNode) : NodeContext(inNode)
	{
		for (auto& pin : Pins | std::views::values)
		{
			if (pin.ShowAs == fb::ShowAs::INPUT_PIN)
			{
				PinCount++;
			}
			if (pin.ShowAs == fb::ShowAs::OUTPUT_PIN)
			{
				nos::TypeInfo arrayType = nos::TypeInfo(pin.TypeName);
				auto elementTypeName = arrayType->ElementType->TypeName;
				Type = nos::TypeInfo(elementTypeName);
				LoadPins();
			}
		}
	}

	void OnNodeUpdated(const nosFbNode* inNode) override
	{
		auto oldPinCount = PinCount;
		PinCount = GetInputs().size();
		if (oldPinCount == PinCount)
			return;
		std::vector<const void*> values;
		for (auto* pin : *inNode->pins())
			if (pin->show_as() == fb::ShowAs::INPUT_PIN)
				values.push_back((void*)pin->data()->data());
		SetOutput(values);
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		nosTypeInfo* info = nullptr;
		nosEngine.GetTypeInfo(params->IncomingTypeName, &info);
		if (info->BaseType == NOS_BASE_TYPE_ARRAY)
		{
			strcpy(params->OutErrorMessage, "Input pin must not be an array type");
			return NOS_RESULT_FAILED;
		}
		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(nosPinUpdate const* update) override
	{
		if (Type || update->UpdatedField != NOS_PIN_FIELD_TYPE_NAME)
			return;
		auto newTypeName = Name(update->TypeName);
		Type = nos::TypeInfo(newTypeName);
		CreateOutput();
	}

	std::vector<const NodePin*> GetInputs()
	{
		std::vector<const NodePin*> inputs;
		size_t i = 0;
		while (true)
		{
			auto pin = GetPin(nos::Name("Input " + std::to_string(i)));
			if (!pin)
				break;
			inputs.push_back(pin);
			i++;
		}
		return inputs;
	}

	bool SetOutput(std::vector<const void*> const& values)
	{
		auto outPin = GetPin(NSN_Output);
		if (!outPin)
			return false;
		
		auto outval = GenerateVector(*Type, values);

		auto vec = (flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*)(outval.data());
		assert(GetInputs().size() == vec->size());
		nosEngine.SetPinValue(outPin->Id, {outval.data(), outval.size()});
		return true;
	}

	void LoadPins()
	{
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<PartialPinUpdate>> updates;

		for (auto& [id, p] : Pins)
			if (p.IsOrphan)
				updates.push_back(CreatePartialPinUpdate(fbb, &p.Id, 0, fb::CreateOrphanState(fbb, false)));

		if (!updates.empty())
		{
			HandleEvent(CreateAppEvent(
				fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &updates)));
		}
	}

	void CreateOutput()
	{
		if (!Type)
			return;

		auto typeName = Name(Type->TypeName).AsString();
		auto outputType = "[" + typeName + "]";

		nosBuffer value;
		std::vector<u8> data;
		std::vector<u8> outData;

		if (NOS_RESULT_SUCCESS == nosEngine.GetDefaultValueOfType(Type->TypeName, &value))
		{
			data = std::vector<u8>{(u8*)value.Data, (u8*)value.Data + value.Size};
			outData = GenerateVector(*Type, {data.data()});
		}

		flatbuffers::FlatBufferBuilder fbb;

		nosUUID id = nosEngine.GenerateID();

		std::vector<::flatbuffers::Offset<nos::fb::Pin>> pins = {
			fb::CreatePinDirect(fbb,
								&id,
								"Output",
								outputType.c_str(),
								fb::ShowAs::OUTPUT_PIN,
								fb::CanShowAs::OUTPUT_PIN_ONLY,
								0,
								0,
								&outData),
		};

		HandleEvent(CreateAppEvent(
			fbb, nos::CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, 0, &pins)));
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		if (!Type)
			return NOS_RESULT_FAILED;
		std::vector<const void*> values;
		for (size_t i = 0; i < params->PinCount; ++i)
		{
			if (params->Pins[i].Name == NSN_Output)
				continue;
			values.push_back(params->Pins[i].Data->Data);
		}
		return SetOutput(values) ? NOS_RESULT_SUCCESS : NOS_RESULT_FAILED;
	}

	void OnMenuRequested(const nosContextMenuRequest* request) override
	{
		auto inputs = GetInputs();

		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<nos::ContextMenuItem>> fields;
		std::string add = "Add Input " + std::to_string(inputs.size());
		fields.push_back(nos::CreateContextMenuItemDirect(fbb, add.c_str(), 1));
		if (inputs.size() > 1)
		{
			std::string remove = "Remove Input " + std::to_string(inputs.size() - 1);
			fields.push_back(nos::CreateContextMenuItemDirect(fbb, remove.c_str(), 2));
		}
		HandleEvent(CreateAppEvent(
			fbb,
			nos::app::CreateAppContextMenuUpdateDirect(fbb, &NodeId, request->pos(), request->instigator(), &fields)));
	}

	void OnMenuCommand(nosUUID itemID, uint32_t cmd) override
	{
		auto inputs = GetInputs();
		flatbuffers::FlatBufferBuilder fbb;
		switch (cmd)
		{
		case 1: // Add Field
		{
			nosBuffer value;
			std::vector<u8> data;
			nos::Name typeName = Type ? Name(Type->TypeName) : NSN_VOID;
			if (NOS_RESULT_SUCCESS == nosEngine.GetDefaultValueOfType(typeName, &value))
				data = std::vector<u8>{(u8*)value.Data, (u8*)value.Data + value.Size};

			auto outputType = "[" + typeName.AsString() + "]";
			auto name = "Input " + std::to_string(inputs.size());
			nosUUID id = nosEngine.GenerateID();

			std::vector pins = {
				nos::fb::CreatePinDirect(fbb,
										 &id,
										 name.c_str(),
										 typeName.AsCStr(),
										 nos::fb::ShowAs::INPUT_PIN,
										 nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY,
										 0,
										 0,
										 &data),
			};
			HandleEvent(
				CreateAppEvent(fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, 0, &pins)));
		}
		break;
		case 2: // Remove Field
		{
			std::vector<fb::UUID> id = {inputs.back()->Id};
			HandleEvent(
				CreateAppEvent(fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, &id)));
		}
		break;
		}
	}
};

nosResult RegisterArray(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Array, ArrayNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::engine