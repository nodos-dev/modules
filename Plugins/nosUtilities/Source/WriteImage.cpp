// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include "Builtins_generated.h"
#include <AppService_generated.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <nosVulkanSubsystem/Helpers.hpp>
#include "../Shaders/Linear2SRGB.frag.spv.dat"

#include <mutex>

#include "Names.h"

namespace nos::utilities
{
NOS_REGISTER_NAME(Linear2SRGB_Pass);
NOS_REGISTER_NAME(Linear2SRGB_Shader);
NOS_REGISTER_NAME(In);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_WriteImage, "nos.utilities.WriteImage")

struct WriteImage : NodeContext {
    std::filesystem::path Path;
    nosResourceShareInfo Input;
    nosGPUEvent Event = 0;
    std::atomic_bool WriteRequested = false;
    std::condition_variable CV;
    std::mutex Mutex;
    std::thread Worker;
    std::atomic_bool Write = false;
    std::atomic_bool ShouldStop = false;

    WriteImage(nosFbNode const* node) : NodeContext(node){
        Worker = std::thread([this] {
            while (!ShouldStop) {
                std::unique_lock<std::mutex> lock(Mutex);
                CV.wait(lock, [this] { return Write || ShouldStop; });
                if (ShouldStop)
                    break;
                if(Event)
                    nosVulkan->WaitGpuEvent(&Event, UINT64_MAX);
                if (this->Write) {
					this->Write = false;
					this->WriteImageToFile();
				}
            }
        });
        for (auto* pin : *node->pins()) {
            auto* pinData = pin->data();
            nosBuffer value = { .Data = (void*)pinData->data(), .Size = pinData->size() };
            OnPinValueChanged(nos::Name(pin->name()->c_str()), *pin->id(), value);
        }
    }

    ~WriteImage() {
		ShouldStop = true;
		CV.notify_all();
		Worker.join();
	}

    void SignalWrite() {
		std::unique_lock<std::mutex> lock(Mutex);
        Write = true;
		CV.notify_all();
	}

    void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override 
    {
        std::unique_lock<std::mutex> lock(Mutex);
		if (pinName == NSN_In)
			Input = vkss::DeserializeTextureInfo(value.Data);
		else if (pinName == NSN_Path)
			Path = std::string((const char*)value.Data, value.Size);
	}

    nosResult CopyTo(nosCopyInfo* copyInfo) override
    {
        nosCmd cmd;
        assert(Event == 0);
        nosVulkan->Begin("Write Image Copy To", &cmd);
		nosCmdEndParams endParams{.ForceSubmit = true, .OutGPUEventHandle = WriteRequested ? &Event : nullptr};
        nosVulkan->End(cmd, &endParams);
		if (WriteRequested)
		{
			nosEngine.RecompilePath(NodeId);
			WriteRequested = false;
			SignalWrite();
		}
        return NOS_RESULT_SUCCESS;
    }

    void WriteImageToFile() {
        auto& path = this->Path;
        auto& input = this->Input;
        try {
            if (!std::filesystem::exists(path.parent_path()))
                std::filesystem::create_directories(path.parent_path());
        }
        catch (std::filesystem::filesystem_error& e) {
            nosEngine.LogE("WriteImage - %s: %s", path.string().c_str(), e.what());
            return;
        }
        nosEngine.LogI("WriteImage: Writing frame to file %s", path.string().c_str());

        struct Captures
        {
            nosResourceShareInfo SRGB;
            nosResourceShareInfo Buf = {};
            std::filesystem::path Path;
        } captures = Captures{ .SRGB = input,.Path = path };

        captures.SRGB.Info.Texture.Format = NOS_FORMAT_R8G8B8A8_SRGB;
        captures.SRGB.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
        nosVulkan->CreateResource(&captures.SRGB);

        nosVulkan->Download(0, &captures.SRGB, &captures.Buf);

        if (auto buf2write = nosVulkan->Map(&captures.Buf))
            if (!stbi_write_png(captures.Path.string().c_str(), captures.SRGB.Info.Texture.Width, captures.SRGB.Info.Texture.Height, 4, buf2write, captures.SRGB.Info.Texture.Width * 4))
                nosEngine.LogE("WriteImage: Unable to write frame to file", "");
            else
                nosEngine.LogI("WriteImage: Wrote frame to file %s", captures.Path.string().c_str());
        nosVulkan->DestroyResource(&captures.Buf);
        nosVulkan->DestroyResource(&captures.SRGB);
    }

    void GetScheduleInfo(nosScheduleInfo* info) override
    { 
        info->NotScheduled = !WriteRequested;
	}

    static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns)
    {
        *count = 1;
        if (!names || !fns)
            return NOS_RESULT_SUCCESS;

        *names = NOS_NAME_STATIC("WriteImage_Save");
        *fns = [](void* ctx, nosFunctionExecuteParams* params)
        {
            auto writeImage = (WriteImage*)ctx;
            auto ids = GetPinIds(params->ParentNodeExecuteParams);
            writeImage->WriteRequested = true;
			// TODO: PathRunner
			//nosSchedulePinParams scheduleParams{ids[NSN_In], 1, true};
			//nosEngine.ScheduleNode(&scheduleParams);
            nosEngine.LogI("WriteImage: Write requested");
			return NOS_RESULT_SUCCESS;
        };

        return NOS_RESULT_SUCCESS;
    }
};

nosResult RegisterWriteImage(nosNodeFunctions* fn)
{
    NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_WriteImage, WriteImage, fn);

    nosShaderInfo2 shader = {
	    .Key = NSN_Linear2SRGB_Shader,
	    .Source = {.SpirvBlob = {(void*)Linear2SRGB_frag_spv, sizeof(Linear2SRGB_frag_spv)}},
	    .AssociatedNodeClassName = NSN_Nos_Utilities_WriteImage
    };
	auto ret = nosVulkan->RegisterShaders2(1, &shader);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	nosPassInfo pass = {
		.Key = NSN_Linear2SRGB_Pass,
		.Shader = NSN_Linear2SRGB_Shader,
		.MultiSample = 1,
	};
	return nosVulkan->RegisterPasses(1, &pass);
}

} // namespace nos