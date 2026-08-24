// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/Plugin.hpp>
#include <nosSysVulkan/Helpers.hpp>
#include <glm/glm.hpp>
#include <random>
#include "nosCompositing/Layout_generated.h"

NOS_REGISTER_NAME(LayoutDrawer)
NOS_REGISTER_NAME(TexturedQuad_Pass)
NOS_REGISTER_NAME(TexturedQuad_Frag)
NOS_REGISTER_NAME(TexturedQuad_Vert)
NOS_REGISTER_NAME(QuadOutline_Pass)
NOS_REGISTER_NAME(QuadOutline_Frag)
NOS_REGISTER_NAME(QuadOutline_Vert)
NOS_REGISTER_NAME(OutputTextures)
NOS_REGISTER_NAME(Preview)
NOS_REGISTER_NAME(PreviewEnabled)
NOS_REGISTER_NAME(LayoutDrawItems)
NOS_REGISTER_NAME(LayoutOutputInfos)
NOS_REGISTER_NAME(InputTextures)
NOS_REGISTER_NAME(Offset)
NOS_REGISTER_NAME(Size)
NOS_REGISTER_NAME(Input)
NOS_REGISTER_NAME(AspectRatio)
NOS_REGISTER_NAME(OutlineWidth)
NOS_REGISTER_NAME(Color)
NOS_REGISTER_NAME_SPACED(TextureArrayType, "[nos.sys.vulkan.Texture]")

namespace nos::compositing
{

struct LayoutDrawerNode : NodeContext
{
	using TextureRef = TypedObjectRef<sys::vulkan::Texture>;

	// One output texture per LayoutOutputInfo. Entries may be invalid if creation failed, check before using.
	std::vector<TextureRef> OutTextures;
	// The array object published on the OutputTextures pin. Kept alive as long as the pin refers to it.
	ObjectRef OutTexturesArray{};

	enum class StatusType
	{
		Preview,
		InvalidInputTextures,
	};
	std::map<StatusType, fb::TNodeStatusMessage> StatusMessages;

	void SendStatusMessages()
	{
		std::vector<fb::TNodeStatusMessage> messages;
		for (auto& [type, message] : StatusMessages)
			messages.push_back(message);
		if (messages.empty())
			ClearNodeStatusMessages();
		else
			SetNodeStatusMessages(messages);
	}

	void SetStatusMessage(StatusType statusType, std::string_view message, fb::NodeStatusMessageType msgType)
	{
		if (auto it = StatusMessages.find(statusType); it != StatusMessages.end())
		{
			if (it->second.text == message && it->second.type == msgType)
				return;
		}
		auto& msg = StatusMessages[statusType] = fb::TNodeStatusMessage{};
		msg.text = message;
		msg.type = msgType;
		SendStatusMessages();
	}

	void ClearStatusMessage(StatusType statusType)
	{
		auto it = StatusMessages.find(statusType);
		if (it == StatusMessages.end())
			return;
		StatusMessages.erase(statusType);
		SendStatusMessages();
	}

	nosResult OnCreate(nosFbNodePtr node) override
	{
		AddPinValueWatcher(NSN_PreviewEnabled,
						   [this](nosImmutableBuffer newVal, std::optional<nosImmutableBuffer> oldValue) {
							   bool previewEnabled = newVal.Data && *static_cast<const bool*>(newVal.Data);
							   SetPinOrphanState(NSN_Preview,
												 previewEnabled ? fb::PinOrphanStateType::ACTIVE
																: fb::PinOrphanStateType::ORPHAN,
												 "Preview disabled.");
							   if (previewEnabled)
								   SetStatusMessage(
									   StatusType::Preview, "Preview enabled.", fb::NodeStatusMessageType::INFO);
							   else
								   ClearStatusMessage(StatusType::Preview);
						   });
		ClearNodeStatusMessages();
		return NOS_RESULT_SUCCESS;
	}

	void UpdateOutputTexturesPin()
	{
		std::vector<nosObjectId> elements;
		elements.reserve(OutTextures.size());
		for (auto const& tex : OutTextures)
			if (tex.IsValid())
				elements.push_back(tex);

		ObjectRef newArray{};
		if (nosEngine.ObjectAPI->CreateArrayObject(
				NSN_TextureArrayType, elements.data(), elements.size(), &newArray.GetStorage()) != NOS_RESULT_SUCCESS)
		{
			nosEngine.LogE("LayoutDrawer: Failed to create output texture array object");
			return;
		}
		OutTexturesArray = std::move(newArray);
		SetPinObject(NSN_OutputTextures, OutTexturesArray);
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		auto const* drawItemsPtr = params.GetPinValue<flatbuffers::Vector<LayoutDrawItem const*>>(NSN_LayoutDrawItems);
		auto const* outputInfosPtr =
			params.GetPinValue<flatbuffers::Vector<LayoutOutputInfo const*>>(NSN_LayoutOutputInfos);
		if (!drawItemsPtr || !outputInfosPtr)
			return NOS_RESULT_SUCCESS;
		auto const& drawItems = *drawItemsPtr;
		auto const& outputInfos = *outputInfosPtr;

		// Keep the element references alive for the duration of the frame.
		auto inTexArray = params.GetPinObject<ArrayObjectRef>(NSN_InputTextures);
		size_t inTextureCount = inTexArray.GetSize();
		std::vector<ObjectRef> inTextures;
		inTextures.reserve(inTextureCount);
		for (size_t i = 0; i < inTextureCount; ++i)
			if (auto elem = inTexArray.GetElement(i))
				inTextures.push_back(std::move(*elem));

		nosTextureFilter inputFilter = NOS_TEXTURE_FILTER_LINEAR;
		nosVulkan->GetPinTextureFilter(params[NSN_InputTextures].Id, &inputFilter);

		bool outsChanged = OutTextures.size() != outputInfos.size();
		OutTextures.resize(outputInfos.size());
		for (size_t i = 0; i < outputInfos.size(); ++i)
		{
			auto const& outInfo = *outputInfos[i];
			auto info = sys::vulkan::GetResourceInfo(OutTextures[i]);
			if (info && info->Width == outInfo.resolution().x() && info->Height == outInfo.resolution().y())
				continue;
			OutTextures[i] = sys::vulkan::CreateTexture(
				{
					.Width = outInfo.resolution().x(),
					.Height = outInfo.resolution().y(),
					.Format = NOS_FORMAT_R16G16B16A16_UNORM,
					.Usage = nosImageUsage(NOS_IMAGE_USAGE_SAMPLED | NOS_IMAGE_USAGE_RENDER_TARGET |
										   NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST),
				},
				"LayoutDrawerOut");
			if (!OutTextures[i].IsValid())
				nosEngine.LogE("LayoutDrawer: Failed to create output texture for output %zu!", i);
			outsChanged = true;
		}
		if (outsChanged)
			UpdateOutputTexturesPin();

		bool unmatchedInputTextures = false;
		for (auto const* drawItem : drawItems)
		{
			if (drawItem->texture_id() >= inTextures.size())
			{
				unmatchedInputTextures = true;
				break;
			}
		}
		if (unmatchedInputTextures)
			SetStatusMessage(StatusType::InvalidInputTextures,
							 "Layout contains draw items referencing invalid input textures",
							 fb::NodeStatusMessageType::FAILURE);
		else
			ClearStatusMessage(StatusType::InvalidInputTextures);

		auto cmd = sys::vulkan::BeginCmd(NSN_LayoutDrawer, NodeId);
		for (size_t outIndex = 0; outIndex < outputInfos.size(); outIndex++)
		{
			auto const& outInfo = *outputInfos[outIndex];
			auto const& outTex = OutTextures[outIndex];
			if (!outTex.IsValid())
				continue;
			nosVulkan->Clear(cmd, outTex, nosVec4{0.0f, 0.0f, 0.0f, 1.0f});

			// Map the canvas space draw items into this output's own [0, 1] space.
			glm::vec2 translation = {outInfo.pos().x(), outInfo.pos().y()};
			glm::vec2 scale = {outInfo.size().x(), outInfo.size().y()};
			if (scale.x == 0.0f || scale.y == 0.0f)
				continue;

			std::vector<LayoutDrawItem> drawItemsForOut;
			drawItemsForOut.reserve(drawItems.size());
			for (auto const* drawItem : drawItems)
			{
				auto drawItemPos = drawItem->position();
				auto drawItemSize = drawItem->size();
				glm::vec2 pos = {drawItemPos.x(), drawItemPos.y()};
				glm::vec2 endPos = {drawItemPos.x() + drawItemSize.x(), drawItemPos.y() + drawItemSize.y()};

				glm::vec2 transformedPos = (pos - translation) / scale;
				glm::vec2 transformedEndPos = (endPos - translation) / scale;

				LayoutDrawItem newItem{};
				newItem.mutable_position() = nos::fb::vec2(transformedPos.x, transformedPos.y);
				newItem.mutable_size() =
					nos::fb::vec2(transformedEndPos.x - transformedPos.x, transformedEndPos.y - transformedPos.y);
				newItem.mutate_texture_id(drawItem->texture_id());
				drawItemsForOut.emplace_back(newItem);
			}
			DrawOut(cmd, inTextures, inputFilter, drawItemsForOut, outTex);
		}

		if (*params.GetPinValue<bool>(NSN_PreviewEnabled))
		{
			auto preview = params.GetPinObject<sys::vulkan::Texture>(NSN_Preview);
			if (preview.IsValid())
			{
				nosVulkan->Clear(cmd, preview, nosVec4{0.0f, 0.0f, 0.0f, 1.0f});
				std::vector<LayoutDrawItem> drawItemsForPreview;
				drawItemsForPreview.reserve(drawItems.size());
				for (auto const* drawItem : drawItems)
					drawItemsForPreview.emplace_back(*drawItem);
				DrawOut(cmd, inTextures, inputFilter, drawItemsForPreview, preview);

				std::vector<LayoutOutputInfo> outlinesForPreview;
				outlinesForPreview.reserve(outputInfos.size());
				for (auto const* output : outputInfos)
					outlinesForPreview.emplace_back(*output);
				DrawOutlines(cmd, outlinesForPreview, preview);
			}
		}

		sys::vulkan::EndCmd(cmd, false, nullptr);
		return NOS_RESULT_SUCCESS;
	}

	void DrawOut(nosCmd cmd,
				 std::span<const ObjectRef> textures,
				 nosTextureFilter filter,
				 std::span<LayoutDrawItem> drawList,
				 nosTextureObject output)
	{
		for (auto const& item : drawList)
		{
			if (item.texture_id() >= textures.size())
				continue;
			auto const& inputTex = textures[item.texture_id()];
			if (!inputTex.IsValid())
				continue;
			auto pos = item.position();
			auto size = item.size();

			std::array bindings = {sys::vulkan::ShaderDataBinding(NSN_Offset, pos),
								   sys::vulkan::ShaderDataBinding(NSN_Size, size),
								   sys::vulkan::ShaderTextureBinding(NSN_Input, inputTex, filter)};

			nosVertexData vertexData = {
				.DepthFunc = NOS_DEPTH_FUNCTION_ALWAYS,
				.DepthWrite = NOS_FALSE,
				.DepthTest = NOS_FALSE,
			};
			nosRunPassParams runPassParams{.Key = NSN_TexturedQuad_Pass,
										   .Bindings = bindings.data(),
										   .BindingCount = static_cast<uint32_t>(bindings.size()),
										   .Output = output,
										   .Vertices = vertexData,
										   .Wireframe = NOS_FALSE,
										   .Benchmark = NOS_FALSE,
										   .DoNotClear = true};
			nosVulkan->RunPass(cmd, &runPassParams);
		}
	}

	void DrawOutlines(nosCmd cmd, std::span<LayoutOutputInfo> outputInfos, TextureRef const& output)
	{
		auto outputResInfo = sys::vulkan::GetResourceInfo(output);
		if (!outputResInfo || outputResInfo->Width == 0 || outputResInfo->Height == 0)
			return;

		// Deterministic colors: the same outline keeps the same color across frames.
		std::mt19937_64 rng{};
		std::uniform_real_distribution<float> dist(0.0f, 1.0f);

		for (auto const& item : outputInfos)
		{
			auto pos = item.pos();
			auto size = item.size();
			float aspectRatio = outputResInfo->Width / (float)outputResInfo->Height;
			float outlineWidth = 20.0f / (float)outputResInfo->Width;

			glm::vec4 color = {dist(rng), dist(rng), dist(rng), 1.0f};

			std::array bindings = {
				sys::vulkan::ShaderDataBinding(NSN_Offset, pos),
				sys::vulkan::ShaderDataBinding(NSN_Size, size),
				sys::vulkan::ShaderDataBinding(NSN_AspectRatio, aspectRatio),
				sys::vulkan::ShaderDataBinding(NSN_OutlineWidth, outlineWidth),
				sys::vulkan::ShaderDataBinding(NSN_Color, color),
			};

			nosVertexData vertexData = {
				.DepthFunc = NOS_DEPTH_FUNCTION_ALWAYS,
				.DepthWrite = NOS_FALSE,
				.DepthTest = NOS_FALSE,
			};
			nosRunPassParams runPassParams{.Key = NSN_QuadOutline_Pass,
										   .Bindings = bindings.data(),
										   .BindingCount = static_cast<uint32_t>(bindings.size()),
										   .Output = output,
										   .Vertices = vertexData,
										   .Wireframe = NOS_FALSE,
										   .Benchmark = NOS_FALSE,
										   .DoNotClear = true};
			nosVulkan->RunPass(cmd, &runPassParams);
		}
	}
};

nosResult RegisterLayoutDrawer(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_LayoutDrawer, LayoutDrawerNode, fn);

	fs::path root = nosEngine.Plugin->RootFolderPath;
	auto fragPath = (root / "Shaders" / "TexturedQuad.frag").generic_string();
	auto vertPath = (root / "Shaders" / "TexturedQuad.vert").generic_string();
	auto outlineFragPath = (root / "Shaders" / "QuadOutline.frag").generic_string();
	auto outlineVertPath = (root / "Shaders" / "QuadOutline.vert").generic_string();

	// Register shaders
	std::array shaders = {
		nosShaderInfo{.ShaderName = NSN_TexturedQuad_Frag,
					  .Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = fragPath.c_str()},
					  .AssociatedNodeClassName = NSN_LayoutDrawer},
		nosShaderInfo{.ShaderName = NSN_TexturedQuad_Vert,
					  .Source = {.Stage = NOS_SHADER_STAGE_VERT, .GLSLPath = vertPath.c_str()},
					  .AssociatedNodeClassName = NSN_LayoutDrawer},
		nosShaderInfo{.ShaderName = NSN_QuadOutline_Frag,
					  .Source = {.Stage = NOS_SHADER_STAGE_FRAG, .GLSLPath = outlineFragPath.c_str()},
					  .AssociatedNodeClassName = NSN_LayoutDrawer},

		nosShaderInfo{.ShaderName = NSN_QuadOutline_Vert,
					  .Source = {.Stage = NOS_SHADER_STAGE_VERT, .GLSLPath = outlineVertPath.c_str()},
					  .AssociatedNodeClassName = NSN_LayoutDrawer},
	};
	auto ret = nosVulkan->RegisterShaders(shaders.size(), shaders.data());
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	std::array passes = {
		nosPassInfo{
			.Key = NSN_TexturedQuad_Pass,
			.Shader = NSN_TexturedQuad_Frag,
			.VertexShader = NSN_TexturedQuad_Vert,
			.MultiSample = 1,
		},
		nosPassInfo{
			.Key = NSN_QuadOutline_Pass,
			.Shader = NSN_QuadOutline_Frag,
			.VertexShader = NSN_QuadOutline_Vert,
			.MultiSample = 1,
		},
	};

	ret = nosVulkan->RegisterPasses(passes.size(), passes.data());
	return ret;
}
} // namespace nos::compositing
