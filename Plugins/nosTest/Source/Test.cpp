// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginAPI.h>

#include <Builtins_generated.h>

#include <Nodos/PluginHelpers.hpp>

#include <nosVulkanSubsystem/nosVulkanSubsystem.h>
#include <nosVulkanSubsystem/Helpers.hpp>

#include "Window/WindowNode.h"

NOS_INIT_WITH_MIN_REQUIRED_MINOR(12) // Do not forget to remove this minimum required minor version on major version
									// changes, or we might not be loaded.
NOS_REGISTER_NAME(in1)
NOS_REGISTER_NAME(in2)
NOS_REGISTER_NAME(out)

NOS_VULKAN_INIT();

namespace nos::test
{


class TestNode : public nos::NodeContext
{
public:
	TestNode(const nosFbNode* node) : nos::NodeContext(node)
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
		AddPinValueWatcher(NOS_NAME_STATIC("double_prop"), [this](nos::Buffer const& newVal, std::optional<nos::Buffer> oldVal) {
			double optOldVal = 0.0f;
			if (oldVal)
				optOldVal = *oldVal->As<double>();
			nosEngine.LogI("TestNode: double_prop changed to %f from %f", *newVal.As<double>(), optOldVal);
			});
	}
	~TestNode()
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
	}
	void OnNodeUpdated(const nosFbNode* updatedNode) override { nosEngine.LogI("TestNode: " __FUNCTION__); }
	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
	}
	virtual void OnPinConnected(nos::Name pinName, nosUUID connectedPin) override { nosEngine.LogI("TestNode: " __FUNCTION__); }
	virtual void OnPinDisconnected(nos::Name pinName) override { nosEngine.LogI("TestNode: " __FUNCTION__); }
	virtual void OnPinShowAsChanged(nos::Name pinName, nos::fb::ShowAs showAs) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
	}
	virtual void OnPathCommand(const nosPathCommand* command) override { nosEngine.LogI("TestNode: OnNodeUpdated"); }
	virtual nosResult CanRemoveOrphanPin(nos::Name pinName, nosUUID pinId) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
		return NOS_RESULT_SUCCESS;
	}
	virtual nosResult OnOrphanPinRemoved(nos::Name pinName, nosUUID pinId) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
		return NOS_RESULT_SUCCESS;
	}

	// Execution
	virtual nosResult ExecuteNode(const nosNodeExecuteArgs* args) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
		return NOS_RESULT_SUCCESS;
	}
	virtual nosResult CopyFrom(nosCopyInfo* copyInfo) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
		return NOS_RESULT_SUCCESS;
	}
	virtual nosResult CopyTo(nosCopyInfo* copyInfo) override
	{
		nosEngine.LogI("TestNode: " __FUNCTION__);
		return NOS_RESULT_SUCCESS;
	}

	// Menu & key events
	virtual void OnMenuRequested(const nosContextMenuRequest* request) override { nosEngine.LogI("TestNode: " __FUNCTION__); }
	virtual void OnMenuCommand(nosUUID itemID, uint32_t cmd) override { nosEngine.LogI("TestNode: " __FUNCTION__); }
	virtual void OnKeyEvent(const nosKeyEvent* keyEvent) override { nosEngine.LogI("TestNode: " __FUNCTION__); }

	virtual void OnPinDirtied(nosUUID pinID, uint64_t frameCount) override { nosEngine.LogI("TestNode: " __FUNCTION__); }
	virtual void OnPathStateChanged(nosPathState pathState) override { nosEngine.LogI("TestNode: " __FUNCTION__); }


	static void TestFunction(void* ctx, const nosNodeExecuteArgs* nodeArgs, const nosNodeExecuteArgs* functionArgs)
	{
		auto args = nos::GetPinValues(functionArgs);

		auto a = *GetPinValue<double>(args, NSN_in1);
		auto b = *GetPinValue<double>(args, NSN_in2);
		auto c = a + b;
		nosEngine.SetPinValue(functionArgs->Pins[2].Id, { .Data = &c, .Size = sizeof(c) });
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

extern "C"
{


	NOSAPI_ATTR nosResult NOSAPI_CALL nosExportNodeFunctions(size_t* outCount, nosNodeFunctions** outFunctions)
	{
		*outCount = (size_t)(11);
		if (!outFunctions)
			return NOS_RESULT_SUCCESS;

		nosModuleStatusMessage msg;
		msg.ModuleId = nosEngine.Context->Id;
		msg.Message = "Test module loaded";
		msg.MessageType = NOS_MODULE_STATUS_MESSAGE_TYPE_INFO;
		msg.UpdateType = NOS_MODULE_STATUS_MESSAGE_UPDATE_TYPE_REPLACE;
		nosEngine.SendModuleStatusMessageUpdate(&msg);

		auto ret = RequestVulkanSubsystem();
		if (ret != NOS_RESULT_SUCCESS)
			return ret;
		
		NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.test.NodeTest"), TestNode, outFunctions[0]);
		outFunctions[1]->ClassName = NOS_NAME_STATIC("nos.test.NodeWithCategories");
		outFunctions[2]->ClassName = NOS_NAME_STATIC("nos.test.NodeWithFunctions");
		outFunctions[2]->GetFunctions = [](size_t* outCount, nosName* pName, nosPfnNodeFunctionExecute* fns)
			{
			*outCount = 1;
			if (!pName || !fns)
				return NOS_RESULT_SUCCESS;

			fns[0] = [](void* ctx, const nosNodeExecuteArgs* nodeArgs, const nosNodeExecuteArgs* functionArgs)
				{
					NodeExecuteArgs args(functionArgs);

					nosEngine.LogI("NodeWithFunctions: TestFunction executed");

					double res = *InterpretPinValue<double>(args[NOS_NAME("in1")].Data->Data) + *InterpretPinValue<double>(args[NOS_NAME("in2")].Data->Data);

					nosEngine.SetPinValue(args[NOS_NAME("out")].Id, nos::Buffer::From(res));
					nosEngine.SetPinDirty(args[NOS_NAME("OutTrigger")].Id);
				};
			pName[0] = NOS_NAME_STATIC("TestFunction");
			return NOS_RESULT_SUCCESS;
		};



		outFunctions[3]->ClassName = NOS_NAME_STATIC("nos.test.NodeWithCustomTypes");
		outFunctions[4]->ClassName = NOS_NAME_STATIC("nos.test.CopyTest");
		outFunctions[4]->ExecuteNode = [](void* ctx, const nosNodeExecuteArgs* args)
		{
			nosCmd cmd;
			nosVulkan->Begin("(nos.test.CopyTest) Copy", &cmd);
			auto values = nos::GetPinValues(args);
			nosResourceShareInfo input = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Input")]);
			nosResourceShareInfo output = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Output")]);
			nosVulkan->Copy(cmd, &input, &output, 0);
			nosVulkan->End(cmd, NOS_FALSE);
			return NOS_RESULT_SUCCESS;
		};
		
		outFunctions[5]->ClassName = NOS_NAME_STATIC("nos.test.CopyTestLicensed");
		outFunctions[5]->OnNodeCreated = [] (const nosFbNode* node, void** outCtxPtr) {
			nosEngine.RegisterFeature(*node->id(), "Nodos.CopyTestLicensed", 1, "Nodos.CopyTestLicensed required");	
		};
		outFunctions[5]->OnNodeDeleted = [] (void* ctx, nosUUID nodeId) {
			nosEngine.UnregisterFeature(nodeId, "Nodos.CopyTestLicensed");
		};
		outFunctions[5]->ExecuteNode = [](void* ctx, const nosNodeExecuteArgs* args)
		{
			nosCmd cmd;
			nosVulkan->Begin("(nos.test.CopyTest) Copy", &cmd);
			auto values = nos::GetPinValues(args);
			nosResourceShareInfo input = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Input")]);
			nosResourceShareInfo output = nos::vkss::DeserializeTextureInfo(values[NOS_NAME_STATIC("Output")]);
			nosVulkan->Copy(cmd, &input, &output, 0);
			nosVulkan->End(cmd, nullptr);
			return NOS_RESULT_SUCCESS;
		};
		outFunctions[6]->ClassName = NOS_NAME_STATIC("nos.test.CopyBuffer");
		outFunctions[6]->ExecuteNode = [](void* ctx, const nosNodeExecuteArgs* args) {
			auto inBuf = nos::GetPinValue<sys::vulkan::Buffer>(nos::GetPinValues(args), NOS_NAME_STATIC("Input"));
			auto outBuf = nos::GetPinValue<sys::vulkan::Buffer>(nos::GetPinValues(args), NOS_NAME_STATIC("Output"));
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
				nosEngine.SetPinValue(args->Pins[1].Id, newBuf);
			}
			nosCmd cmd{};
			nosVulkan->Begin("(nos.test.CopyBuffer) Copy", &cmd);
			nosVulkan->Copy(cmd, &in, &out, 0);
			nosVulkan->End(cmd, nullptr);
			return NOS_RESULT_SUCCESS;
		};
		RegisterFrameInterpolator(outFunctions[7]);
		nos::test::RegisterWindowNode(outFunctions[8]);
		outFunctions[9]->ClassName = NOS_NAME_STATIC("nos.test.BypassTexture");
		outFunctions[9]->ExecuteNode = [](void* ctx, const nosNodeExecuteArgs* args)
		{
			auto values = nos::GetPinValues(args);
			nos::sys::vulkan::TTexture in, out;
			auto intex = flatbuffers::GetRoot<nos::sys::vulkan::Texture>(values[NOS_NAME_STATIC("Input")]);
			intex->UnPackTo(&in);
			out = in;
			out.unmanaged = true;
			auto ids = nos::GetPinIds(args);
			nosEngine.SetPinValue(ids[NOS_NAME_STATIC("Output")], nos::Buffer::From(out));
			return NOS_RESULT_SUCCESS;
		};
		outFunctions[10]->ClassName = NOS_NAME_STATIC("nos.test.LiveOutWithInput");
		outFunctions[10]->CopyFrom = [](void* ctx, nosCopyInfo* copyInfo)
			{
				nosEngine.LogD("LiveOutWithInput: CopyFrom");
				return NOS_RESULT_SUCCESS;
			};
		return NOS_RESULT_SUCCESS;
	}

}
} // namespace nos::test
