// Copyright Nodos AS. All Rights Reserved.
#include <Nodos/SubsystemAPI.h>
#include "nosAIServices.h"

NOS_INIT();

namespace nos::ai
{
	extern "C"
	{

		NOSAPI_ATTR nosResult NOSAPI_CALL nosExportSubsystem(nosSubsystemFunctions* subsystemFunctions, void** exported)
		{
			auto subsystem = new nosAISubsystem;
			nosResult res = nos::ai::Bind(subsystem);
			if (res != NOS_RESULT_SUCCESS)
				return res;

			*exported = subsystem;
			return NOS_RESULT_SUCCESS;
		}

		NOSAPI_ATTR nosResult NOSAPI_CALL nosExportSubsystemTypeFunctions(size_t* outSize, nosSubsystemTypeFunctions** outList)
		{
			*outSize = 0;
			if (!outList)
				return NOS_RESULT_SUCCESS;
			return NOS_RESULT_SUCCESS;
		}

		NOSAPI_ATTR nosResult NOSAPI_CALL nosExportSubsystemNodeFunctions(size_t* outSize, nosSubsystemNodeFunctions** outList)
		{
			*outSize = 0;
			if (!outList)
				return NOS_RESULT_SUCCESS;
			return NOS_RESULT_SUCCESS;
		}

		NOSAPI_ATTR nosResult NOSAPI_CALL nosUnloadSubsystem(void* subsystemContext)
		{
			//TODO: Garbage Collect?
			return NOS_RESULT_SUCCESS;
		}
	}

}