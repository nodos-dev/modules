// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include <nosTrack/Track.h>
#include "Builtins_generated.h"



namespace nos::track
{
NOS_REGISTER_NAME(Input);
NOS_REGISTER_NAME(Impulse);
NOS_REGISTER_NAME(Decay);
NOS_REGISTER_NAME(Track);


struct UserTrack : NodeContext
{
	glm::vec3 V = {};
	float Impulse = 1.0;
	float Decay = 0;

	track::TTrack State;

	nosResult OnCreate(nosFbNodePtr node) override
	{
		for (auto* pin : *node->pins())
			if (NSN_Input == pin->name()->string_view())
				ReadAndUpdate(InterpretObjectData<track::TTrack>((void*)pin->data()->data()));
		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(NodeExecuteParams const& params) override
	{
		// Guard against unready pins: GetPinValue below is dereferenced directly,
		// so bail out if any required pin is missing (ported from zd.track.UserTrack).
		if (!(params.contains(NSN_Impulse) && params.contains(NSN_Decay) &&
			  params.contains(NSN_Track) && params.contains(NSN_Input)))
			return NOS_RESULT_INVALID_ARGUMENT;
		Impulse = glm::max(*params.GetPinValue<float>(NSN_Impulse), 1.f);
		Decay = glm::max(*params.GetPinValue<float>(NSN_Decay), 0.f);
		(glm::vec3&)State.location += V;
		V *= exp(-Decay);
		ReadAndUpdate(params.GetPinValue<track::TTrack>(NSN_Input));
		return NOS_RESULT_SUCCESS;
	}

	void ReadAndUpdate(track::TTrack const& track)
	{
		State = track;
		SetPinValue(NSN_Track, track);
		SetPinValue(NSN_Input, track);
	}

	float& Yaw() { return ((glm::vec3&)State.rotation).z; }
	float& Pitch() { return ((glm::vec3&)State.rotation).y; }

	void OnKeyEvent(nosKeyEvent const* keyEvent) override
	{
		auto& key = keyEvent->Key;
		auto& mdelta = keyEvent->MouseDelta;

		glm::vec3& rot = (glm::vec3&)State.rotation;
		glm::vec3& pos = (glm::vec3&)State.location;

		if (32 == key) // ?
		{
			rot = {};
			pos = glm::vec3{600, 50, 100};
		}

		auto orientation = MakeRotation(rot);

		glm::vec3 f = orientation[0];
		glm::vec3 r = orientation[1];
		glm::vec3 u = orientation[2];

		glm::vec3 pf = f * float((key == 'W') - (key == 'S'));
		glm::vec3 pr = r * float((key == 'D') - (key == 'A'));
		glm::vec3 pu = u * float((key == 'E') - (key == 'Q'));

		V += Impulse * (pf + pr + pu);
		pos += 10.f * (pf + pr + pu);

		// glm::mat3 roll_basis = transpose(MakeRotation(glm::vec3(ctx->state.rotation.x(), 0, 0)));
		//
		// glm::mat3 delta = (glm::mat3)glm::angleAxis(mdelta.y * 0.015f, roll_basis[1]);
		//// delta = delta*(glm::mat3)glm::angleAxis(mdelta.x * 0.015f, roll_basis[2]);

		// orientation = orientation * delta;

		//(glm::vec3&)ctx->state.rotation = GetEulers(glm::mat4(orientation));
		Yaw() += glm::degrees(mdelta.x * 0.015f);
		Pitch() -= glm::degrees(mdelta.y * 0.015f);

		rot = glm::mod(rot, glm::vec3(360));

		SetPinValue(NSN_Track, State);
		SetPinValue(NSN_Input, State);
	}

};

void RegisterController(nosNodeFunctions* functions)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("nos.track.UserTrack"), UserTrack, functions);
}

}