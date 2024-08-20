// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

namespace nos::utilities
{
NOS_REGISTER_NAME(Seconds);
NOS_REGISTER_NAME_SPACED(Nos_Utilities_Time, "nos.utilities.Time")
struct TimeNodeContext : NodeContext
{
	TimeNodeContext(nosFbNode const* node) : NodeContext(node) {}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		auto pin = GetPinValues(params);
		auto sec = GetPinValue<float>(pin, NSN_Seconds);
		float time = (params->DeltaSeconds.x * frameCount++) / (double)params->DeltaSeconds.y;
		nosEngine.SetPinValue(params->Pins[0].Id, { .Data = &time, .Size = sizeof(float) });
		return NOS_RESULT_SUCCESS;
	}

	uint64_t frameCount = 0;
};


nosResult RegisterTime(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NSN_Nos_Utilities_Time, TimeNodeContext, fn);
	// functions["nos.CalculateNodalPoint"].EntryPoint = [](nos::Args& args, void* ctx){
	// 	auto pos = args.Get<glm::dvec3>("Camera Position");
	// 	auto rot = args.Get<glm::dvec3>("Camera Orientation");
	// 	auto sca = args.Get<f64>("Nodal Offset");
	// 	auto out = args.Get<glm::dvec3>("Nodal Point");
	// 	glm::dvec2 ANG = glm::radians(glm::dvec2(rot->z, rot->y));
	// 	glm::dvec2 COS = cos(ANG);
	// 	glm::dvec2 SIN = sin(ANG);
	// 	glm::dvec3 f = glm::dvec3(COS.y * COS.x, COS.y * SIN.x, SIN.y);
	// 	*out = *pos + f **sca;
	// 	return true;
	// };
	return NOS_RESULT_SUCCESS;
}

} // namespace nos