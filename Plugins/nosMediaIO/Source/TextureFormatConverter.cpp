#include <Nodos/PluginHelpers.hpp>
#include <nosVulkanSubsystem/Helpers.hpp>
#include <nosVulkanSubsystem/Types_generated.h>

namespace nos::MediaIO
{
NOS_REGISTER_NAME(Input)
NOS_REGISTER_NAME(Output)
NOS_REGISTER_NAME(OutputFormat)
NOS_REGISTER_NAME(TextureFormatConverter)
NOS_REGISTER_NAME(InputTexture)
NOS_REGISTER_NAME(FloatToIntFormat)
NOS_REGISTER_NAME(FloatToIntFormat_Pass)
NOS_REGISTER_NAME(DST_TEXTURE_UINT32)
NOS_REGISTER_NAME(DST_TEXTURE_UINT16)
NOS_REGISTER_NAME(DST_TEXTURE_UINT8)
NOS_REGISTER_NAME(DST_TEXTURE_INT32)
NOS_REGISTER_NAME(DST_TEXTURE_INT16)
NOS_REGISTER_NAME(DST_TEXTURE_INT8)

std::pair<nos::Name, std::vector<uint8_t>> FloatToIntFormatShader;

inline void CreateStringList(nosUUID& GenUUID, nosUUID& NodeUUID, std::string name, std::vector<std::string> list) {
	flatbuffers::FlatBufferBuilder fbb;
	flatbuffers::FlatBufferBuilder fbb2;
	std::vector<flatbuffers::Offset<nos::fb::Pin>> StrListPin;
	nos::fb::TVisualizer vis = { .type = nos::fb::VisualizerType::COMBO_BOX, .name = name };
	auto buf = std::vector<u8>((u8*)list.front().data(), (u8*)list.front().data() + list.front().size() + 1);

	nosEngine.GenerateID(&GenUUID);

	StrListPin.push_back(nos::fb::CreatePinDirect(fbb,
		&GenUUID,
		name.c_str(),
		"string",
		nos::fb::ShowAs::PROPERTY,
		nos::fb::CanShowAs::PROPERTY_ONLY,
		0,
		nos::fb::Visualizer::Pack(fbb, &vis),
		&buf));

	HandleEvent(nos::CreateAppEvent(fbb,
		nos::CreatePartialNodeUpdateDirect(fbb, &NodeUUID, nos::ClearFlags::NONE, 0, &StrListPin)));

	HandleEvent(nos::CreateAppEvent(
		fbb2, nos::app::CreateUpdateStringList(fbb2, nos::fb::CreateStringList(fbb2, fbb2.CreateString(name), fbb2.CreateVectorOfStrings(list)))));
}

struct TextureFormatConverter : nos::NodeContext
{
	nosResourceShareInfo InputTexture = {}, OutputTexture = {};
	nosUUID NodeUUID = {}, InputUUID = {}, OutputUUID = {}, FormatUUID = {};
	nos::sys::vulkan::Format OutputFormat = {};
	nosResourceShareInfo outBuf = {};
	nosResourceShareInfo inBuf = {};

	bool IsSavedNode = false;
	TextureFormatConverter(nosFbNode const* node) : NodeContext(node)
	{
		NodeUUID = *node->id();

		for (const auto& pin : *node->pins()) {
			if (NSN_Input.Compare(pin->name()->c_str()) == 0) {
				InputUUID = *pin->id();
				InputTexture = nos::vkss::DeserializeTextureInfo((void*)pin->data()->data());
			}
			else if (NSN_Output.Compare(pin->name()->c_str()) == 0) {
				OutputUUID = *pin->id();
			}
			else if (NSN_OutputFormat.Compare(pin->name()->c_str()) == 0) {
				IsSavedNode = true;
				FormatUUID = *pin->id();
				SetPinOrphanState(FormatUUID, false);
				const char* SelectedFormat = (const char*)pin->data()->data();
				auto FormatEnums = nos::sys::vulkan::EnumValuesFormat();
				std::vector<std::string> Formats = {};
				nos::sys::vulkan::Format newFormat = {};
				for (int i = 0; i < 69; i++) {
					if (strcmp(nos::sys::vulkan::EnumNameFormat(FormatEnums[i]), SelectedFormat) == 0) {
						newFormat = FormatEnums[i];
						if (newFormat != OutputFormat) {
							OutputFormat = newFormat;
							PrepareResources();
						}
					}
					Formats.push_back(nos::sys::vulkan::EnumNameFormat(FormatEnums[i]));
				}
				nos::UpdateStringList(NSN_OutputFormat.AsString(), std::move(Formats));
			}
		}
		if (!IsSavedNode) {
			std::vector<std::string> Formats = {};
			auto FormatEnums = nos::sys::vulkan::EnumValuesFormat();
			for (int i = 0; i < 69; i++) {
				Formats.push_back(nos::sys::vulkan::EnumNameFormat(FormatEnums[i]));
			}
			CreateStringList(FormatUUID, NodeUUID, NSN_OutputFormat.AsString(), std::move(Formats));
		}
	}

	~TextureFormatConverter() {
		nosVulkan->DestroyResource(&OutputTexture);
	}

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{
		if (InputUUID == pinId) {
			InputTexture = nos::vkss::DeserializeTextureInfo(value.Data);
			PrepareResources();
		}
		if (FormatUUID == pinId) {
			const char* SelectedFormat = (const char*)value.Data;
			auto FormatEnums = nos::sys::vulkan::EnumValuesFormat();
			nos::sys::vulkan::Format newFormat = {};
			for (int i = 0; i < 69; i++) {
				if (strcmp(nos::sys::vulkan::EnumNameFormat(FormatEnums[i]), SelectedFormat) == 0) {
					newFormat = FormatEnums[i];
					if (newFormat != OutputFormat) {
						OutputFormat = newFormat;
						PrepareResources();
					}
					break;
				}
			}
		}
	}

	nosResult ExecuteNode(const nosNodeExecuteArgs* args) override
	{
		auto pinIds = nos::GetPinIds(args);
		auto pinValues = nos::GetPinValues(args);
		InputTexture = nos::vkss::DeserializeTextureInfo(pinValues[NSN_Input]);
		auto Out = nos::vkss::DeserializeTextureInfo(pinValues[NSN_Output]);
		//TODO: Also should be able to convert from INT texture to FLOAT textures
		//TODO: Also should be able to convert between signed integer and unsigned integers
		//TODO: Editor view and AJA does not expects INTEGER formats hence both the editor and ajaOut view does not show correct image.
		if (!nosVulkan->IsBlitCompatible(InputTexture.Info.Texture.Format, Out.Info.Texture.Format)) {
			struct OutputType { int outputType; };
			OutputType out = {};
			std::vector<nosShaderBinding> inputs;
			switch (Out.Info.Texture.Format) 
			{
				case NOS_FORMAT_R32G32B32A32_UINT:
				{
					out.outputType = 0;
					break;
				}
				case NOS_FORMAT_R16G16B16A16_UINT:
				{
					out.outputType = 1;
					break;
				}
				case NOS_FORMAT_R8G8B8A8_UINT:
				{
					out.outputType = 2;
					break;
				}
				case NOS_FORMAT_R32G32B32A32_SINT:
				{
					out.outputType = 3;
					break;
				}
				case NOS_FORMAT_R16G16B16A16_SINT:
				{
					out.outputType = 4;
					break;
				}
				default: 
				{
					out.outputType = -1;
					break;
				}
			}
			if (out.outputType != -1) {
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_InputTexture, InputTexture));
				inputs.emplace_back(nos::vkss::ShaderBinding<OutputType>(NSN_Output, out));

				//Validation layer does not like this but we sure that only true desired texture will be used in shader
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_DST_TEXTURE_UINT32, Out));
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_DST_TEXTURE_UINT16, Out));
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_DST_TEXTURE_UINT8, Out));
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_DST_TEXTURE_INT32, Out));
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_DST_TEXTURE_INT16, Out));
				inputs.emplace_back(nos::vkss::ShaderBinding(NSN_DST_TEXTURE_INT8, Out));

				nosCmd cmdRunPass = {};
				nosCmdBeginParams beginParams {
					.Name = NOS_NAME("Texture Format Conversion: Float to Int"),
					.AssociatedNodeId = NodeId,
					.OutCmdHandle = &cmdRunPass
				};
				nosVulkan->Begin2(&beginParams);
				nosRunComputePassParams pass = {};
				nosCmdEndParams endParams = {};
				pass.Key = NSN_FloatToIntFormat_Pass;
				pass.DispatchSize = nosVec2u(InputTexture.Info.Texture.Width / 16, InputTexture.Info.Texture.Height / 16);
				pass.Bindings = inputs.data();
				pass.BindingCount = inputs.size();
				pass.Benchmark = 0;
				nosVulkan->RunComputePass(cmdRunPass, &pass);
				nosVulkan->End(cmdRunPass, &endParams);
			}
		}
		else {
			nosCmd cmd = {};
			nosCmdBeginParams beginParams = {
				.Name = NOS_NAME("Texture Format Conversion: Blit"),
				.AssociatedNodeId = NodeId,
				.OutCmdHandle = &cmd,
			};
			nosCmdEndParams endParams = {};
			nosVulkan->Begin2(&beginParams);
			nosVulkan->Copy(cmd, &InputTexture, &Out, nullptr);
			nosVulkan->End(cmd, &endParams);
		}


		outBuf.Info.Type = NOS_RESOURCE_TYPE_BUFFER;
		outBuf.Info.Buffer.Size = Out.Memory.Size;
		outBuf.Info.Buffer.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);

		inBuf.Info.Type = NOS_RESOURCE_TYPE_BUFFER;
		inBuf.Info.Buffer.Size = Out.Memory.Size;
		inBuf.Info.Buffer.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);

		return NOS_RESULT_SUCCESS;
	}

	void PrepareResources() {
		if(OutputTexture.Info.Texture.Width == InputTexture.Info.Texture.Width && OutputTexture.Info.Texture.Height == InputTexture.Info.Texture.Height
			&& OutputTexture.Info.Texture.Format == nosFormat((int)OutputFormat) ) {
			//No change
			return;
		}
		if (OutputTexture.Memory.Handle != NULL) {
			nosVulkan->DestroyResource(&OutputTexture);
		}

		OutputTexture.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
		OutputTexture.Info.Texture.FieldType = InputTexture.Info.Texture.FieldType;
		OutputTexture.Info.Texture.Filter = InputTexture.Info.Texture.Filter;
		OutputTexture.Info.Texture.Format = nosFormat((int)OutputFormat);
		OutputTexture.Info.Texture.Height = InputTexture.Info.Texture.Height;
		OutputTexture.Info.Texture.Usage = InputTexture.Info.Texture.Usage;
		OutputTexture.Info.Texture.Width = InputTexture.Info.Texture.Width;

		nosVulkan->CreateResource(&OutputTexture);
		UpdateOutputPin();
	}

	void UpdateOutputPin() {
		auto TTexture = nos::vkss::ConvertTextureInfo(OutputTexture);
		flatbuffers::FlatBufferBuilder fbb;
		auto TextureTable = nos::sys::vulkan::Texture::Pack(fbb, &TTexture);
		fbb.Finish(TextureTable);
		nosEngine.SetPinValueDirect(OutputUUID, { .Data = fbb.GetBufferPointer(), .Size = fbb.GetSize() });
	}

	void SetPinOrphanState(nos::fb::UUID uuid, bool isOrphan) {
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<::flatbuffers::Offset<nos::PartialPinUpdate>> toUpdate;
		toUpdate.push_back(nos::CreatePartialPinUpdateDirect(fbb, &uuid, 0, nos::fb::CreateOrphanStateDirect(fbb, isOrphan)));

		flatbuffers::FlatBufferBuilder fbb2;
		HandleEvent(
			CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &NodeUUID, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, &toUpdate)));
	}
};

nosResult RegisterTextureFormatConverter(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_TextureFormatConverter, TextureFormatConverter, fn);

	std::filesystem::path root = nosEngine.Context->RootFolderPath;
	auto shaderPath = (root / "Shaders" / "FloatToInt.comp").generic_string();
	nosShaderInfo2 FloatToIntShaderInfo = {
		.Key = NSN_FloatToIntFormat,
		.Source = {.Stage = NOS_SHADER_STAGE_COMP, .GLSLPath = shaderPath.c_str()},
		.AssociatedNodeClassName = NOS_NAME("nos.MediaIO.TextureFormatConverter")
	};
	nosResult ret = nosVulkan->RegisterShaders2(1, &FloatToIntShaderInfo);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	nosPassInfo pass = { .Key = NSN_FloatToIntFormat_Pass, .Shader = NSN_FloatToIntFormat, .MultiSample = 1 };
	return nosVulkan->RegisterPasses(1, &pass);
}

}
