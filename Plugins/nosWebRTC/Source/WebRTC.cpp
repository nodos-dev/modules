// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginAPI.h>
#include <Builtins_generated.h>
#include <Nodos/PluginHelpers.hpp>
#include <AppService_generated.h>
#include <AppEvents_generated.h>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>
#include "Names.h"

NOS_INIT()
NOS_VULKAN_INIT()

NOS_BEGIN_IMPORT_DEPS()
	NOS_VULKAN_INIT()
NOS_END_IMPORT_DEPS()

nosResult RegisterWebRTCPlayer(nosNodeFunctions* outFunctions);
nosResult RegisterWebRTCStreamer(nosNodeFunctions* outFunctions);
void RegisterWebRTCSignalingServer(nosNodeFunctions* outFunctions);

NOS_REGISTER_NAME(In)
NOS_REGISTER_NAME(Out)
NOS_REGISTER_NAME(ServerIP)
NOS_REGISTER_NAME(StreamerID)
NOS_REGISTER_NAME(MaxFPS)
NOS_REGISTER_NAME(WebRTCPlayer);
NOS_REGISTER_NAME(WebRTCStreamer);
NOS_REGISTER_NAME(TargetBitrate);

NOS_REGISTER_NAME(RGBtoYUV420_Compute_Shader);
NOS_REGISTER_NAME(RGBtoYUV420_Compute_Pass);
NOS_REGISTER_NAME(YUV420toRGB_Compute_Shader);
NOS_REGISTER_NAME(YUV420toRGB_Compute_Pass);
NOS_REGISTER_NAME(Input);
NOS_REGISTER_NAME(Output);
NOS_REGISTER_NAME(PlaneY);
NOS_REGISTER_NAME(PlaneU);
NOS_REGISTER_NAME(PlaneV);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outCount, nosNodeFunctions** outFunctions) {
	*outCount = (size_t)(3);
	if (!outFunctions)
		return NOS_RESULT_SUCCESS;

	nosResult res = RegisterWebRTCStreamer(outFunctions[0]);
	if (res != NOS_RESULT_SUCCESS)
		return res;
	res = RegisterWebRTCPlayer(outFunctions[1]);
	if (res != NOS_RESULT_SUCCESS)
		return res;
	RegisterWebRTCSignalingServer(outFunctions[2]);

	return NOS_RESULT_SUCCESS;
}

extern "C"
{

NOSAPI_ATTR nosResult NOSAPI_CALL nosExportPlugin(nosPluginFunctions* outFunctions)
{
	outFunctions->ExportNodeFunctions = ExportNodeFunctions;
	return NOS_RESULT_SUCCESS;
}

}