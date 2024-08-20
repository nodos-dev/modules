// Copyright MediaZ Teknoloji A.S. All Rights Reserved.
#include "Track.h"
#include "Builtins_generated.h"



namespace nos::track
{
static NOS_REGISTER_NAME(Input);
static NOS_REGISTER_NAME(Impulse);
static NOS_REGISTER_NAME(Decay);
static NOS_REGISTER_NAME(Track);

void RegisterController(nosNodeFunctions* functions)
{
	functions->ClassName = NOS_NAME_STATIC("nos.track.UserTrack");
    struct UserTrack {
        
		glm::vec3 v = {};
		f32 impulse = 1.0;
		f32 decay = 0;
		nos::fb::UUID OutTrackId;
		nos::fb::UUID InTrackId;

		fb::TTrack state;

        void ReadAndUpdate(fb::TTrack const& track)
        {
			state = track;
        	auto val = nos::Buffer::From(track);
        	nosEngine.SetPinValue(OutTrackId, {.Data = val.Data(), .Size = val.Size()});
        	nosEngine.SetPinValue(InTrackId, {.Data = val.Data(), .Size = val.Size()});
        }

		f32& yaw()
		{
			return ((glm::vec3&)state.rotation).z;
		}
		f32& pitch()
		{
			return ((glm::vec3&)state.rotation).y;
		}
    };

    functions->OnNodeCreated = [](fb::Node const* n, void** ctx) {
        auto c = new UserTrack();
        *ctx = c;
		auto pins = PinMapping().Load(*n);

		auto input  = pins[NSN_Input];
		auto output = pins[NSN_Track];
		auto impulse = pins[NSN_Impulse];
		auto decay   = pins[NSN_Decay];

        c->OutTrackId = *output->id();
        c->InTrackId = *input->id();

		auto inBuf = nos::Buffer(input->data());
		c->ReadAndUpdate(inBuf.As<nos::fb::TTrack>());
    };

    functions->OnNodeDeleted = [](void* ctx, auto) {delete (UserTrack*)ctx; };
    functions->OnKeyEvent = [](void* ctx_, nosKeyEvent const* keyEvent){
		auto& key = keyEvent->Key;
    	auto& mdelta = keyEvent->MouseDelta;
    	auto ctx = (UserTrack*)ctx_;

		glm::vec3& rot = (glm::vec3&)ctx->state.rotation;
		glm::vec3& pos = (glm::vec3&)ctx->state.location;

        if (32 == key) // ?
        {
			rot = {};
            pos = glm::vec3{600, 50, 100};
        }
		
		auto orientation = MakeRotation(rot);

        glm::vec3 f = orientation[0];
        glm::vec3 r = orientation[1];
        glm::vec3 u = orientation[2];
        
        glm::vec3 pf =  f * f32((key == 'W') - (key == 'S'));
        glm::vec3 pr =  r * f32((key == 'D') - (key == 'A'));
        glm::vec3 pu =  u * f32((key == 'E') - (key == 'Q'));
        
        ctx->v += ctx->impulse * (pf + pr + pu);
        pos += 10.f * (pf + pr + pu);

		//glm::mat3 roll_basis = transpose(MakeRotation(glm::vec3(ctx->state.rotation.x(), 0, 0)));
		//
		//glm::mat3 delta = (glm::mat3)glm::angleAxis(mdelta.y * 0.015f, roll_basis[1]);
		//// delta = delta*(glm::mat3)glm::angleAxis(mdelta.x * 0.015f, roll_basis[2]);

		//orientation = orientation * delta;

		//(glm::vec3&)ctx->state.rotation = GetEulers(glm::mat4(orientation));
		ctx->yaw()   += glm::degrees(mdelta.x * 0.015f);
		ctx->pitch() -= glm::degrees(mdelta.y * 0.015f);

		rot = glm::mod(rot, glm::vec3(360));

		auto val = nos::Buffer::From(ctx->state);
		nosEngine.SetPinValue(ctx->OutTrackId, { .Data = val.Data(), .Size = val.Size() });
		nosEngine.SetPinValue(ctx->InTrackId, { .Data = val.Data(), .Size = val.Size() });

    };

    functions->ExecuteNode = [](void* ctx, nosNodeExecuteParams* params)
    {
        auto c = (UserTrack*)ctx;
    	auto values = GetPinValues(params);
		c->impulse = glm::max(*(f32*)values[NSN_Impulse], 1.f);
		c->decay = glm::max(*(f32*)values[NSN_Decay], 0.f);
        (glm::vec3&)c->state.location += c->v;
        c->v *= exp(-c->decay);
		auto inTrack = GetPinValue<fb::TTrack>(values, NSN_Input);
        c->ReadAndUpdate(inTrack);
        return NOS_RESULT_SUCCESS;
    };
}

}