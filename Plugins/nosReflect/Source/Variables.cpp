#include <Nodos/Plugin.hpp>

#include <shared_mutex>

#include "Names.h"

#include <nosSysVariables/nosVariableSubsystem.h>

namespace nos::reflect
{
NOS_REGISTER_NAME(SetVariable)
NOS_REGISTER_NAME(GetVariable)
NOS_REGISTER_NAME(Name)

enum class VariableStatusItem
{
	VariableName,
	TypeName,
};

struct VariableNodeBase : NodeContext
{
	nosResult OnCreate(nosFbNodePtr node) override
	{
		TypeName = GetPin(NSN_Value)->TypeName;
		return NOS_RESULT_SUCCESS;
	}

	~VariableNodeBase() override
	{
		if (!HasName())
		{
			return;
		}
		auto res = nosVariables->DeleteNodeReference(Name, NodeId);
		NOS_SOFT_CHECK(res == NOS_RESULT_SUCCESS, "Failed to delete node reference");
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		nos::TypeInfo incomingType(params->IncomingTypeName);
		for (int i = 0; i < incomingType->AttributeCount; ++i)
		{
			if (incomingType->Attributes[i].Name == NOS_NAME_STATIC("resource"))
			{
				strcpy(params->OutErrorMessage, "Resource types are not supported");
				return NOS_RESULT_FAILED;
			}
		}
		return NOS_RESULT_SUCCESS;
	}

	void UpdateStatus()
	{
		std::vector<fb::TNodeStatusMessage> messages;
		for (auto& [type, message] : StatusMessages)
			messages.push_back(message);
		SetNodeStatusMessages(messages);
	}

	void SetStatus(VariableStatusItem item,
				   fb::NodeStatusMessageType msgType,
				   std::string text,
				   std::string details,
				   uint64_t timeout,
				   bool popup)
	{
		StatusMessages[item] = fb::TNodeStatusMessage{{}, std::move(text), msgType, details, timeout, true, popup};
		UpdateStatus();
	}

	void ClearStatus(VariableStatusItem item)
	{
		StatusMessages.erase(item);
		UpdateStatus();
	}

	virtual void OnPinUpdated(const nosPinUpdate* pinUpdate) override
	{
		if (pinUpdate->UpdatedField != NOS_PIN_FIELD_TYPE_NAME)
			return;
		ClearStatus(VariableStatusItem::TypeName);
		if (TypeName != NSN_TypeNameGeneric && TypeName == pinUpdate->TypeName)
			return;
		TypeName = pinUpdate->TypeName;
		SetPinOrphanState(NSN_Value, fb::PinOrphanStateType::ACTIVE);
	}

	bool HasType() const { return TypeName != NSN_TypeNameGeneric; }

	bool HasName() const { return Name.IsValid() && !Name.AsString().empty(); }

	std::unordered_map<VariableStatusItem, fb::TNodeStatusMessage> StatusMessages;
	nos::Name Name;
	nos::Name TypeName = NSN_TypeNameGeneric;
	int32_t CallbackId = -1;
};

struct SetVariableNode : VariableNodeBase
{
	nosResult OnCreate(nosFbNodePtr node) override
	{
		VariableNodeBase::OnCreate(node);
		CheckType();
		// For editor to show changes without a scheduled node, we use pin value change callbacks.
		// Once we support this in the engine, we can move these to ExecuteNode function.
		AddPinValueWatcher<const char*>(
			NOS_NAME("Name"), [this](const char* newValue, std::optional<const char*> oldVal) {
				if (oldVal)
				{
					nos::Name oldName(*oldVal);
					nosVariables->DeleteNodeReference(oldName, NodeId);
					nosVariables->UnregisterVariableUpdateCallback(oldName, CallbackId);
					CallbackId = -1;
				}
				Name = nos::Name(newValue);
				CheckName();
				if (!HasName())
					return;
				CallbackId =
					nosVariables->RegisterVariableUpdateCallback(Name, &SetVariableNode::VariableUpdateCallback, this);
				// Check if already exists
				{
					nosName outTypeName{};
					nosBuffer outValue{};
					auto res = nosVariables->Get(Name, &outTypeName, &outValue);
					if (res == NOS_RESULT_SUCCESS)
					{
						nosVariables->AddNodeReference(Name, NodeId);
						// If type is already set, reset it to the correct type.
						if (HasType())
						{
							SetPinType(NOS_NAME("Value"), outTypeName);
							SetPinValue(NOS_NAME("Value"), outValue);
						}
						else
							TypeName = outTypeName;
						return;
					}
				}
				if (HasType())
				{
					if (Value)
					{
						nosVariables->Set(Name, TypeName, Value->GetInternal());
						nosVariables->AddNodeReference(Name, NodeId);
					}
					else
						SetDefaultValue();
				}
			});
		AddPinValueWatcher(NOS_NAME("Value"), [this](const nos::Buffer& value, std::optional<nos::Buffer> oldValue) {
			if (!HasType())
				return;
			Value = value;
			if (!HasName())
				return;
			nosVariables->Set(Name, TypeName, Value->GetInternal());
			nosVariables->AddNodeReference(Name, NodeId);
		});
		return NOS_RESULT_SUCCESS;
	}

	~SetVariableNode() override
	{
		if (CallbackId != -1 && HasName())
			nosVariables->UnregisterVariableUpdateCallback(Name, CallbackId);
	}

	void OnPinUpdated(const nosPinUpdate* pinUpdate) override
	{
		VariableNodeBase::OnPinUpdated(pinUpdate);
		if (pinUpdate->UpdatedField != NOS_PIN_FIELD_TYPE_NAME)
			return;
		CheckType();
		if (HasName() && HasType())
		{
			if (!Value)
				SetDefaultValue();
			else
			{
				nosVariables->Set(Name, TypeName, Value->GetInternal());
				nosVariables->AddNodeReference(Name, NodeId);
			}
		}
	}

	void SetDefaultValue()
	{
		nosName outTypeName{};
		nosBuffer outValue{};
		auto res = nosVariables->Get(Name, &outTypeName, &outValue);
		if (res == NOS_RESULT_SUCCESS)
			SetPinValue(NOS_NAME("Value"), outValue);
		else if (auto def = GetDefaultValueOfType(TypeName))
			SetPinValue(NOS_NAME("Value"), *def);
	}

	void OnVariableUpdated(nos::Name name, nos::Name typeName, const nosBuffer* value)
	{
		if (!HasType())
			SetPinType(NOS_NAME("Value"), typeName);
	}

	static void VariableUpdateCallback(nosName name, void* userData, nosName typeName, const nosBuffer* value)
	{
		auto* node = static_cast<SetVariableNode*>(userData);
		node->OnVariableUpdated(name, typeName, value);
	}

	void OnNodeMenuRequested(nosContextMenuRequestPtr request) override
	{
		if (HasType())
			return;
		flatbuffers::FlatBufferBuilder fbb;
		size_t count = 0;
		auto res = nosEngine.GetPinDataTypeNames(nullptr, nullptr, &count);
		if (NOS_RESULT_FAILED == res)
			return;
		AllTypeNames.resize(count);
		res = nosEngine.GetPinDataTypeNames(nullptr, AllTypeNames.data(), &count);
		if (NOS_RESULT_FAILED == res)
			return;
		std::vector<flatbuffers::Offset<nos::ContextMenuItem>> types;
		uint32_t index = 0;
		for (auto it = AllTypeNames.begin(); it != AllTypeNames.end();)
		{
			nos::Name typeName(*it);
			nos::TypeInfo info(typeName);
			bool skip = false;
			for (int i = 0; i < info->AttributeCount; ++i)
			{
				if (info->Attributes[i].Name == NOS_NAME("resource"))
				{
					skip = true;
					break;
				}
			}
			if (skip)
				it = AllTypeNames.erase(it);
			else
				++it;
		}
		for (auto ty : AllTypeNames)
			types.push_back(nos::CreateContextMenuItemDirect(fbb, nos::Name(ty).AsCStr(), index++));
		std::vector<flatbuffers::Offset<nos::ContextMenuItem>> items;
		items.push_back(nos::CreateContextMenuItemDirect(fbb, "Set Type", -1, &types));
		HandleEvent(CreateAppEvent(
			fbb, app::CreateAppContextMenuUpdateDirect(fbb, &NodeId, request->pos(), request->instigator(), &items)));
	}

	void OnMenuCommand(uuid const& itemID, uint32_t cmd) override
	{
		if (HasType())
			return;
		if (cmd >= AllTypeNames.size())
			return;
		auto tyName = AllTypeNames[cmd];
		SetPinType(NOS_NAME("Value"), tyName);
	}

	void CheckType()
	{
		if (!HasType())
		{
			SetStatus(VariableStatusItem::TypeName, fb::NodeStatusMessageType::WARNING, "Type not set", "", 5, true);
			SetPinOrphanState(NSN_Value, fb::PinOrphanStateType::PASSIVE, "Data type not set");
		}
		else
		{
			ClearStatus(VariableStatusItem::TypeName);
			SetPinOrphanState(NSN_Value, fb::PinOrphanStateType::ACTIVE);
		}
	}

	void CheckName()
	{
		if (!HasName())
			SetStatus(
				VariableStatusItem::VariableName, fb::NodeStatusMessageType::WARNING, "Provide a name", "", 5, true);
		else
			SetStatus(VariableStatusItem::VariableName, fb::NodeStatusMessageType::INFO, Name.AsString(), "", 2, false);
	}

	std::optional<nos::Buffer> Value;
	std::vector<nosName> AllTypeNames;
};

struct GetVariableNode : VariableNodeBase
{
	nosResult OnCreate(nosFbNodePtr node) override
	{
		VariableNodeBase::OnCreate(node);
		nos::Buffer initialValue;
		for (auto* pin : *node->pins())
		{
			if (pin->name()->string_view() == "Value")
			{
				initialValue = nos::Buffer(pin->data()->data(), pin->data()->size());
				break;
			}
		}
		AddPinValueWatcher(
			NOS_NAME("Name"),
			[this, initialValue = std::move(initialValue)](const nos::Buffer& value,
														   std::optional<nos::Buffer> oldValue) {
				if (oldValue)
				{
					nos::Name oldName(static_cast<const char*>(oldValue->Data()));
					nosVariables->DeleteNodeReference(oldName, NodeId);
					nosVariables->UnregisterVariableUpdateCallback(oldName, CallbackId);
					CallbackId = -1;
				}
				auto newName = static_cast<const char*>(value.Data());
				if (strlen(newName) == 0)
				{
					SetStatus(VariableStatusItem::VariableName,
							  fb::NodeStatusMessageType::WARNING,
							  "Provide a name",
							  "",
							  5,
							  true);
					return;
				}
				if (newName == Name)
					return;
				Name = nos::Name(newName);
				nosName outTypeName{};
				nosBuffer outValue{};
				auto res = nosVariables->Get(Name, &outTypeName, &outValue);
				if (res != NOS_RESULT_SUCCESS)
				{
					auto valuePin = GetPin(NOS_NAME("Value"));
					if (res == NOS_RESULT_NOT_FOUND && valuePin && valuePin->TypeName != NSN_TypeNameGeneric)
					{
						RegisterVariable(valuePin, initialValue);
					}
					else
					{
						SetStatus(VariableStatusItem::VariableName,
								  fb::NodeStatusMessageType::FAILURE,
								  "Failed to get variable " + std::string(newName),
								  "",
								  5,
								  true);
						SetPinValue(NOS_NAME("Name"), "");
					}
					return;
				}
				nosVariables->AddNodeReference(Name, NodeId);
				CallbackId =
					nosVariables->RegisterVariableUpdateCallback(Name, &GetVariableNode::VariableUpdateCallback, this);
				ClearStatus(VariableStatusItem::VariableName);
				SetPinType(NOS_NAME("Value"), outTypeName);
				SetPinValue(NOS_NAME("Value"), outValue);
				SetNodeStatusMessages({{{},
										Name.AsString(),
										fb::NodeStatusMessageType::INFO,
										"Variable type and value set",
										3,
										true,
										false}});
			});
		return NOS_RESULT_SUCCESS;
	}

	~GetVariableNode() override
	{
		if (CallbackId != -1 && HasName())
			nosVariables->UnregisterVariableUpdateCallback(Name, CallbackId);
	}

	void RegisterVariable(const NodePin* valuePin, nos::Buffer const& initialValue)
	{
		auto res = nosVariables->Set(Name, valuePin->TypeName, initialValue.GetInternal());
		if (res != NOS_RESULT_SUCCESS)
		{
			SetStatus(VariableStatusItem::VariableName,
					  fb::NodeStatusMessageType::FAILURE,
					  "Failed to set variable " + Name.AsString(),
					  "",
					  5,
					  true);
			SetPinValue(NOS_NAME("Name"), "");
			return;
		}
		nosVariables->AddNodeReference(Name, NodeId);
		CallbackId = nosVariables->RegisterVariableUpdateCallback(Name, &GetVariableNode::VariableUpdateCallback, this);
	}

	void OnVariableUpdated(nos::Name name, nos::Name typeName, const nosBuffer* value)
	{
		if (!HasType())
			SetPinType(NOS_NAME("Value"), typeName);
		// This is here to force the pin's frame number to update, hacky.
		nosEngine.SetNodeDirty(NodeId);
		SetPinValue(NOS_NAME("Value"), *value);
	}

	static void VariableUpdateCallback(nosName name, void* userData, nosName typeName, const nosBuffer* value)
	{
		auto* node = static_cast<GetVariableNode*>(userData);
		node->OnVariableUpdated(name, typeName, value);
		nosEngine.TriggerNodeEvent(node->NodeId, NOS_NAME("OnVariableUpdated"));
	}
};

nosResult RegisterSetVariable(nosNodeFunctions* node)
{
	NOS_BIND_NODE_CLASS(NSN_SetVariable, SetVariableNode, node);
	return NOS_RESULT_SUCCESS;
}

nosResult RegisterGetVariable(nosNodeFunctions* node)
{
	NOS_BIND_NODE_CLASS(NSN_GetVariable, GetVariableNode, node);
	return NOS_RESULT_SUCCESS;
}
} // namespace nos::reflect
