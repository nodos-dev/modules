// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include "Names.h"

namespace nos::utilities
{
// Forwards the path picked on the Path property to the Out pin, so nodes whose
// path pin carries no file picker can still be driven from one.
struct PathPickerNode : NodeContext
{
	PathPickerNode(nosFbNodePtr node) : NodeContext(node)
	{
		if (!node->pins())
			return;
		for (auto* pin : *node->pins())
		{
			if (!flatbuffers::IsFieldPresent(pin, fb::Pin::VT_DATA))
				continue;
			nosBuffer value = {.Data = (void*)pin->data()->data(), .Size = pin->data()->size()};
			OnPinValueChanged(nos::Name(pin->name()->c_str()), *pin->id(), value);
		}
	}

	void OnPinValueChanged(nos::Name pinName, uuid const& pinId, nosBuffer val) override
	{
		if (pinName == NSN_Path)
			SetPinValue(NSN_Out, val);
	}
};

nosResult RegisterPathPicker(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("PathPicker"), PathPickerNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities
