// Copyright MediaZ AS. All Rights Reserved.

#include "BasicMain.h"

#include "GaussianBlur.frag.spv.dat"

namespace mz
{

struct GaussBlurContext : public NodeContext
{
    mz::fb::TTexture IntermediateTexture;

    GaussBlurContext(fb::Node const& node) : NodeContext(node)
    {
        RegisterShaders();
        RegisterPasses();

		IntermediateTexture.filtering = mz::fb::Filtering::LINEAR;
		IntermediateTexture.usage = mz::fb::ImageUsage::RENDER_TARGET | mz::fb::ImageUsage::SAMPLED;
    }

    ~GaussBlurContext()
    {
        DestroyResources();
    }

    void RegisterShaders()
    {
        static bool registered = false;
        if (registered)
            return;
        GServices.MakeAPICalls(true,
                              app::TRegisterShader{
                                  .key = "Gaussian_Blur",
                                  .spirv = ShaderSrc<sizeof(GaussianBlur_frag_spv)>(GaussianBlur_frag_spv)});
        registered = true;
    }

    void RegisterPasses()
    {
        GServices.MakeAPICalls(true,
                              app::TRegisterPass{
                                  .key = "Gaussian_Blur_Pass_" + UUID2STR(NodeId),
                                  .shader = "Gaussian_Blur",
                              });
    }

    void DestroyResources()
    {
        if (IntermediateTexture.handle)
            GServices.Destroy(IntermediateTexture);
        GServices.MakeAPICalls(true, app::TUnregisterPass{
                                        .key = "Gaussian_Blur_Pass_" + UUID2STR(NodeId)});
    }

    void SetupIntermediateTexture(mz::fb::TTexture* outputTexture)
    {
        if (IntermediateTexture.width == outputTexture->width &&
            IntermediateTexture.height == outputTexture->height &&
            IntermediateTexture.format == outputTexture->format)
            return;

        GServices.Destroy(IntermediateTexture);
        IntermediateTexture.width = outputTexture->width;
        IntermediateTexture.height = outputTexture->height;
        IntermediateTexture.format = outputTexture->format;
        GServices.Create(IntermediateTexture);
    }

    void Run(mz::Args& pins)
    {
        float softness = *pins.Get<float>("Softness");
        softness += 1.0f; // [0, 1] -> [1, 2]
        mz::fb::vec2 kernelSize = *pins.Get<mz::fb::vec2>("Kernel_Size");
        float horzKernel = kernelSize.x();
        float vertKernel = kernelSize.y();

		auto outputTexture = pins.GetBuffer("Output")->As<mz::fb::TTexture>();
		SetupIntermediateTexture(&outputTexture);

        // Pass 1 begin
        app::TRunPass horzPass;
        horzPass.pass = "Gaussian_Blur_Pass_" + UUID2STR(NodeId);
        CopyUniformFromPin(horzPass, pins, "Input");
        AddUniform(horzPass, "Softness", &softness, sizeof(softness));
        AddUniform(horzPass, "Kernel_Size", &horzKernel, sizeof(horzKernel));
        u32 passType = 0; // Horizontal pass
        AddUniform(horzPass, "Pass_Type", &passType, sizeof(passType));
        horzPass.output.reset(&IntermediateTexture);
        // Pass 1 end
        // Pass 2 begin
        app::TRunPass vertPass;
        vertPass.pass = "Gaussian_Blur_Pass_" + UUID2STR(NodeId);
        AddUniform(vertPass, "Input", mz::Buffer::From(IntermediateTexture));
        AddUniform(vertPass, "Softness", &softness, sizeof(softness));
        AddUniform(vertPass, "Kernel_Size", &vertKernel, sizeof(vertKernel));
        passType = 1; // Vertical pass
        AddUniform(vertPass, "Pass_Type", &passType, sizeof(passType));
        vertPass.output.reset(&outputTexture);
        // Pass 2 end
        // Run passes
        GServices.MakeAPICalls(false, horzPass, vertPass);
        vertPass.output.release();
        horzPass.output.release();
    }
};

void RegisterGaussianBlur(NodeActionsMap& functions)
{
    auto& actions = functions["mz.GaussianBlur"];

    actions.NodeCreated = [](fb::Node const& node, Args& args, void** ctx) {
        *ctx = new GaussBlurContext(node);
    };
    actions.NodeRemoved = [](void* ctx, mz::fb::UUID const& id) {
        delete static_cast<GaussBlurContext*>(ctx);
    };
    actions.PinValueChanged = [](void* ctx, mz::fb::UUID const& id, mz::Buffer* value) {};
    actions.EntryPoint = [](mz::Args& pins, void* ctx) {
        auto* blurCtx = static_cast<GaussBlurContext*>(ctx);
        blurCtx->Run(pins);
        return true;
    };
}

} // namespace mz