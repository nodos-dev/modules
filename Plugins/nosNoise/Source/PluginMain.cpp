// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

// Includes
#include <Nodos/PluginAPI.h>
#include <Nodos/PluginHelpers.hpp>

// Subsystem dependencies
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>

NOS_INIT();
NOS_VULKAN_INIT();

namespace nos::noise
{
enum Nodes : int
{
    SimplexNoise = 0,
    WorleyNoise = 1,
    Count
};

class DynamicSizedNoiseNode : public NodeContext
{
public:
    DynamicSizedNoiseNode(const nosFbNode* node) : NodeContext(node)
    {
    }

    nosResult ExecuteNode(const nosNodeExecuteArgs* args) override
    {
        auto pins = GetPinValues(args);
        auto outTex = InterpretPinValue<sys::vulkan::Texture>(pins[NOS_NAME("Output")]);
        auto& res = *InterpretPinValue<const fb::vec2u>(pins[NOS_NAME("Resolution")]);
        if (res.x() != outTex->width() || res.y() != outTex->height())
        {
            sys::vulkan::TTexture newTex;
            outTex->UnPackTo(&newTex);
            newTex.handle = 0;
            newTex.external_memory = {};
            newTex.width = res.x();
            newTex.height = res.y();
            nosEngine.LogD("Resizing texture to %dx%d", newTex.width, newTex.height);
            nosEngine.SetPinValueByName(NodeId, NOS_NAME("Output"), nos::Buffer::From(newTex));;
        }
        return nosVulkan->ExecuteGPUNode(this, args);
    }

};

void RegisterSimplexNoise(nosNodeFunctions* node)
{
}

extern "C"
{

NOSAPI_ATTR nosResult NOSAPI_CALL nosExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
    if (!outList)
    {
        *outSize = Nodes::Count;
        return NOS_RESULT_SUCCESS;
    }

    auto ret = RequestVulkanSubsystem();
    if (ret != NOS_RESULT_SUCCESS)
    {
        nosEngine.LogE("Failed to get Vulkan subsystem");
        return ret;
    }
    
    NOS_BIND_NODE_CLASS(NOS_NAME("SimplexNoise"), DynamicSizedNoiseNode, outList[SimplexNoise])
    NOS_BIND_NODE_CLASS(NOS_NAME("WorleyNoise"), DynamicSizedNoiseNode, outList[WorleyNoise]);
    return NOS_RESULT_SUCCESS;
}
}
}
