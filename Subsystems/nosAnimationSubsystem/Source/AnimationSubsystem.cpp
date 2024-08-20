// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/SubsystemAPI.h>

#include <nosAnimationSubsystem/AnimEditorTypes_generated.h>
#include <PinDataAnimator.h>


NOS_INIT()
NOS_BEGIN_IMPORT_DEPS()
NOS_END_IMPORT_DEPS()

namespace nos::sys::anim
{
namespace editor
{
	NOS_FBS_CREATE_FUNCTION_MAKE_FOR_UNION(nos::sys::anim::editor, FromAnimation)
}
static std::unique_ptr<PinDataAnimator> GAnimator = nullptr;

nosResult OnPreExecuteNode(nosNodeExecuteParams* params)
{
	nosUUID scheduledNodeId;
	if (nosEngine.GetCurrentRunnerPathInfo(&scheduledNodeId, nullptr) == NOS_RESULT_FAILED)
		return NOS_RESULT_FAILED;

	auto pathInfo = GAnimator->GetPathInfo(scheduledNodeId);

	if(!pathInfo)
		return NOS_RESULT_FAILED;

	uint64_t curFrame = pathInfo->StartFSM + pathInfo->CurFrame;

	for (size_t i = 0; i < params->PinCount; ++i)
		GAnimator->UpdatePin(params->Pins[i].Id, params->DeltaSeconds, curFrame, params->Pins[i].Data);
	return NOS_RESULT_SUCCESS;
}

void OnPinDeleted(nosUUID pinId)
{
	GAnimator->OnPinDeleted(pinId);
}

nosResult ShouldExecuteNodeWithoutDirty(nosNodeExecuteParams* params)
{
	for (size_t i = 0; i < params->PinCount; ++i)
	{
		auto const& pinId = params->Pins[i].Id;
		if(params->Pins[i].ShowAs == fb::ShowAs::OUTPUT_PIN)
			continue;
		if(GAnimator->IsPinAnimating(pinId))
			return NOS_RESULT_SUCCESS;
	}
	return NOS_RESULT_NOT_FOUND;
}

void OnPathStart(nosUUID scheduledPinId)
{
	nosVec2u deltaSec;
	nosEngine.GetCurrentRunnerPathInfo(nullptr, &deltaSec);
	GAnimator->CreatePathInfo(scheduledPinId, deltaSec);
}

void OnPathStop(nosUUID scheduledPinId)
{
	GAnimator->DeletePathInfo(scheduledPinId);
}

void OnEndFrame(nosUUID scheduledPinId)
{
	GAnimator->PathExecutionFinished(scheduledPinId);
}

void OnMessageFromEditor(uint64_t editorId, nosBuffer blob)
{
	auto msg = flatbuffers::GetRoot<sys::anim::editor::FromEditor>(blob.Data);
	switch (msg->event_type())
	{
	case sys::anim::editor::FromEditorUnion::AnimatePin: {
		auto animatePin = msg->event_as_AnimatePin();
		if(!animatePin || !animatePin->pin_path())
			return;
		nosUUID pinId{};
		if (nosEngine.ItemPathToItemId(animatePin->pin_path()->c_str(), &pinId) == NOS_RESULT_SUCCESS)
		{
			nosUUID sourceId{};
			if (nosEngine.GetSourcePinId(pinId, &sourceId) == NOS_RESULT_SUCCESS)
				GAnimator->AddAnimation(sourceId, *animatePin);
		}
		else
		{
			nosEngine.LogE("Failed to find pin %s", animatePin->pin_path()->c_str());
		}
		break;
	}
	default:
		break;
	}
}

void OnEditorConnected(uint64_t editorId)
{
	// TODO: AnimSys send animatable types to editor
	auto names = GAnimator->GetAnimatableTypes();
		
	editor::TAnimatableTypes types;

	for (auto& name : names)
		types.types.push_back(name.AsString());

	flatbuffers::FlatBufferBuilder fbb;
	fbb.Finish(editor::MakeFromAnimation(fbb, editor::CreateAnimatableTypes(fbb, &types)));
	nos::Buffer buf = fbb.Release();
	nosEngine.SendCustomMessageToEditors(NOS_NAME("nos.sys.animation"), buf);
}

nosResult NOSAPI_CALL OnPreUnloadSubsystem()
{
	nos::sys::anim::GAnimator = nullptr;
	return NOS_RESULT_SUCCESS;
}
}

extern "C"
{
NOSAPI_ATTR nosResult NOSAPI_CALL nosExportSubsystem(nosSubsystemFunctions* subsystemFunctions)
{
		
	subsystemFunctions->OnPreExecuteNode = nos::sys::anim::OnPreExecuteNode;
	subsystemFunctions->ShouldExecuteNodeWithoutDirty = nos::sys::anim::ShouldExecuteNodeWithoutDirty;
	subsystemFunctions->OnPathStart = nos::sys::anim::OnPathStart;
	subsystemFunctions->OnPathStop = nos::sys::anim::OnPathStop;
	subsystemFunctions->OnEndFrame = nos::sys::anim::OnEndFrame;
	subsystemFunctions->OnPinDeleted = nos::sys::anim::OnPinDeleted;

	subsystemFunctions->OnMessageFromEditor = nos::sys::anim::OnMessageFromEditor;
	subsystemFunctions->OnEditorConnected = nos::sys::anim::OnEditorConnected;

	subsystemFunctions->OnPreUnloadSubsystem = nos::sys::anim::OnPreUnloadSubsystem;
		
	nos::sys::anim::GAnimator = std::make_unique<nos::sys::anim::PinDataAnimator>();
	return NOS_RESULT_SUCCESS;
}
}