// Copyright MediaZ AS. All Rights Reserved.

// Includes
#include <MediaZ/PluginAPI.h>
#include <glm/glm.hpp>
#include <Builtins_generated.h>

// Shaders
#include "ColorCorrect.frag.spv.dat"
#include "Diff.frag.spv.dat"
#include "GaussianBlur.frag.spv.dat"
#include "Kuwahara.frag.spv.dat"
#include "KawaseLightStreak.frag.spv.dat"
#include "PremultiplyAlpha.frag.spv.dat"
#include "Sharpen.frag.spv.dat"
#include "Sobel.frag.spv.dat"
#include "Thresholder.frag.spv.dat"
#include "Sampler.frag.spv.dat"

// Nodes
#include "GaussianBlur.hpp"

MZ_INIT();

namespace mz::filters
{

enum Filters : int
{
	ColorCorrect = 0,
	Diff,
	Kuwahara,
	GaussianBlur,
	KawaseLightStreak,
	PremultiplyAlpha,
	Sharpen,
	Sobel,
	Thresholder,
	Sampler,
	Count
};

#define COLOR_PIN_COLOR_IDX 0
#define COLOR_PIN_OUTPUT_IDX 1

extern "C"
{

MZAPI_ATTR mzResult MZAPI_CALL mzExportNodeFunctions(size_t* outSize, mzNodeFunctions* outList)
{
	if (!outList)
	{
		*outSize = Filters::Count;
		return MZ_RESULT_SUCCESS;
	}
	auto* node = outList;
	int i = 0;
	do
	{
		switch ((Filters)i++)
		{
		// COLOR CORRECT FILTER
		case Filters::ColorCorrect: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.ColorCorrect");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf) -> mzResult {
				outSpirvBuf->Data = (void*)(ColorCorrect_frag_spv);
				outSpirvBuf->Size = sizeof(ColorCorrect_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// DIFF FILTER
		case Filters::Diff: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.Diff");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf) -> mzResult {
				outSpirvBuf->Data = (void*)(Diff_frag_spv);
				outSpirvBuf->Size = sizeof(Diff_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// KUWAHARA FILTER
		case Filters::Kuwahara: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.Kuwahara");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf) -> mzResult {
				outSpirvBuf->Data = (void*)(Kuwahara_frag_spv);
				outSpirvBuf->Size = sizeof(Kuwahara_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// GAUSSIAN BLUR FILTER
		case Filters::GaussianBlur: {
			RegisterGaussianBlur(node);
			break;
		}
		// MERGE FILTER
		case Filters::KawaseLightStreak: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.KawaseLightStreak");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf)-> mzResult {
				outSpirvBuf->Data = (void*)(KawaseLightStreak_frag_spv);
				outSpirvBuf->Size = sizeof(KawaseLightStreak_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// PREMULTIPLY ALPHA FILTER
		case Filters::PremultiplyAlpha: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.PremultiplyAlpha");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf)-> mzResult {
				outSpirvBuf->Data = (void*)(PremultiplyAlpha_frag_spv);
				outSpirvBuf->Size = sizeof(PremultiplyAlpha_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// SHARPEN FILTER
		case Filters::Sharpen: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.Sharpen");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf)-> mzResult {
				outSpirvBuf->Data = (void*)(Sharpen_frag_spv);
				outSpirvBuf->Size = sizeof(Sharpen_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// SOBEL FILTER
		case Filters::Sobel: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.Sobel");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf)-> mzResult {
				outSpirvBuf->Data = (void*)(Sobel_frag_spv);
				outSpirvBuf->Size = sizeof(Sobel_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// THRESHOLDER FILTER
		case Filters::Thresholder: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.Thresholder");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf)-> mzResult {
				outSpirvBuf->Data = (void*)(Thresholder_frag_spv);
				outSpirvBuf->Size = sizeof(Thresholder_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		// SAMPLER FILTER
		case Filters::Sampler: {
			node->TypeName = MZ_NAME_STATIC("mz.filters.Sampler");
			node->GetShaderSource = [](mzBuffer* outSpirvBuf)-> mzResult {
				outSpirvBuf->Data = (void*)(Sampler_frag_spv);
				outSpirvBuf->Size = sizeof(Sampler_frag_spv);
				return MZ_RESULT_SUCCESS;
			};
			break;
		}
		default: break;
		}
	} while (node = node->Next);
	return MZ_RESULT_SUCCESS;
}
}
}
