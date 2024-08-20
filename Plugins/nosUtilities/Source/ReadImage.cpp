// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

// External
#include <stb_image.h>
#include <stb_image_write.h>

// Framework
#include <Builtins_generated.h>
#include <AppService_generated.h>

// nosNodes
#include <nosVulkanSubsystem/Helpers.hpp>

#include "Names.h"

#include <atomic>
#include <chrono>
#include <sstream>

using Clock = std::chrono::high_resolution_clock;

namespace nos::vkss
{
#include <nosVulkanSubsystem/CAPIStructHelpers.inl>
}

namespace nos::utilities
{
NOS_REGISTER_NAME(SRGB2Linear_Pass);
NOS_REGISTER_NAME(SRGB2Linear_Shader);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_ReadImage, "nos.utilities.ReadImage")

enum State
{
    Idle = 0,
    Loading = 1,
    Failed = 2,
};

struct ReadImageContext
{
    std::atomic<State> state;
    decltype(Clock::now()) time_started;
	fb::UUID nodeid;
	std::string load_path;


	void UpdateStatus(State newState)
	{
        if(newState == state.exchange(newState))
        {
            return;
        }

        flatbuffers::FlatBufferBuilder fbb;
        std::vector<flatbuffers::Offset<nos::fb::NodeStatusMessage>> msg;
        switch(newState)
        {
        case State::Loading:
		    time_started = Clock::now();
			msg.push_back(fb::CreateNodeStatusMessageDirect(fbb, "Loading image", fb::NodeStatusMessageType::INFO));
            break;
        case State::Idle:
        {
			std::stringstream ss;
			auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - time_started);
            ss << "Read Image: Loaded in " << dt << "\nFile: " << load_path;
            nosEngine.LogDI(load_path.c_str(), ss.str().c_str());
            break;
        }
        case State::Failed:
            msg.push_back(fb::CreateNodeStatusMessageDirect(fbb, "Failed to load image", fb::NodeStatusMessageType::FAILURE));
            break;
        }

        HandleEvent(CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &nodeid, ClearFlags::NONE, 0, 0, 0, 0, 0, 0, &msg)));
	}
};

static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns)
{
    *count = 1;
    if(!names || !fns)
        return NOS_RESULT_SUCCESS;
    
    *names = NOS_NAME_STATIC("ReadImage_Load");
    
	*fns = [](void* ctx, nosFunctionExecuteParams* params)
    {
        auto c = (ReadImageContext*)ctx;
        if(c->state == State::Loading)
        {
            nosEngine.LogE("Read Image is already loading an image.");
            return NOS_RESULT_FAILED;
        }

        c->UpdateStatus(State::Loading);

		nos::NodeExecuteParams nodeParams(params->ParentNodeExecuteParams);
		std::filesystem::path path = InterpretPinValue<const char>(nodeParams[NSN_Path].Data->Data);
		c->load_path = path.string();
		try
		{
			if (!std::filesystem::exists(path))
			{
				nosEngine.LogE("Read Image cannot load file %s", path.string().c_str());
				c->UpdateStatus(State::Failed);
				return NOS_RESULT_FAILED;
			}

			int w, h, n;
			stbi_info(path.string().c_str(), &w, &h, &n);
			
			nosResourceShareInfo outRes = {
				.Info = {.Type = NOS_RESOURCE_TYPE_TEXTURE,
						 .Texture = {.Width = (u32)w, .Height = (u32)h, .Format = NOS_FORMAT_R8G8B8A8_UNORM, .FieldType = NOS_TEXTURE_FIELD_TYPE_PROGRESSIVE}}};

			// unless reading raw bytes, this is useless since samplers convert to linear space automatically
			if (*InterpretPinValue<bool>(nodeParams[NSN_sRGB].Data->Data))
				outRes.Info.Texture.Format = NOS_FORMAT_R8G8B8A8_SRGB;

			nosEngine.SetPinValue(nodeParams[NSN_Out].Id, nos::Buffer::From(nos::vkss::ConvertTextureInfo(outRes)));
			std::thread([c, nodeParams = vkss::OwnedNodeExecuteParams(*params->ParentNodeExecuteParams), &outRes]() mutable  {
				nos::NodeExecuteParams params(&nodeParams);
				std::filesystem::path path = InterpretPinValue<const char>(params[NSN_Path].Data->Data);
				try
				{
					if (!std::filesystem::exists(path))
					{
						nosEngine.LogE("Read Image cannot load file %s", path.string().c_str());
						return;
					}

					int w, h, n;
					u8* img = stbi_load(path.string().c_str(), &w, &h, &n, 4);
					if (!img)
					{
						nosEngine.LogE("Couldn't load image from %s.", path.string().c_str());
						return;
					}

					auto res = vkss::DeserializeTextureInfo(params[NSN_Out].Data->Data);

					nosCmd cmd{};
					nosVulkan->Begin("ReadImage: Load", &cmd);
					nosVulkan->ImageLoad(cmd, img, nosVec2u(w, h), NOS_FORMAT_R8G8B8A8_SRGB, &res);
					nosCmdEndParams endParams{.ForceSubmit = true};
					nosVulkan->End(cmd, &endParams);

					free(img);
					flatbuffers::FlatBufferBuilder fbb;
					HandleEvent(CreateAppEvent(fbb, app::CreatePinDirtied(fbb, &params[NSN_Out].Id)));
				}
				catch (const std::exception& e)
				{
					nosEngine.LogE("Error while loading image: %s", e.what());
				}

                c->UpdateStatus(State::Idle);
			}).detach();
		}
		catch (const std::exception& e)
		{
			nosEngine.LogE("Error while loading image: %s", e.what());
            c->UpdateStatus(State::Failed);
		}
		return NOS_RESULT_SUCCESS;
    };
    
    return NOS_RESULT_SUCCESS;
}


nosResult RegisterReadImage(nosNodeFunctions* fn)
{
	fn->ClassName = NSN_Nos_Utilities_ReadImage;
	fn->GetFunctions = GetFunctions;

	fn->OnNodeCreated = [](const nosFbNode* node, void** outCtxPtr)
	{
		*outCtxPtr = new ReadImageContext{.nodeid = *node->id()};
	};
	fn->OnNodeDeleted = [](void* ctx, nosUUID nodeId)
	{
		delete (ReadImageContext*)ctx;
	};

	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities