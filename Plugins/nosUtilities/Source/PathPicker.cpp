// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>

#include "Names.h"
#include "PathPicker_generated.h"

namespace nos::utilities
{
NOS_REGISTER_NAME(Mode);

// Forwards the path picked on the Path property to the Out pin, so nodes whose
// path pin carries no file picker can still be driven from one. Mode swaps the
// picker between files and folders.
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
		else if (pinName == NSN_Mode)
			SetPinVisualizer(NSN_Path, {.type = *static_cast<PathPickerMode*>(val.Data) == PathPickerMode::Folder
											? fb::VisualizerType::FOLDER_PICKER
											: fb::VisualizerType::FILE_PICKER});
	}
};

nosResult RegisterPathPicker(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("PathPicker"), PathPickerNode, fn);
	return NOS_RESULT_SUCCESS;
}

} // namespace nos::utilities
