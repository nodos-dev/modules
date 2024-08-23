// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginAPI.h>

#include <Builtins_generated.h>

#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/nosVulkanSubsystem.h>
#include <nosVulkanSubsystem/Helpers.hpp>

#if defined(_WIN32)
#include "Window/WindowNode.h"
#endif
NOS_INIT()

NOS_REGISTER_NAME(in1)
NOS_REGISTER_NAME(in2)
NOS_REGISTER_NAME(out)

NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_IMPORT()
NOS_END_IMPORT_DEPS()


namespace nos::test
{


class TestNode : public nos::NodeContext
{
public:
    TestNode(const nosFbNode* node) : nos::NodeContext(node)
    {
        nosEngine.LogI("TestNode: Constructor");
        AddPinValueWatcher(NOS_NAME_STATIC("double_prop"), [this](nos::Buffer const& newVal, std::optional<nos::Buffer> oldVal) {
            double optOldVal = 0.0f;
            if (oldVal)
                optOldVal = *oldVal->As<double>();
            nosEngine.LogI("TestNode: double_prop changed to %f from %f", *newVal.As<double>(), optOldVal);
        });
    }

    ~TestNode()
    {
        nosEngine.LogI("TestNode: Destructor");
    }

    void OnNodeUpdated(const nosFbNode* updatedNode) override
    {
        nosEngine.LogI("TestNode: OnNodeUpdated");
    }

    void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
    {
        nosEngine.LogI("TestNode: OnPinValueChanged");
    }

    virtual void OnPinConnected(nos::Name pinName, nosUUID connectedPin) override
    {
        nosEngine.LogI("TestNode: OnPinConnected");
    }

    virtual void OnPinDisconnected(nos::Name pinName) override
    {
        nosEngine.LogI("TestNode: OnPinDisconnected");
    }

    virtual void OnPinShowAsChanged(nos::Name pinName, nos::fb::ShowAs showAs) override
    {
        nosEngine.LogI("TestNode: OnPinShowAsChanged");
    }

    virtual void OnPathCommand(const nosPathCommand* command) override
    {
        nosEngine.LogI("TestNode: OnPathCommand");
    }

    virtual nosResult CanRemoveOrphanPin(nos::Name pinName, nosUUID pinId) override
    {
        nosEngine.LogI("TestNode: CanRemoveOrphanPin");
        return NOS_RESULT_SUCCESS;
    }

    virtual nosResult OnOrphanPinRemoved(nos::Name pinName, nosUUID pinId) override
    {
        nosEngine.LogI("TestNode: OnOrphanPinRemoved");
        return NOS_RESULT_SUCCESS;
    }

	// Execution
	virtual nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nosEngine.LogI("TestNode: ExecuteNode");
		return NOS_RESULT_SUCCESS;
	}
	virtual nosResult CopyFrom(nosCopyInfo* copyInfo) override
	{
		nosEngine.LogI("TestNode:  CopyFrom");
		return NOS_RESULT_SUCCESS;
	}
	virtual nosResult CopyTo(nosCopyInfo* copyInfo) override
	{
		nosEngine.LogI("TestNode:  CopyTo");
		return NOS_RESULT_SUCCESS;
	}

    // Menu & key events
    virtual void OnMenuRequested(const nosContextMenuRequest* request) override
    {
        nosEngine.LogI("TestNode: OnMenuRequested");
    }

    virtual void OnMenuCommand(nosUUID itemID, uint32_t cmd) override
    {
        nosEngine.LogI("TestNode: OnMenuCommand");
    }

    virtual void OnKeyEvent(const nosKeyEvent* keyEvent) override
    {
        nosEngine.LogI("TestNode: OnKeyEvent");
    }

    virtual void OnPinDirtied(nosUUID pinID, uint64_t frameCount) override
    {
        nosEngine.LogI("TestNode: OnPinDirtied");
    }

    virtual void OnPathStateChanged(nosPathState pathState) override
    {
        nosEngine.LogI("TestNode: OnPathStateChanged");
    }

	static nosResult TestFunction(void* ctx, nosFunctionExecuteParams* params)
	{
		auto args = nos::GetPinValues(params->FunctionNodeExecuteParams);

		auto a = *GetPinValue<double>(args, NSN_in1);
		auto b = *GetPinValue<double>(args, NSN_in2);
		auto c = a + b;
		nosEngine.SetPinValue(params->FunctionNodeExecuteParams->Pins[2].Id, { .Data = &c, .Size = sizeof(c) });
		return NOS_RESULT_SUCCESS;
	}

    static nosResult GetFunctions(size_t* outCount, nosName* pName, nosPfnNodeFunctionExecute* fns)
    {
        *outCount = 1;
        if (!pName || !fns)
            return NOS_RESULT_SUCCESS;

        *fns = TestFunction;
        *pName = NOS_NAME_STATIC("TestFunction");
        return NOS_RESULT_SUCCESS;
    }
};


nosResult RegisterFrameInterpolator(nosNodeFunctions* nodeFunctions);

struct TestPluginFunctions : PluginFunctions
{
	nosResult ExportNodeFunctions(size_t& outCount, nosNodeFunctions** outFunctions) override
	{
		#ifdef _WIN32
		outCount = 11;
		#else
		outCount = 10;
		#endif
		if (!outFunctions)
			return NOS_RESULT_SUCCESS;

		nosModuleStatusMessage msg;
		msg.ModuleId = nosEngine.Module->Id;
		msg.Message = "Test module loaded";
		msg.MessageType = NOS_MODULE_STATUS_MESSAGE_TYPE_INFO;
		msg.UpdateType = NOS_MODULE_STATUS_MESSAGE_UPDATE_TYPE_REPLACE;
		nosEngine.SendModuleStatusMessageUpdate(&msg);
		int index = 0;
		NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.test.NodeTest"), TestNode, outFunctions[index++]);
		outFunctions[index++]->ClassName = NOS_NAME_STATIC("nos.test.NodeWithCategories");
		outFunctions[index]->ClassName = NOS_NAME_STATIC("nos.test.NodeWithFunctions");
		outFunctions[index++]->GetFunctions = [](size_t* outCount, nosName* pName, nosPfnNodeFunctionExecute* fns)
			{
				*outCount = 1;
				if (!pName || !fns)
					return NOS_RESULT_SUCCESS;

				fns[0] = [](void* ctx, nosFunctionExecuteParams* params)
					{
						NodeExecuteParams execParams(params->FunctionNodeExecuteParams);

						nosEngine.LogI("NodeWithFunctions: TestFunction executed");

						double res = *InterpretPinValue<double>(execParams[NOS_NAME("in1")].Data->Data) + *InterpretPinValue<double>(execParams[NOS_NAME("in2")].Data->Data);

						nosEngine.SetPinValue(execParams[NOS_NAME("out")].Id, nos::Buffer::From(res));
						nosEngine.SetPinDirty(execParams[NOS_NAME("OutTrigger")].Id);
						return NOS_RESULT_SUCCESS;
					};
				pName[0] = NOS_NAME_STATIC("TestFunction");
				return NOS_RESULT_SUCCESS;
			};



		outFunctions[index++]->ClassName = NOS_NAME_STATIC("nos.test.NodeWithCustomTypes");
		outFunctions[index]->ClassName = NOS_NAME_STATIC("nos.test.CopyTest");
		outFunctions[index++]->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params)
			{
				nosCmd cmd;
				nosVulkan->Begin("(nos.test.CopyTest) Copy", &cmd);
				auto values = nos::GetPinValues(params);
				nosResourceShareInfo input = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Input")]);
				nosResourceShareInfo output = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Output")]);
				nosVulkan->Copy(cmd, &input, &output, 0);
				nosVulkan->End(cmd, NOS_FALSE);
				return NOS_RESULT_SUCCESS;
			};

		outFunctions[index]->ClassName = NOS_NAME_STATIC("nos.test.CopyTestLicensed");
		outFunctions[index]->OnNodeCreated = [](const nosFbNode* node, void** outCtxPtr) {
			nosEngine.RegisterFeature(*node->id(), "Nodos.CopyTestLicensed", 1, "Nodos.CopyTestLicensed required");
			};
		outFunctions[index]->OnNodeDeleted = [](void* ctx, nosUUID nodeId) {
			nosEngine.UnregisterFeature(nodeId, "Nodos.CopyTestLicensed");
			};
		outFunctions[index++]->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params)
			{
				nosCmd cmd;
				nosVulkan->Begin("(nos.test.CopyTest) Copy", &cmd);
				auto values = nos::GetPinValues(params);
				nosResourceShareInfo input = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Input")]);
				nosResourceShareInfo output = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Output")]);
				nosVulkan->Copy(cmd, &input, &output, 0);
				nosVulkan->End(cmd, nullptr);
				return NOS_RESULT_SUCCESS;
			};
		outFunctions[index]->ClassName = NOS_NAME_STATIC("nos.test.CopyBuffer");
		outFunctions[index++]->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params) {
			auto inBuf = nos::GetPinValue<sys::vulkan::Buffer>(nos::GetPinValues(params), NOS_NAME_STATIC("Input"));
			auto outBuf = nos::GetPinValue<sys::vulkan::Buffer>(nos::GetPinValues(params), NOS_NAME_STATIC("Output"));
			auto in = vkss::ConvertToResourceInfo(*inBuf);
			if (in.Memory.Handle == 0)
				return NOS_RESULT_INVALID_ARGUMENT;
			auto out = vkss::ConvertToResourceInfo(*outBuf);
			if (out.Info.Buffer.Size != in.Info.Buffer.Size)
			{
				out = in;
				nosVulkan->CreateResource(&out);
				out.Info.Buffer.Usage = nosBufferUsage(out.Info.Buffer.Usage | NOS_BUFFER_USAGE_TRANSFER_DST);
				auto newBuf = nos::Buffer::From(vkss::ConvertBufferInfo(out));
				nosEngine.SetPinValue(params->Pins[1].Id, newBuf);
			}
			nosCmd cmd{};
			nosVulkan->Begin("(nos.test.CopyBuffer) Copy", &cmd);
			nosVulkan->Copy(cmd, &in, &out, 0);
			nosVulkan->End(cmd, nullptr);
			return NOS_RESULT_SUCCESS;
			};
		RegisterFrameInterpolator(outFunctions[index++]);
		#ifdef _WIN32
		nos::test::RegisterWindowNode(outFunctions[index++]);
		#endif
		outFunctions[index]->ClassName = NOS_NAME_STATIC("nos.test.BypassTexture");
		outFunctions[index++]->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params)
			{
				auto values = nos::GetPinValues(params);
				nos::sys::vulkan::TTexture in, out;
				auto intex = flatbuffers::GetRoot<nos::sys::vulkan::Texture>(values[NOS_NAME_STATIC("Input")]);
				intex->UnPackTo(&in);
				out = in;
				out.unmanaged = true;
				auto ids = nos::GetPinIds(params);
				nosEngine.SetPinValue(ids[NOS_NAME_STATIC("Output")], nos::Buffer::From(out));
				return NOS_RESULT_SUCCESS;
			};
		outFunctions[index]->ClassName = NOS_NAME_STATIC("nos.test.LiveOutWithInput");
		outFunctions[index++]->CopyFrom = [](void* ctx, nosCopyInfo* copyInfo)
			{
				nosEngine.LogD("LiveOutWithInput: CopyFrom");
				return NOS_RESULT_SUCCESS;
			};
		return NOS_RESULT_SUCCESS;
	}
};

NOS_EXPORT_PLUGIN_FUNCTIONS(TestPluginFunctions)

} // namespace nos::test
