// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include "Track.h"

namespace nos::track
{

enum TrackNode : int
{
	FreeD,
	UserTrack,
	Count
};

void RegisterFreeDNode(nosNodeFunctions* functions);
void RegisterController(nosNodeFunctions* functions);

nosResult NOSAPI_CALL ExportNodeFunctions(size_t* outSize, nosNodeFunctions** outList)
{
	*outSize = (size_t)TrackNode::Count;
	if (!outList)
		return NOS_RESULT_SUCCESS;

	for (int i = 0; i < TrackNode::Count; ++i)
	{
		auto node = outList[i];
		switch ((TrackNode)i)
		{
		case TrackNode::FreeD:
			RegisterFreeDNode(node);
			break;
		case TrackNode::UserTrack:
			RegisterController(node);
			break;
		}
	}
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

} // namespace nos::track