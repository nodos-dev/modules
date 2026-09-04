// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>
#include <glm/vec2.hpp>

#include "nosCompositing/CanvasMapper_generated.h"
#include "Names.h"

namespace nos::compositing
{
// The shader declares fixed size arrays of this length, see Shaders/CanvasMapper.frag.
constexpr uint32_t MAX_CANVAS_LAYERS = 16;

struct CanvasMapperContext : public NodeContext
{
	nosResult ExecuteNode(nos::NodeExecuteParams const& params) override
	{
		auto arrayObj = params.GetPinObject<ArrayObjectRef>(NSN_Input);
		if (!arrayObj.IsValid())
			return NOS_RESULT_SUCCESS;

		size_t layerCount = arrayObj.GetSize();
		if (0 == layerCount)
			return NOS_RESULT_SUCCESS;

		auto outputInfo = sys::vulkan::GetResourceInfo(params.GetPinObject(NSN_Output));
		if (!outputInfo || outputInfo->Type != NOS_RESOURCE_TYPE_TEXTURE)
			return NOS_RESULT_FAILED;
		auto outputSize = glm::vec2(outputInfo->Texture.Width, outputInfo->Texture.Height);
		if (0 == outputSize.x || 0 == outputSize.y)
			return NOS_RESULT_FAILED;

		auto rgss = *params.GetPinValue<bool>(NOS_NAME_STATIC("RGSS"));

		std::array<nos::fb::vec2, MAX_CANVAS_LAYERS> pos = {};
		std::array<nos::fb::vec2, MAX_CANVAS_LAYERS> sca = {};
		std::array<float, MAX_CANVAS_LAYERS> rot = {};
		std::array<nos::fb::vec2, MAX_CANVAS_LAYERS> ori = {};
		u32 ble = 0;
		std::array<float, MAX_CANVAS_LAYERS> opa = {};
		std::vector<nosTextureObject> textures;
		std::vector<nosTextureFilter> filters;
		// Keeps the layer textures alive until the pass is submitted.
		std::vector<ObjectRef> textureRefs;

		u32 last = 0;
		for (size_t i = 0; i < layerCount && last < MAX_CANVAS_LAYERS; ++i)
		{
			auto layer = arrayObj.GetElement<CompositeObjectRef>(i);
			if (!layer || !layer->IsValid())
				continue;
			auto texture = layer->GetField(NOS_NAME_STATIC("texture"));
			if (!texture || !texture->IsValid())
				continue;

			// Read fields through the object API rather than casting the array's data view to a flatbuffers vector:
			// the view is absent when the pin holds no object, and it belongs to a temporary whose guard reference
			// dies with the statement that produced it.
			auto size = layer->GetFieldValue<nos::fb::vec2u>(NOS_NAME_STATIC("size"));
			if (!size || 0 == size->x() || 0 == size->y())
				continue;
			sca[last] = nos::fb::vec2(float(size->x()) / outputSize.x, float(size->y()) / outputSize.y);

			// A layer that leaves one of these unset is drawn with the zero the arrays were initialized with.
			pos[last] = layer->GetFieldValue<nos::fb::vec2>(NOS_NAME_STATIC("position")).value_or(nos::fb::vec2());
			ori[last] = layer->GetFieldValue<nos::fb::vec2>(NOS_NAME_STATIC("origin")).value_or(nos::fb::vec2());
			rot[last] = layer->GetFieldValue<float>(NOS_NAME_STATIC("rotation")).value_or(0.f);
			opa[last] = layer->GetFieldValue<float>(NOS_NAME_STATIC("opacity")).value_or(0.f);

			u32 blendMode = layer->GetFieldValue<u32>(NOS_NAME_STATIC("blend_mode")).value_or(0);
			// The shader tests one bit per drawn layer, so index by the packed position, not the source index.
			ble |= (blendMode & 1u) << last;

			textureRefs.push_back(std::move(*texture));
			textures.push_back(textureRefs.back());
			filters.push_back(NOS_TEXTURE_FILTER_LINEAR);
			last++;
		}

		u32 count = (u32)textures.size();
		if (0 == count)
			return NOS_RESULT_SUCCESS;

		auto backgroundColor = *params.GetPinValue<nosVec4>(NOS_NAME_STATIC("BackgroundColor"));
		std::vector bindings = {
			nos::sys::vulkan::ShaderTextureArrayBinding(
				NOS_NAME_STATIC("Textures"),
				textures.data(),
				filters.data(),
				count),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("OutputSize"), outputSize),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("BackgroundColor"), backgroundColor),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("Positions"), pos),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("Scales"), sca),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("Origins"), ori),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("Rotations"), rot),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("Opacities"), opa),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("BlendModes"), ble),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("Texture_Count"), count),
			nos::sys::vulkan::ShaderDataBinding(NOS_NAME_STATIC("RGSS"), rgss),
		};

		nosCmd cmd;
		nosCmdBeginParams bp = {.Name = nos::Name("compositePass"), .AssociatedNodeId = NodeId, .OutCmdHandle = &cmd};
		nosVulkan->Begin(&bp);
		nosRunPassParams compositePass = {};
		compositePass.Key = NOS_NAME_STATIC("CANVAS_MAPPER_PASS");
		compositePass.Bindings = bindings.data();
		compositePass.BindingCount = (u32)bindings.size();
		compositePass.Output = params.GetPinObject(NSN_Output);
		nosVulkan->RunPass(cmd, &compositePass);
		nosVulkan->End(cmd, 0);
		return NOS_RESULT_SUCCESS;
	}
};

void RegisterCanvasMapper(nosNodeFunctions* nodeFunctions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("CanvasMapper"), CanvasMapperContext, nodeFunctions);
}

} // namespace nos::compositing
