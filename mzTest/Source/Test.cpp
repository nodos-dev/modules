// Copyright MediaZ AS. All Rights Reserved.

#include <MediaZ/PluginAPI.h>

#include <Builtins_generated.h>

#include <MediaZ/Helpers.hpp>

MZ_INIT();
MZ_REGISTER_NAME(in1)
MZ_REGISTER_NAME(in2)
MZ_REGISTER_NAME(out)


static void TestFunction(void* ctx, const mzNodeExecuteArgs* nodeArgs, const mzNodeExecuteArgs* functionArgs)
{
	auto args = mz::GetPinValues(functionArgs);

	auto a = *GetPinValue<double>(args, MZN_in1);
	auto b = *GetPinValue<double>(args, MZN_in2);
	auto c = a + b;
	mzEngine.SetPinValue(functionArgs->PinIds[2], {.Data = &c, .Size = sizeof(c)});
}

static mzResult GetFunctions(size_t* outCount, mzName* pName, mzPfnNodeFunctionExecute* fns)
{
	*outCount = 1;
	if (!pName || !fns)
		return MZ_RESULT_SUCCESS;

	*fns = TestFunction;
	*pName = MZ_NAME_STATIC("TestFunction");
	return MZ_RESULT_SUCCESS;
}


extern "C"
{

	MZAPI_ATTR mzResult MZAPI_CALL mzExportNodeFunctions(size_t* outCount, mzNodeFunctions* outFunctions)
	{
		*outCount = (size_t)(4);
		if (!outFunctions)
			return MZ_RESULT_SUCCESS;
		
		outFunctions->GetFunctions = GetFunctions;
		outFunctions->TypeName = MZ_NAME_STATIC("mz.test.NodeTest");
		MZ_NEXT_NODE_CLASS(outFunctions);
		outFunctions->TypeName = MZ_NAME_STATIC("mz.test.NodeWithCategories");
		MZ_NEXT_NODE_CLASS(outFunctions);
		outFunctions->TypeName = MZ_NAME_STATIC("mz.test.NodeWithFunctions");
		MZ_NEXT_NODE_CLASS(outFunctions);
		outFunctions->TypeName = MZ_NAME_STATIC("mz.test.NodeWithCustomTypes");
		return MZ_RESULT_SUCCESS;
	}

}
