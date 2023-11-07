#include <MediaZ/Helpers.hpp>

namespace mz::utilities
{
MZ_REGISTER_NAME(Seconds);
MZ_REGISTER_NAME(Time_Pass);
MZ_REGISTER_NAME(Time_Shader);
MZ_REGISTER_NAME_SPACED(Mz_Utilities_Time, "mz.utilities.Time")
struct TimeNodeContext : NodeContext
{
	TimeNodeContext(mzFbNode const* node) : NodeContext(node) {}

	mzResult ExecuteNode(const mzNodeExecuteArgs* args) override
	{
		auto pin = GetPinValues(args);
		auto sec = GetPinValue<float>(pin, MZN_Seconds);
		float time = (args->DeltaSeconds.x * frameCount++) / (double)args->DeltaSeconds.y;
		mzEngine.SetPinValue(args->PinIds[0], { .Data = &time, .Size = sizeof(float) });
		return MZ_RESULT_SUCCESS;
	}

	uint64_t frameCount = 0;
};


void RegisterTime(mzNodeFunctions* fn)
{
	MZ_BIND_NODE_CLASS(MZN_Mz_Utilities_Time, TimeNodeContext, fn);
	// functions["mz.CalculateNodalPoint"].EntryPoint = [](mz::Args& args, void* ctx){
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
}

} // namespace mz