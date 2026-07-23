// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "TypeCommon.h"

#include <algorithm>
#include <map>
#include <unordered_set>

// Nodos SDK
#include <PluginManifest_generated.h>

namespace nos::reflect
{
NOS_REGISTER_NAME(Type)

// Make: constructs a value from input pins. A scalar/string/union is built from a single "Value" pin; a
// struct exposes one input pin per field; a nos.Dict is built from user-named "Field N" pins whose
// display names become the dict keys (folded-in Make Dict behaviour). The target type is set from the
// output pin connection, a template parameter (typed presets), or the node's "Set Type" menu.
struct MakeNode : NodeContext
{
	std::optional<nos::TypeInfo> Type = {};
	nosObjectKind ObjectKind = NOS_OBJECT_KIND_PRIMITIVE;
	fb::TVisualizer Visualizer = {};

	enum MenuCommand : uint32_t
	{
		ADD_FIELD = 1,
		REMOVE_FIELD = 2,
		RESET_TYPE = 3,
		MAKE_DICT = 4,				   // turn an unresolved Make into a dict maker
		SET_FIELD_TYPE_BASE = 0x10000, // + index into AvailableTypes: set a dict field pin's type
		SET_NODE_TYPE_BASE = 0x20000,  // + index into AvailableTypes: set the whole node's type
	};

	bool IsDict() const { return Type && Type->TypeName == NSN_DictTypeName; }

	nosResult OnCreate(nosFbNodePtr node) override
	{
		std::optional<nos::Name> typeName;
		if (flatbuffers::IsFieldPresent(node, fb::Node::VT_TEMPLATE_PARAMETERS) && 1 == node->template_parameters()->size())
			typeName = nos::Name((const char*)node->template_parameters()->Get(0)->value()->Data());
		else if (auto* pins = node->pins()) // typed/dict presets carry the target type on the Output pin
			for (auto* pin : *pins)
				if (pin->show_as() == fb::ShowAs::OUTPUT_PIN && nos::Name(pin->name()->string_view()) == NSN_Output)
				{
					typeName = nos::Name(pin->type_name()->string_view());
					break;
				}

		if (!typeName || *typeName == NSN_TypeNameGeneric)
			return NOS_RESULT_SUCCESS;

		// Auto-name freshly instantiated typed presets ("Make Float", ...); dict keeps its preset name.
		std::optional<std::string> updateDisplayName;
		if (*typeName != NSN_DictTypeName && flatbuffers::IsFieldPresent(node, fb::Node::VT_DISPLAY_NAME) &&
			node->display_name()->str().empty())
			updateDisplayName = "Make " + typeName->AsString();
		OnTypeUpdated(*typeName, updateDisplayName ? updateDisplayName->c_str() : nullptr);
		if (IsDict())
			UnorphanFieldPins(node);
		return NOS_RESULT_SUCCESS;
	}

	// Field pins restored from a saved graph can come back orphaned/passive; reactivate them so they are
	// live again on load (a la nos.math.Eval).
	void UnorphanFieldPins(nosFbNodePtr node)
	{
		std::vector<uuid> toUnorphan;
		auto pins = node->pins();
		if (!pins)
			return;
		for (auto* pin : *pins)
		{
			if (pin->show_as() != fb::ShowAs::INPUT_PIN)
				continue;
			if (auto orphan = pin->orphan_state())
				if (orphan->type() == fb::PinOrphanStateType::PASSIVE || orphan->type() == fb::PinOrphanStateType::ORPHAN)
					toUnorphan.push_back(*pin->id());
		}
		for (auto const& id : toUnorphan)
			SetPinOrphanState(id, fb::PinOrphanStateType::ACTIVE);
	}

	// Strict mode: only builtin types are supported (used to filter the auto-generated presets).
	static bool IsTypeSupported(nos::TypeInfo& info, bool strict)
	{
		if (info->BaseType == NOS_BASE_TYPE_NONE || info->BaseType == NOS_BASE_TYPE_ARRAY)
			return false;
		// If has 'skip_make' attribute
		if (info->BaseType == NOS_BASE_TYPE_STRUCT || info->BaseType == NOS_BASE_TYPE_UNION || IsEnumType(info))
		{
			bool skip = strict;
			for (int i = 0; i < info->AttributeCount; ++i)
			{
				if (info->Attributes[i].Name == NOS_NAME_STATIC("builtin") || info->Attributes[i].Name == NOS_NAME_STATIC("force_make"))
					skip = false;
				else if (info->Attributes[i].Name == NOS_NAME_STATIC("skip_make"))
				{
					skip = true;
					break;
				}
			}
			if (skip)
				return false;
		}
		return true;
	}

	void OnPinConnected(nos::Name pinName, uuid const& connectedPin, nosObjectId connectedObject) override
	{
		if (pinName == NSN_Value && Type && Type->TypeName == NOS_NAME_STATIC("string"))
		{
			nosBuffer buffer{};
			nosEngine.GetPinVisualizer(connectedPin, "", &buffer);
			if (buffer.Size)
			{
				SetPinVisualizer(NSN_Value, *InterpretObjectData<fb::Visualizer>(buffer)->UnPack());
				nosEngine.FreeBuffer(&buffer);
			}
		}
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		if (!Type)
			return NOS_RESULT_SUCCESS;

		if (IsDict())
			return ExecuteDict(params);

		auto& type = *Type;
		switch (type->BaseType)
		{
		case NOS_BASE_TYPE_FLOAT:
		case NOS_BASE_TYPE_INT:
		case NOS_BASE_TYPE_UINT:
		case NOS_BASE_TYPE_STRING:
		case NOS_BASE_TYPE_UNION:
			nosEngine.SetPinValue(params[NSN_Output].Id, params.GetPinBuffer(NSN_Value));
			return NOS_RESULT_SUCCESS;
		}

		switch (ObjectKind)
		{
		case NOS_OBJECT_KIND_PRIMITIVE:
		case NOS_OBJECT_KIND_FOREIGN: {
			for (auto const& [name, pin] : params)
			{
				if (name == NSN_Output)
					continue;
				SetField(params[NSN_Output].Id,
						 {nosDataPathComponent{.ComponentType = NOS_DATA_PATH_FIELD_COMPONENT, .Component = name}},
						 *GetObjectDataView(*pin.Object));
			}
			break;
		}
		case NOS_OBJECT_KIND_COMPOSITE: {
			std::vector<CompositeObjectField> fields;
			for (auto const& [name, pin] : params)
			{
				if (name == NSN_Output)
					continue;
				fields.push_back({name, *pin.Object});
			}
			auto obj = CompositeObjectRef::Create(type->TypeName, fields);
			if (obj)
				SetPinObject(params[NSN_Output].Id, *obj);
			else
				return NOS_RESULT_FAILURE;
			break;
		}
		}
		return NOS_RESULT_SUCCESS;
	}

	std::vector<std::pair<nos::Name, uint64_t>> LastSignature; // sorted (key, value object id) last published

	// Bundle the field pins into a nos.Dict, keyed by each field's display name. Only republish when the
	// inputs actually changed, so we don't churn a fresh object (and re-trigger downstream) every execute.
	nosResult ExecuteDict(NodeExecuteParams const& params)
	{
		std::vector<CompositeObjectField> fields;
		std::vector<std::pair<nos::Name, uint64_t>> signature;
		std::unordered_set<nos::Name> seenKeys;
		for (auto const& [name, info] : params)
		{
			if (name == NSN_Output)
				continue;
			auto* pin = GetPin(name);
			if (!pin || pin->ShowAs != fb::ShowAs::INPUT_PIN)
				continue;
			ObjectRef value = params.GetPinObject(name);
			if (!value.IsValid())
				continue;
			if (!seenKeys.insert(pin->DisplayName).second)
			{
				nosEngine.LogW("Make Dict: duplicate key '%s' ignored", pin->DisplayName.AsString().c_str());
				continue;
			}
			signature.push_back({pin->DisplayName, value.GetObjectId().Value});
			fields.push_back({pin->DisplayName, std::move(value)});
		}
		std::sort(signature.begin(), signature.end(),
				  [](auto const& a, auto const& b) { return a.first.AsString() < b.first.AsString(); });
		if (signature == LastSignature)
			return NOS_RESULT_SUCCESS;
		LastSignature = std::move(signature);
		auto dict = DictRef::Create(fields);
		if (!dict)
			return NOS_RESULT_FAILURE;
		SetPinObject(NSN_Output, *dict);
		params.MarkAllOutsDirty = false;
		return NOS_RESULT_SUCCESS;
	}

	// --- Type selection / dict field menus --------------------------------------------------------

	std::vector<nos::Name> AvailableTypes;

	std::vector<nos::Name> const& GetAvailableTypes()
	{
		if (!AvailableTypes.empty())
			return AvailableTypes;
		size_t count = 0;
		if (nosEngine.GetPinDataTypeNames(nullptr, nullptr, &count) == NOS_RESULT_FAILED)
			return AvailableTypes;
		std::vector<nosName> names(count);
		nosEngine.GetPinDataTypeNames(nullptr, names.data(), &count);
		for (auto n : names)
			if (n != NSN_TypeNameGeneric)
				AvailableTypes.push_back(n);
		std::sort(AvailableTypes.begin(), AvailableTypes.end(),
				  [](nos::Name a, nos::Name b) { return a.AsString() < b.AsString(); });
		return AvailableTypes;
	}

	// Build a "Set Type" submenu, grouping namespaced types (e.g. nos.fb.*) under nested submenus.
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
		if (IsDict())
		{
			SendContextMenu(request, [](ContextMenuBuilder& menu) { menu.Item("Add Field", ADD_FIELD); });
			return;
		}
		if (Type)
			return; // already a concrete type; nothing to set
		auto const& types = GetAvailableTypes();
		SendContextMenu(request, [&](ContextMenuBuilder& menu) {
			menu.Submenu("Set Type", [&](ContextMenuBuilder& sub) { BuildTypeMenu(sub, types, SET_NODE_TYPE_BASE); });
			menu.Item("Make Dictionary", MAKE_DICT); // first-class dict entry alongside Set Type
		});
	}

	void OnPinMenuRequested(nos::Name pinName, nosContextMenuRequestPtr request) override
	{
		if (!IsDict() || pinName == NSN_Output)
			return;
		auto const& types = GetAvailableTypes();
		SendContextMenu(request, [&](ContextMenuBuilder& menu) {
			menu.Submenu("Set Type", [&](ContextMenuBuilder& sub) { BuildTypeMenu(sub, types, SET_FIELD_TYPE_BASE); });
			menu.Item("Reset Type", RESET_TYPE);
			menu.Item("Remove Field", REMOVE_FIELD);
		});
	}

	void OnMenuCommand(uuid const& itemId, uint32_t cmd) override
	{
		auto const& types = GetAvailableTypes();
		switch (cmd)
		{
		case ADD_FIELD:
			AddField();
			return;
		case REMOVE_FIELD:
			if (itemId != NodeId)
				DeletePin(itemId);
			return;
		case RESET_TYPE:
			if (itemId != NodeId)
				SetPinType(itemId, NSN_TypeNameGeneric);
			return;
		case MAKE_DICT:
			SetType(nos::TypeInfo(nos::Name(NSN_DictTypeName)));
			return;
		}
		if (cmd >= SET_NODE_TYPE_BASE)
		{
			size_t idx = cmd - SET_NODE_TYPE_BASE;
			if (idx < types.size())
				SetType(nos::TypeInfo(types[idx]));
		}
		else if (cmd >= SET_FIELD_TYPE_BASE && itemId != NodeId)
		{
			size_t idx = cmd - SET_FIELD_TYPE_BASE;
			if (idx < types.size())
				SetPinType(itemId, types[idx]);
		}
	}

	size_t NextFieldIndex()
	{
		size_t i = 0;
		while (GetPin(nos::Name("Field " + std::to_string(i))))
			i++;
		return i;
	}

	void AddField()
	{
		auto name = nos::Name("Field " + std::to_string(NextFieldIndex()));
		AddPin(PinBuilder(name)
				   .SetDisplayName(name)
				   .SetTypeName(NSN_TypeNameGeneric)
				   .SetShowAs(nos::fb::ShowAs::INPUT_PIN)
				   .SetCanShowAs(nos::fb::CanShowAs::INPUT_PIN_OR_PROPERTY));
	}

	// Set the dynamic node's type by resolving the output pin. The chosen type then persists via the
	// output pin's type_name (read back in OnCreate) - template parameters are reserved for
	// construction-time presets, not runtime type changes.
	void SetType(nosTypeInfo const* typeInfo) { PinResolveRequest(NSN_Output, typeInfo->TypeName); }

	void OnPinUpdated(const nosPinUpdate* update) override
	{
		if (Type)
			return;
		if (update->UpdatedField == NOS_PIN_FIELD_TYPE_NAME && update->PinName == NSN_Output)
			OnTypeUpdated(update->TypeName);
	}

	void OnTypeUpdated(nos::Name typeName, const char* updatedDisplayName = nullptr)
	{
		Type = nos::TypeInfo(typeName);
		if (NOS_RESULT_SUCCESS != nosEngine.ObjectAPI->GetObjectKindFromTypeName(typeName, &ObjectKind))
		{
			SetNodeOrphanState(fb::NodeOrphanStateType::ORPHAN, "Invalid type");
			return;
		}
		if (updatedDisplayName)
			SetNodeDisplayName(updatedDisplayName);
		LoadPins();
	}

	nosResult OnResolvePinDataTypes(nosResolvePinDataTypesParams* params) override
	{
		if (IsDict())
		{
			// The engine pre-fills every still-generic pin with the incoming type. Only the instigated
			// field should adopt it; reset the others to their own type so each field resolves
			// independently rather than all fields snapping to the first connected type.
			for (size_t i = 0; i < params->PinCount; i++)
			{
				auto& pin = params->Pins[i];
				pin.OutResolvedTypeName = (nos::Name(pin.Name) == nos::Name(params->InstigatorPinName))
											  ? params->IncomingTypeName
											  : pin.TypeName;
			}
			return NOS_RESULT_SUCCESS;
		}
		nos::TypeInfo incomingType(params->IncomingTypeName);
		if (!IsTypeSupported(incomingType, false))
		{
			strcpy(params->OutErrorMessage, "Type not supported for make.");
			return NOS_RESULT_FAILED;
		}
		return NOS_RESULT_SUCCESS;
	}

	// Ensure the Output pin exists with the right type, reactivating/retyping an existing one.
	void EnsureOutputPin(nos::Name typeName, std::optional<nos::Buffer> defaultValue)
	{
		if (auto* out = GetPin(NSN_Output))
		{
			if (out->IsOrphan)
				SetPinOrphanState(out->Id, fb::PinOrphanStateType::ACTIVE);
			if (out->TypeName != typeName)
				SetPinType(out->Id, typeName);
			return;
		}
		PinBuilder builder(NSN_Output);
		builder.SetDisplayName(typeName)
			.SetTypeName(typeName)
			.SetShowAs(nos::fb::ShowAs::OUTPUT_PIN)
			.SetCanShowAs(nos::fb::CanShowAs::OUTPUT_PIN_ONLY);
		if (defaultValue)
			builder.SetValue(std::move(*defaultValue));
		AddPin(builder);
	}

	void LoadPins()
	{
		auto& type = *Type;

		if (IsDict())
		{
			EnsureOutputPin(type->TypeName, std::nullopt);
			if (NextFieldIndex() == 0)
				AddField(); // a fresh dict starts with one field
			return;
		}

		assert(type->BaseType != NOS_BASE_TYPE_NONE);
		auto defBuf = GetDefaultValueOfType(type->TypeName);
		if (!defBuf)
			return;
		nos::Buffer typeDefault = defBuf->GetBuffer();

		std::unordered_set<nosName> accepted{NSN_Output};
		EnsureOutputPin(type->TypeName, typeDefault);

		switch (type->BaseType)
		{
		case NOS_BASE_TYPE_INT:
		case NOS_BASE_TYPE_UINT:
		case NOS_BASE_TYPE_FLOAT:
		case NOS_BASE_TYPE_STRING:
		case NOS_BASE_TYPE_UNION: {
			// A primitive is constructed from a single "Value" pin.
			accepted.insert(NSN_Value);
			if (auto* pin = GetPin(NSN_Value))
			{
				if (pin->IsOrphan)
					SetPinOrphanState(pin->Id, fb::PinOrphanStateType::ACTIVE);
				if (type->BaseType == NOS_BASE_TYPE_STRING)
				{
					nosBuffer buffer{};
					nosEngine.GetPinVisualizer(pin->Id, "", &buffer);
					if (buffer.Size)
					{
						InterpretObjectData<fb::Visualizer>(buffer)->UnPackTo(&Visualizer);
						nosEngine.FreeBuffer(&buffer);
					}
				}
			}
			else
			{
				nos::Buffer value = type->BaseType == NOS_BASE_TYPE_STRING ? nos::Buffer(std::vector<uint8_t>(1, 0))
																		   : nos::Buffer(std::vector<uint8_t>(type->ByteSize));
				AddPin(PinBuilder(NSN_Value)
						   .SetDisplayName(NSN_Value)
						   .SetTypeName(type->TypeName)
						   .SetShowAs(fb::ShowAs::INPUT_PIN)
						   .SetCanShowAs(fb::CanShowAs::INPUT_PIN_OR_PROPERTY)
						   .SetValue(std::move(value)));
			}
			break;
		}
		case NOS_BASE_TYPE_STRUCT: {
			for (int i = 0; i < type->FieldCount; ++i)
			{
				auto& field = type->Fields[i];
				accepted.insert(field.Name);
				if (auto* pin = GetPin(field.Name))
				{
					if (pin->TypeName == field.Type->TypeName)
					{
						if (pin->IsOrphan)
							SetPinOrphanState(pin->Id, fb::PinOrphanStateType::ACTIVE);
						continue;
					}
					DeletePin(pin->Id); // field type changed: recreate with the new type
				}
				AddPin(PinBuilder(field.Name)
						   .SetDisplayName(field.Name)
						   .SetTypeName(field.Type->TypeName)
						   .SetShowAs(fb::ShowAs::INPUT_PIN)
						   .SetCanShowAs(fb::CanShowAs::INPUT_PIN_OR_PROPERTY)
						   .SetValue(FieldInitialValue(typeDefault, type->TypeName, field)));
			}
			break;
		}
		}

		for (auto& [id, pin] : Pins)
			if (!accepted.contains(pin.Name))
				DeletePin(id);
	}

	// The field's value taken from the type's default buffer, falling back to the field's own default.
	nos::Buffer FieldInitialValue(nos::Buffer const& typeDefault, nos::Name typeName, nosFieldInfo const& field)
	{
		nosQueryBufferParams params = {};
		params.Buffer = typeDefault;
		nosDataPathComponent path = {};
		path.Component.FieldName = field.Name;
		path.ComponentType = NOS_DATA_PATH_FIELD_COMPONENT;
		params.Path = &path;
		params.PathLength = 1;
		params.TypeName = typeName;
		auto queriedField = QueryBuffer(params);
		if (queriedField) // empty arrays come back as an empty buffer, which is still the right value
			return nos::Buffer(queriedField->Data(), queriedField->Size());
		if (field.Type->ByteSize)
			return nos::Buffer(field.DefaultValue.Data, field.DefaultValue.Size);
		return nos::Buffer();
	}
};

// Type names (by nosName id) we've already emitted a "Make <Type>" quick-add preset for. Engine node
// menu entries are append-only, so we must only ever register presets for types not seen before.
// Accessed only from the plugin manager thread (RegisterMake at load & OnPostOtherPluginLoaded).
static std::unordered_set<uint64_t> GMakePresetTypes;

// Registers "Make <Type>" quick-add presets for the given types that we haven't emitted one for yet.
// Only the delta is registered (engine menu entries are append-only), so it's safe to call repeatedly.
void RegisterMakePresetsForTypes(const nosName* typeNames, size_t count)
{
	std::vector<nos::Buffer> nodePresets;
	for (size_t i = 0; i < count; ++i)
	{
		nosName typeName = typeNames[i];
		nos::TypeInfo typeInfo(typeName);
		if (!MakeNode::IsTypeSupported(typeInfo, true))
			continue;
		if (typeInfo.TypeName == NSN_DictTypeName)
			continue; // "Make Dict" is provided as a curated preset in Make.nosnode
		if (!GMakePresetTypes.insert(typeName.ID).second)
			continue; // already emitted a preset for this type
		std::string name = nos::Name(typeInfo.TypeName).AsString();
		auto idx = name.find_last_of(".");
		idx = idx == std::string::npos ? 0 : 1 + idx;
		fb::TNodePreset preset;
		fb::TNodeMenuInfo info;
		info.category = "Type";
		info.display_name = "Make " + name.substr(idx);
		preset.menu_info = std::make_unique<fb::TNodeMenuInfo>(std::move(info));
		std::vector<uint8_t> data(1 + name.size());
		memcpy(data.data(), name.data(), name.size());
		preset.params.emplace_back(new fb::TTemplateParameter{{}, NSN_Type.AsString(), "string", std::move(data)});
		flatbuffers::FlatBufferBuilder fbb;
		fbb.Finish(CreateNodePreset(fbb, &preset));
		nos::Buffer buf = fbb.Release();
		nodePresets.push_back(std::move(buf));
	}
	if (nodePresets.empty())
		return;
	std::vector<nosFbNodePresetPtr> fbNodePresets;
	for (auto& buf : nodePresets)
		fbNodePresets.push_back(flatbuffers::GetMutableRoot<nos::fb::NodePreset>(buf.Data()));
	nosEngine.RegisterNodePresets(NOS_NAME("nos.reflect.Make"), fbNodePresets.size(), fbNodePresets.data());
}

// Load-time seeding: enumerate every currently-registered type (builtins + already-loaded plugins) once.
// Newcomers from plugins loaded later arrive directly via OnPostOtherPluginLoaded (no re-enumeration).
void RegisterMakePresets()
{
	std::vector<nosName> typeNames;
	size_t count = 0;
	auto res = nosEngine.GetPinDataTypeNames(nullptr, 0, &count);
	if (NOS_RESULT_FAILED != res)
	{
		typeNames.resize(count);
		nosEngine.GetPinDataTypeNames(nullptr, typeNames.data(), &count);
	}
	RegisterMakePresetsForTypes(typeNames.data(), typeNames.size());
}

nosResult RegisterMake(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Make, MakeNode, fn);
	RegisterMakePresets();
	return NOS_RESULT_SUCCESS;
}

nosResult RegisterMakeDynamic(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_MakeDynamic, MakeNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::reflect
