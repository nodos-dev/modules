// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/SubsystemAPI.h>
#include <Nodos/NodeHelpers.hpp>

#include <nosPython/Python_generated.h>
#include <nosPython/nosPython.h>

// External
#define PYBIND11_SIMPLE_GIL_MANAGEMENT
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/embed.h>
namespace pyb = pybind11;
using namespace pyb::literals;

NOS_INIT()
NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

namespace nos::py
{
class Interpreter
{
public:
	Interpreter() : ScopedInterpreter(), Release()
	{
		pyb::gil_scoped_acquire gil;
		auto sysInfo = pyb::module::import("sys").attr("version").cast<std::string>();
		nosEngine.LogI("%s", sysInfo.c_str());
		try {
			NodosInternalModule = std::make_unique<pyb::module>(pyb::module::import("__nodos_internal__"));
		}
		catch (std::exception& e)
		{
			nosEngine.LogE(e.what(), "Failed to import __nodos_internal__ with error %s", e.what());
		}
	}

	~Interpreter()
	{
		pyb::gil_scoped_acquire gil;
		NodeInstances.clear();
		Modules.clear();
		NodosInternalModule.reset();
	}

	void AddPath(std::string path)
	{
		nosEngine.LogD("Adding module search path: %s", path.c_str());
		pyb::module::import("sys").attr("path").attr("append")(path);
	}

	nosResult ImportModule(nos::Name name, std::filesystem::path pySourcePath) {
		if (Modules.contains(name)) {
			try {
				pyb::gil_scoped_acquire gil;
				Modules[name]->reload();
			}
			catch (std::exception& e)
			{
				nosEngine.LogE(e.what(), "Failed to reload module %s with error %s", name.AsCStr(), e.what());
				return NOS_RESULT_FAILED;
			}
			return NOS_RESULT_SUCCESS;
		}
		try {
			pyb::gil_scoped_acquire gil;
			AddPath(pySourcePath.parent_path().generic_string());
			pyb::module pyModule = pyb::module::import(pySourcePath.filename().stem().generic_string().c_str());
			Modules[name] = std::make_unique<pyb::module>(std::move(pyModule));
		}
		catch (std::exception& e)
		{
			nosEngine.LogE(e.what(), "Failed to import module %s with error %s", name.AsCStr(), e.what());
			return NOS_RESULT_FAILED;
		}
		return NOS_RESULT_SUCCESS;
	}

	void CreateNodeInstance(const nosFbNode* node)
	{
		auto name = nos::Name(node->name()->str());
		auto classNameStr = node->class_name()->str();
		auto namespacedClassName = nos::Name(classNameStr);
		auto className = classNameStr.substr(classNameStr.find_last_of('.') + 1);
		auto id = *node->id();
		try {
			pyb::gil_scoped_acquire gil;
			pyb::object pyObject = Modules[namespacedClassName]->attr(className.c_str());
			pyb::object instance = pyObject();
			NodeInstances[id] = std::make_unique<pyb::object>(std::move(instance));
		}
		catch (std::exception& e)
		{
			nosEngine.LogE(e.what(), "Failed to create instance of %s with error %s", name.AsCStr(), e.what());
		}
	}

	void RemoveNodeInstance(nos::fb::UUID id)
	{
		if (NodeInstances.contains(id)) {
			pyb::gil_scoped_acquire gil;
			NodeInstances.erase(id);
		}
	}

	std::shared_ptr<pyb::object> GetNodeInstance(nos::fb::UUID id)
	{
		auto it = NodeInstances.find(id);
		if (it == NodeInstances.end())
			return nullptr;
		return it->second;
	}

protected:
	pyb::scoped_interpreter ScopedInterpreter;
	pyb::gil_scoped_release Release;
	std::unordered_map<nos::fb::UUID, std::shared_ptr<pyb::object>> NodeInstances;
	std::unordered_map<nos::Name, std::unique_ptr<pyb::module>> Modules;
	std::unique_ptr<pyb::module> NodosInternalModule;
};

static Interpreter* GInterpreter = nullptr;

// Classes with prefix 'PyNative' are only constructed from C++ side, with 'Py' are for Python-constructible.
class PyNativeNodeExecuteParams : public nos::NodeExecuteParams
{
public:
	using nos::NodeExecuteParams::NodeExecuteParams;

	std::optional<pyb::memoryview> GetPinValue(std::string pinName) const
	{
		auto it = this->find(nos::Name(pinName));
		if (it == this->end())
			return std::nullopt;
		auto buf = it->second.Data;
		return pyb::memoryview::from_memory(buf->Data, buf->Size);
	}
	std::optional<nosUUID> GetPinId(std::string pinName) const
	{
		auto it = this->find(nos::Name(pinName));
		if (it == this->end())
			return std::nullopt;
		return it->second.Id;
	}
};

struct PyNativeOnPinValueChangedArgs {
	nos::Name PinName = {};
	nosBuffer Value = {};
	PyNativeOnPinValueChangedArgs(nos::Name pinName, nosBuffer value) : PinName(pinName), Value(value) {}

	pyb::memoryview GetPinValue() const
	{
		return pyb::memoryview::from_memory(Value.Data, Value.Size);
	}
	nos::Name GetPinName() const
	{
		return PinName;
	}
};

struct PyNativeOnPinConnectedArgs {
	nos::Name PinName = {};
	PyNativeOnPinConnectedArgs(nos::Name pinName) : PinName(pinName) {}
};

struct PyNativeOnPinDisconnectedArgs {
	nos::Name PinName = {};
	PyNativeOnPinDisconnectedArgs(nos::Name pinName) : PinName(pinName) {}
};

struct PyNativeContextMenuRequestInstigator
{
	uint32_t EditorId;
	uint32_t RequestId;
	PyNativeContextMenuRequestInstigator(uint32_t editorId, uint32_t requestId) : EditorId(editorId), RequestId(requestId) {}
};
	
struct PyNativeContextMenuRequest
{
	PyNativeContextMenuRequest(const nosContextMenuRequest* request) : Instigator(request->instigator()->client_id(), request->instigator()->request_id())
	{
		ItemId = *request->item_id();
		Pos = *request->pos();
	}
	nosUUID ItemId {};
	nos::fb::vec2 Pos {};
	PyNativeContextMenuRequestInstigator Instigator;
};

// TODO: TODO: Use pyb::class_ to bind
std::unique_ptr<nos::TContextMenuItem> ConvertPyObjectToContextMenuItem(const pyb::object& item)
{
	nos::TContextMenuItem menuItem;
	menuItem.display_name = item.attr("text").cast<std::string>();
	if (auto command = item.attr("command_id"); !command.is_none())
		menuItem.command = command.cast<uint32_t>();
	if (auto subMenu = item.attr("sub_items"); !subMenu.is_none())
	{
		for (auto& subItem : subMenu.cast<std::vector<pyb::object>>())
			menuItem.content.emplace_back(ConvertPyObjectToContextMenuItem(subItem));
	}
	return std::make_unique<nos::TContextMenuItem>(menuItem);
}

PYBIND11_EMBEDDED_MODULE(__nodos_internal__, m)
{
	// Enums
	pyb::class_<nosResult>(m, "result")
		.def_property_readonly_static("SUCCESS", [](const pyb::object&) {return NOS_RESULT_SUCCESS; })
		.def_property_readonly_static("FAILED", [](const pyb::object&) {return NOS_RESULT_FAILED; });

	// Structs
	pyb::class_<nosUUID>(m, "uuid")
		.def(pyb::init<>())
		.def("__str__", [](const nosUUID& id) -> std::string { return nos::UUID2STR(id); })
		.def("__hash__", [](const nosUUID& id) -> size_t { return nos::UUIDHash(id); })
		.def("__eq__", [](const nosUUID& self, const nosUUID& other) -> bool { return self == other; });

	pyb::class_<nos::Name>(m, "Name")
		.def(pyb::init<uint64_t>())
		.def(pyb::init<const std::string&>())
		.def("__str__", [](const nos::Name& name) -> std::string { return name.AsString(); })
		.def("__hash__", [](const nos::Name& name) -> size_t { return name.ID; })
		.def("__eq__", [](const nos::Name& self, const nos::Name& other) -> bool { return self == other; });

	pyb::class_<PyNativeNodeExecuteParams>(m, "NodeExecuteArgs")
		.def_property_readonly("node_class_name", [](const PyNativeNodeExecuteParams& args) -> std::string_view { return args.NodeClassName.AsCStr(); })
		.def_property_readonly("node_name", [](const PyNativeNodeExecuteParams& args) -> std::string_view { return args.NodeName.AsCStr(); })
		.def("get_pin_value", &PyNativeNodeExecuteParams::GetPinValue, "Access the memory of the pin specified by 'pin_name'", "pin_name"_a)
		.def("get_pin_id", &PyNativeNodeExecuteParams::GetPinId, "Get the unique identifier of the pin specified by 'pin_name'", "pin_name"_a);

	pyb::class_<PyNativeOnPinValueChangedArgs>(m, "PinValueChangedArgs")
		.def_property_readonly("pin_value", [](const PyNativeOnPinValueChangedArgs& args) -> pyb::memoryview { return args.GetPinValue(); })
		.def_property_readonly("pin_name", [](const PyNativeOnPinValueChangedArgs& args) -> std::string_view { return args.PinName.AsCStr(); });

	pyb::class_<PyNativeOnPinConnectedArgs>(m, "PinConnectedArgs")
		.def_property_readonly("pin_value", [](const PyNativeOnPinConnectedArgs& args) -> std::string_view { return args.PinName.AsCStr(); });
	
	pyb::class_<PyNativeOnPinDisconnectedArgs>(m, "PinDisconnectedArgs")
		.def_property_readonly("pin_value", [](const PyNativeOnPinDisconnectedArgs& args) -> std::string_view { return args.PinName.AsCStr(); });

	pyb::class_<PyNativeContextMenuRequest>(m, "ContextMenuRequest")
		.def_property("item_id", [](const PyNativeContextMenuRequest& req) -> nosUUID { return req.ItemId; }, [](PyNativeContextMenuRequest& req, nosUUID id) { req.ItemId = id; })
		.def_property("position", [](const PyNativeContextMenuRequest& req) -> nos::fb::vec2 { return req.Pos; }, [](PyNativeContextMenuRequest& req, nos::fb::vec2 pos) { req.Pos = pos; })
		.def_property("instigator", [](const PyNativeContextMenuRequest& req) -> PyNativeContextMenuRequestInstigator { return req.Instigator; }, [](PyNativeContextMenuRequest& req, PyNativeContextMenuRequestInstigator instigator) { req.Instigator = instigator; });

	pyb::class_<PyNativeContextMenuRequestInstigator>(m, "ContextMenuRequestInstigator")
		.def_property("editor_id", [](const PyNativeContextMenuRequestInstigator& instigator) -> uint32_t { return instigator.EditorId; }, [](PyNativeContextMenuRequestInstigator& instigator, uint32_t id) { instigator.EditorId = id; })
		.def_property("request_id", [](const PyNativeContextMenuRequestInstigator& instigator) -> uint32_t { return instigator.RequestId; }, [](PyNativeContextMenuRequestInstigator& instigator, uint32_t id) { instigator.RequestId = id; });

	// Engine Services
	m.def("set_pin_value",
		[](const nosUUID& id, const pyb::buffer& buf) {
			auto info = buf.request();
			nosEngine.SetPinValue(id, nosBuffer{.Data = info.ptr, .Size = info.size < 0 ? 0ull : (size_t(info.size) * info.itemsize)});
		});
	m.def("log_info",
		[](const std::string& log) {
			nosEngine.LogI(log.c_str());
		});
	m.def("log_warning",
		[](const std::string& log) {
			nosEngine.LogW(log.c_str());
		});
	m.def("log_error",
		[](const std::string& log) {
			nosEngine.LogE(log.c_str());
		});
	m.def("check_compat",
		[](int major, int minor) -> bool {
			if (major != NOS_PY_MAJOR_VERSION)
				return false;
			if (minor > NOS_PY_MINOR_VERSION)
				return false;
			return true;
		});
	m.def("get_name", [](const std::string& name) -> nos::Name { return nos::Name(name); });
	m.def("get_string", [](uint64_t nameId) -> std::string { return nos::Name(nameId).AsString(); });
	m.def("send_context_menu_update", [](std::vector<pyb::object> const& items, PyNativeContextMenuRequest const& req) { 
		app::TAppContextMenuUpdate update;
		update.item_id = req.ItemId;
		update.pos = req.Pos;
		for (auto& item : items)
			update.menu.emplace_back(ConvertPyObjectToContextMenuItem(item));
		flatbuffers::FlatBufferBuilder fbb;
		HandleEvent(CreateAppEvent(fbb, app::CreateAppContextMenuUpdate(fbb, &update)));
	});
}

void Init()
{
	GInterpreter = new Interpreter;
}

void Deinit()
{
	delete GInterpreter;
}

nosResult NOSAPI_CALL OnPyNodeRegistered(nosModuleIdentifier pluginId, nosName className, nosBuffer options)
{
	char path[2048];
	nosEngine.GetModuleFolderPath(pluginId, 2048, path);
	fs::path moduleRoot = std::string(path);

	auto* pyNodeOptions = flatbuffers::GetRoot<PythonNode>(options.Data);

	if (!flatbuffers::IsFieldPresent(pyNodeOptions, PythonNode::VT_SOURCE))
		return NOS_RESULT_INVALID_ARGUMENT;

	fs::path relSourcePath = pyNodeOptions->source()->str();
	std::string sourcePathStr = (moduleRoot / relSourcePath).generic_string();

	if (!fs::exists(sourcePathStr))
	{
		nosEngine.LogE("Python Subsystem: Source file %s does not exist.", sourcePathStr.c_str());
		return NOS_RESULT_INVALID_ARGUMENT;
	}

	return GInterpreter->ImportModule(className, std::filesystem::canonical(sourcePathStr));
}

class PyNativeNode : public nos::NodeContext
{
public:
	PyNativeNode(const nosFbNode* node) : NodeContext(node)
	{
		GInterpreter->CreateNodeInstance(node);
	}

	~PyNativeNode()
	{
		GInterpreter->RemoveNodeInstance(NodeId);
	}

	std::shared_ptr<pyb::object> GetPyObject() const
	{
		return GInterpreter->GetNodeInstance(NodeId);
	}

	template <typename RetType, typename... Args>
	requires std::is_same_v<RetType, void> || std::is_same_v<RetType, nosResult>
	RetType CallMethod(const std::string& methodName, Args... args)
	{
		auto m = GetPyObject();
		if (!m)
		{
			if constexpr (std::is_same_v<RetType, nosResult>)
				return NOS_RESULT_NOT_FOUND;
			else
				return;
		}
		try {
			pyb::gil_scoped_acquire gil;
			if constexpr (std::is_same_v<RetType, void>)
				m->attr(methodName.c_str())(std::forward<Args>(args)...);
			else
				return m->attr(methodName.c_str())(std::forward<Args>(args)...).cast<RetType>();
		}
		catch (std::exception& exp)
		{
			nosEngine.LogDE(exp.what(), "%s:%s method call failed. See details.", NodeName.AsCStr(), methodName.c_str());
			if constexpr (std::is_same_v<RetType, nosResult>)
				return NOS_RESULT_FAILED;
		}
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto m = GetPyObject();
		if (!m)
			return NOS_RESULT_NOT_FOUND;

		return CallMethod<nosResult>("execute_node", NOS_RESULT_FAILED, PyNativeNodeExecuteParams(params));
	}

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{
		CallMethod<void>("on_pin_value_changed", PyNativeOnPinValueChangedArgs(pinName, value));
	}

	void OnNodeMenuRequested(const nosContextMenuRequest* request) override
	{
		CallMethod<void>("on_node_menu_requested", PyNativeContextMenuRequest(request));
	}
	
	void OnPinMenuRequested(nos::Name pinName, const nosContextMenuRequest* request) override
	{
		CallMethod<void>("on_pin_menu_requested", nos::Name(pinName), PyNativeContextMenuRequest(request));
	}

	void OnMenuCommand(nosUUID itemID, uint32_t cmd) override
	{
		CallMethod<void>("on_menu_command", itemID, cmd);
	}

	void OnPinConnected(nos::Name pinName, nosUUID connectedPin) override
	{
		CallMethod<void>("on_pin_connected", PyNativeOnPinConnectedArgs(pinName));
	}
	
	void OnPinDisconnected(nos::Name pinName) override
	{
		CallMethod<void>("on_pin_disconnected", PyNativeOnPinDisconnectedArgs(pinName));
	}
};

nosResult NOSAPI_CALL ExportSubsystemNodeFunctions(size_t* outSize, nosSubsystemNodeFunctions** outList)
{
	*outSize = 1;
	if (!outList)
		return NOS_RESULT_SUCCESS;
	auto pyFuncs = outList[0];
	pyFuncs->OnNodeClassRegistered = nos::py::OnPyNodeRegistered;
	pyFuncs->NodeType = NOS_NAME_STATIC("nos.py.PythonNode");
	auto* functions = &pyFuncs->NodeFunctions;
	NOS_BIND_NODE_CLASS(0, nos::py::PyNativeNode, functions);
	return NOS_RESULT_SUCCESS;
}

nosResult NOSAPI_CALL OnPreUnloadSubsystem()
{
	nos::py::Deinit();
	// Python DLL might not be released when nos.py is unloaded, due to some third party python module (like numpy).
	// Since PyFinalize is called, interpreter ends up in an invalid state and
	// subsequent imports after nos.py reloaded will cause Nodos to crash.
	// Related issues:
	// - https://github.com/numpy/numpy/issues/8097
	// - https://github.com/python/cpython/issues/78490
	// - https://bugs.python.org/issue401713#msg34524
	// One solution would be to statically link Python to nos.py & fork Python to solve the
	// issue (or create a PR and wait for them to add to the release).
	// Another definite & future-proof solution would be to spin-up a new process that embeds Python and communicate it
	// via IPC from nos.py. But this creates some limitations & complexity.
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::py

extern "C"
{

NOSAPI_ATTR nosResult NOSAPI_CALL nosExportSubsystem(nosSubsystemFunctions* subsystemFunctions)
{
	nos::py::Init();
	subsystemFunctions->OnPreUnloadSubsystem = nos::py::OnPreUnloadSubsystem;
	subsystemFunctions->ExportSubsystemNodeFunctions = nos::py::ExportSubsystemNodeFunctions;
	return NOS_RESULT_SUCCESS;
}


}