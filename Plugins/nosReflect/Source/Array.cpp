// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

namespace nos::reflect
{
struct ArrayNode : NodeContext
{
	std::optional<nos::TypeInfo> Type = std::nullopt;
	bool invalidNode = false;
	static constexpr char InputElementPrefix[] = "Input ";
	ObjectRef ArrayObject{};
	nosResult OnCreate(nosFbNodePtr inNode) override
	{
		for (auto& pin : Pins | std::views::values)
		{
			if (pin.ShowAs == fb::ShowAs::INPUT_PIN)
			{
				if (pin.TypeName != NSN_TypeNameGeneric)
					Type = nos::TypeInfo(pin.TypeName);
			}
			if (pin.ShowAs == fb::ShowAs::OUTPUT_PIN)
			{
				nos::TypeInfo arrayType = nos::TypeInfo(pin.TypeName);
				if (arrayType.TypeName == NSN_TypeNameGeneric)
					continue;
				if (arrayType->BaseType != NOS_BASE_TYPE_ARRAY) {
					invalidNode = true;
					pin.IsOrphan = true;
					pin.ShowAs = nos::fb::ShowAs::PROPERTY;
					continue;
				}
				auto elementTypeName = arrayType->ElementType->TypeName;
				Type = nos::TypeInfo(elementTypeName);
			}
		}
		LoadPins();
		return NOS_RESULT_SUCCESS;
	}

	void OnPinObjectChanged(nos::Name pinName, uuid const& pinId, nosObjectId newHandle) override
	{
		if (pinName == NSN_Output)
			ArrayObject = newHandle;
	}

	void OnNodeUpdated(const nosNodeUpdate* update) override
	{
		//if (update->Type == NOS_NODE_UPDATE_PIN_DELETED || update->Type == NOS_NODE_UPDATE_PIN_CREATED)
		//	UpdateOutputVectorSize();
	}

	size_t GetInputElementIndexFromName(nos::Name name) {
		std::string nameStr = name.AsString();
		if (nameStr.find(InputElementPrefix) == std::string::npos)
			return std::numeric_limits<size_t>::max();
		try {
			// Remove "Input "
			return std::stoull(nameStr.substr(sizeof(InputElementPrefix) - 1));
		}
		catch (const std::exception&) {
			nosEngine.LogE("Input pin's name is not in the 'Input <index>' format: %s", nameStr.c_str());
			return std::numeric_limits<size_t>::max();
		}
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		auto setResolvedTypeNames = [params](nosName elementName) {
			for (size_t i = 0; i < params->PinCount; i++)
			{
				auto& pinInfo = params->Pins[i];
				std::string pinName = nosEngine.GetString(pinInfo.Name);
				if (pinName.find("Input") != std::string::npos)
					pinInfo.OutResolvedTypeName = elementName;
				if (pinName == "Output")
					pinInfo.OutResolvedTypeName = nos::Name('[' + nos::Name(elementName).AsString() + ']');
			}
		};
		nos::TypeInfo info(params->IncomingTypeName);
		if (params->InstigatorPinName == NSN_Output)
		{
			if (info->BaseType != NOS_BASE_TYPE_ARRAY)
			{
				strcpy(params->OutErrorMessage, "Output pin must be an array type");
				return NOS_RESULT_FAILED;
			}
			nos::Name elementName = info->ElementType->TypeName;
			setResolvedTypeNames(elementName);
		}

		for (auto& pin : GetInputs())
		{
			if (pin->ShowAs == nos::fb::ShowAs::INPUT_PIN && pin->Id == params->InstigatorPinId)
			{
				if (info->BaseType == NOS_BASE_TYPE_ARRAY)
				{
					strcpy(params->OutErrorMessage, "Input pin must not be an array type");
					return NOS_RESULT_FAILED;
				}
				setResolvedTypeNames(params->IncomingTypeName);
				break;
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	void OnPinUpdated(nosPinUpdate const* update) override
	{
		if (Type || update->UpdatedField != NOS_PIN_FIELD_TYPE_NAME)
			return;
		
		auto newTypeName = Name(update->TypeName);
		auto typeInfo = nos::TypeInfo(newTypeName);
		if (typeInfo->BaseType == NOS_BASE_TYPE_ARRAY)
		{
			newTypeName = typeInfo->ElementType->TypeName;
		}
		Type = nos::TypeInfo(newTypeName);
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

	void LoadPins()
	{
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<PartialPinUpdate>> updates;

		for (auto& [id, p] : Pins)
			if (p.IsOrphan)
				updates.push_back(CreatePartialPinUpdate(fbb, &p.Id, 0, fb::CreatePinOrphanStateDirect(fbb, invalidNode ? nos::fb::PinOrphanStateType::ORPHAN : nos::fb::PinOrphanStateType::ACTIVE), 0, 0, nos::Action::NOP, 0, p.ShowAs));

		if (!updates.empty())
		{
			HandleEvent(CreateAppEvent(
				fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &updates)));
		}
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		if (!Type)
			return NOS_RESULT_FAILED;

		auto& rawParams = *params.RawParams;
		std::vector<nosObjectId> inputObjects;
		inputObjects.reserve(rawParams.PinCount);
		nosName outputTypeName{};
		for (size_t pinIndex = 0; pinIndex < rawParams.PinCount; pinIndex++)
		{
			auto& pin = rawParams.Pins[pinIndex];
			if (pin->Name == NSN_Output)
			{
				outputTypeName = pin->TypeName;
				continue;
			}
			// Only the element pins contribute to the array. Match them by name: an element pin can be shown as a
			// property rather than an input pin, and it is still executed, so ShowAs must not be used to identify one.
			if (GetInputElementIndexFromName(pin->Name) == std::numeric_limits<size_t>::max())
				continue;
			if (!pin->Object)
			{
				// Skipping would shift every later element, and the remove requests index the array by pin name.
				nosEngine.LogE("Array: Input pin %s has no object", nos::Name(pin->Name).AsCStr());
				return NOS_RESULT_FAILED;
			}
			inputObjects.push_back(*pin->Object);
		}

		ObjectRef newArrayObject{};
		auto res = nosEngine.ObjectAPI->CreateArrayObject(
			outputTypeName, inputObjects.data(), inputObjects.size(), &newArrayObject.GetStorage());
		if (res != NOS_RESULT_SUCCESS || !newArrayObject.IsValid())
			return NOS_RESULT_FAILED;
		ArrayObject = std::move(newArrayObject);
		SetPinObject(NSN_Output, ArrayObject);
		return NOS_RESULT_SUCCESS;
	}

	void OnMenuRequested(nosContextMenuRequestPtr request) override
	{
		auto inputs = GetInputs();

		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<nos::ContextMenuItem>> fields;
		std::string add = "Add Input " + std::to_string(inputs.size());
		fields.push_back(nos::CreateContextMenuItemDirect(fbb, add.c_str(), 1));
		if (inputs.size() > 0)
		{
			std::string remove = "Remove ";
			if (auto pin = GetPin(*request->item_id()))
				remove += pin->DisplayName.AsString();
			else
				remove += "Last Input";

			fields.push_back(nos::CreateContextMenuItemDirect(fbb, remove.c_str(), 2));
		}
		HandleEvent(CreateAppEvent(
			fbb,
			nos::app::CreateAppContextMenuUpdateDirect(fbb, request->item_id(), request->pos(), request->instigator(), &fields)));
	}

	void SendAddElementRequest() {
		auto inputs = GetInputs();
		flatbuffers::FlatBufferBuilder fbb;

		std::vector<uint8_t> data;
		nos::Name typeName = Type ? Name(Type->TypeName) : NSN_TypeNameGeneric;
		if (auto buf = GetDefaultValueOfType(typeName))
			data = std::vector<uint8_t>{(uint8_t*)buf->Data(), (uint8_t*)buf->Data() + buf->Size()};

		auto outputType = "[" + typeName.AsString() + "]";
		auto name = "Input " + std::to_string(inputs.size());
		uuid id = nosEngine.GenerateID();

		std::vector pins = {
			nos::fb::CreatePinDirect(fbb,
									 &id,
									 name.c_str(),
									 typeName.AsCStr(),
									 nos::fb::ShowAs::INPUT_PIN,
									 nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY,
									 0,
									 &data),
		};
		HandleEvent(
			CreateAppEvent(fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, 0, &pins)));

		if (!Type)
			return;

		// TODO: Transfer: Helpers.
		ObjectRef newElement{};
		auto res = nosEngine.ObjectAPI->Construct(typeName, {.Data = data.data(), .Size = data.size()}, &newElement.GetStorage());
		if (res != NOS_RESULT_SUCCESS || !newElement.IsValid())
		{
			nosEngine.LogE("Failed to construct new element of type %s", typeName.AsString().c_str());
			return;
		}
		nosArrayObjectDelta delta{
			.Type = NOS_ARRAY_OBJECT_DELTA_TYPE_APPEND,
			.Append = {
				.Element = newElement
			}
		};
		ApplyArrayDelta(delta);
	}

	void SendRemoveElementRequest(std::optional<size_t> elementIndex = std::nullopt) {
		auto inputs = GetInputs();
		if (inputs.size() == 0) {
			nosEngine.LogE("You can't remove element from an empty array");
			return;
		}
		flatbuffers::FlatBufferBuilder fbb;

		if (!elementIndex)
			elementIndex = inputs.size() - 1;

		if (*elementIndex >= inputs.size())
		{
			nosEngine.LogE("Array: No element at index %zu to remove", *elementIndex);
			return;
		}

		std::vector<fb::UUID> id = { inputs[*elementIndex]->Id};
		HandleEvent(
			CreateAppEvent(fbb, CreatePartialNodeUpdateDirect(fbb, &NodeId, ClearFlags::NONE, &id)));

		nosArrayObjectDelta delta{
			.Type = NOS_ARRAY_OBJECT_DELTA_TYPE_REMOVE_ELEMENT,
			.Remove = {
				.Index = *elementIndex
			}
		};
		ApplyArrayDelta(delta);
	}

	// Publishes an edited copy of the output array. Writing into ArrayObject's own storage would leak the reference it
	// already holds, so the copy goes to a fresh ref and is moved in.
	void ApplyArrayDelta(nosArrayObjectDelta const& delta)
	{
		if (!ArrayObject.IsValid())
			return; // Nothing published yet, the next execution rebuilds the array from the pins.
		ObjectRef edited{};
		auto res = nosEngine.ObjectAPI->CopyArrayObjectWithEdits(ArrayObject, &delta, 1, &edited.GetStorage());
		if (res != NOS_RESULT_SUCCESS || !edited.IsValid())
		{
			nosEngine.LogE("Array: Failed to apply edit to the output array");
			return;
		}
		ArrayObject = std::move(edited);
		SetPinObject(NSN_Output, ArrayObject);
	}

	void OnMenuCommand(uuid const& itemId, uint32_t cmd) override
	{
		switch (cmd)
		{
		case 1: // Add Field
		{
			SendAddElementRequest();
		}
		break;
		case 2: // Remove Field
		{
			std::optional<size_t> elementIndx = std::nullopt;
			if (itemId != NodeId) {
				auto* pin = GetPin(itemId);
				if (!pin)
					return;
				if (auto pinName = pin->DisplayName; pinName != NSN_Output)
					elementIndx = GetInputElementIndexFromName(pinName);
			}
			SendRemoveElementRequest(elementIndx);
		}
		break;
		}
	}

	static nosResult GetFunctions(size_t* outCount, nosName* outFunctionNames, nosPfnNodeFunctionExecute* outFunctions) {
		*outCount = 2;
		if (!outFunctionNames || !outFunctions)
			return NOS_RESULT_SUCCESS;

		outFunctionNames[0] = NOS_NAME_STATIC("Add Element");
		outFunctions[0] = [](void* ctx, nosFunctionExecuteParams* params)
			{
				auto nodeCtx = (ArrayNode*)ctx;
				nodeCtx->SendAddElementRequest();
				return NOS_RESULT_SUCCESS;
			};

		outFunctionNames[1] = NOS_NAME_STATIC("Remove Element");
		outFunctions[1] = [](void* ctx, nosFunctionExecuteParams* params)
			{
				auto nodeCtx = (ArrayNode*)ctx;
				nodeCtx->SendRemoveElementRequest();
				return NOS_RESULT_SUCCESS;
			};

		return NOS_RESULT_SUCCESS;
	}
};

nosResult RegisterArray(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Array, ArrayNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::engine