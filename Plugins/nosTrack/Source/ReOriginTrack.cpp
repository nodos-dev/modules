// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginHelpers.hpp>
#include "nosSysTrack/Track_generated.h"

#include <glm/glm.hpp>

#include <fstream>
#include <filesystem>
#include <iomanip>

#include <nosGraphics/CoordinateFrameConversion.hpp>

namespace nos::track
{

NOS_REGISTER_NAME(Input);
NOS_REGISTER_NAME(Output);
NOS_REGISTER_NAME(CoordinateSystem);
NOS_REGISTER_NAME(Origin);
NOS_REGISTER_NAME(Path);
NOS_REGISTER_NAME(ReOriginTrack_MarkOrigin);
NOS_REGISTER_NAME(ReOriginTrack_ClearOrigin);

struct ReOriginTrackContext : NodeContext
{
	nos::graphics::Frame Frame = nos::graphics::UNREAL_SYSTEM;
	nos::sys::track::TTrack Origin;     // Marked reference pose (identity until marked).
	nos::sys::track::TTrack LatestInput; // Last input seen, captured by Mark Origin.

	ReOriginTrackContext(nosFbNodePtr node) : NodeContext(node)
	{
		if (node->pins())
		{
			for (auto* pin : *node->pins())
			{
				if (!flatbuffers::IsFieldPresent(pin, fb::Pin::VT_DATA))
					continue;
				nosBuffer value = {.Data = (void*)pin->data()->data(), .Size = pin->data()->size()};
				OnPinValueChanged(nos::Name(pin->name()->c_str()), *pin->id(), value);
			}
		}
		UpdateStatus();
	}

	void OnPinValueChanged(nos::Name pinName, uuid const& pinId, nosBuffer val) override
	{
		if (pinName == NSN_CoordinateSystem)
			Frame = *static_cast<nos::graphics::Frame*>(val.Data);
		else if (pinName == NSN_Origin)
			flatbuffers::GetRoot<nos::sys::track::Track>(val.Data)->UnPackTo(&Origin);
	}

	bool HasOrigin() const
	{
		auto const& l = Origin.location;
		auto const& r = Origin.rotation;
		return l.x() != 0 || l.y() != 0 || l.z() != 0 || r.x() != 0 || r.y() != 0 || r.z() != 0;
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) override
	{
		nosBuffer inBuf{};
		for (size_t i = 0; i < params->PinCount; ++i)
			if (params->Pins[i].Name == NSN_Input)
			{
				inBuf = {.Data = (void*)params->Pins[i].Data->Data, .Size = params->Pins[i].Data->Size};
				break;
			}
		if (!inBuf.Data)
			return NOS_RESULT_SUCCESS;

		auto* in = flatbuffers::GetRoot<nos::sys::track::Track>(inBuf.Data);
		if (!in)
			return NOS_RESULT_SUCCESS;
		in->UnPackTo(&LatestInput);

		// Pass non-pose fields through; only location/rotation are re-origined.
		nos::sys::track::TTrack out = LatestInput;

		// Rigid-body relative pose: out = inverse(T_origin) * T_input.
		// transpose(R_origin) inverts the rotation; the translation is the input
		// offset rotated into the origin's local frame. Frame-agnostic given a
		// proper rotation matrix, and no unit conversion (origin and input share
		// one coordinate system).
		auto const& il = LatestInput.location;
		auto const& ol = Origin.location;
		glm::dmat3 R_origin = nos::graphics::EulerToMat(
			Frame.euler(), glm::dvec3(Origin.rotation.x(), Origin.rotation.y(), Origin.rotation.z()));
		glm::dmat3 R_in = nos::graphics::EulerToMat(
			Frame.euler(), glm::dvec3(LatestInput.rotation.x(), LatestInput.rotation.y(), LatestInput.rotation.z()));
		glm::dmat3 R_originT = glm::transpose(R_origin);

		glm::dvec3 relPos = R_originT * glm::dvec3(il.x() - ol.x(), il.y() - ol.y(), il.z() - ol.z());
		glm::dvec3 relRot = nos::graphics::MatToEuler(Frame.euler(), R_originT * R_in);

		out.location = nos::fb::vec3((float)relPos.x, (float)relPos.y, (float)relPos.z);
		out.rotation = nos::fb::vec3((float)relRot.x, (float)relRot.y, (float)relRot.z);

		auto buf = nos::Buffer::From(out);
		SetPinValue(NSN_Output, {.Data = buf.Data(), .Size = buf.Size()});
		return NOS_RESULT_SUCCESS;
	}

	void StoreOrigin()
	{
		auto buf = nos::Buffer::From(Origin);
		SetPinValue(NSN_Origin, {.Data = buf.Data(), .Size = buf.Size()});
	}

	void SaveOrigin(std::string const& utf8Path)
	{
		std::filesystem::path path = nos::Utf8ToPath(utf8Path);
		try
		{
			if (!path.parent_path().empty() && !std::filesystem::exists(path.parent_path()))
				std::filesystem::create_directories(path.parent_path());
		}
		catch (std::filesystem::filesystem_error& e)
		{
			nosEngine.LogE("ReOriginTrack: %s", e.what());
			return;
		}

		std::ofstream file(path);
		if (!file.is_open())
		{
			nosEngine.LogE("ReOriginTrack: Cannot open %s", utf8Path.c_str());
			return;
		}

		auto const& l = Origin.location;
		auto const& r = Origin.rotation;
		file << std::setprecision(12);
		file << "{\n";
		file << "\t\"coordinate_system\": {\n";
		file << "\t\t\"up\": \"" << nos::graphics::EnumNameSignedAxis(Frame.up()) << "\",\n";
		file << "\t\t\"forward\": \"" << nos::graphics::EnumNameSignedAxis(Frame.forward()) << "\",\n";
		file << "\t\t\"handedness\": \"" << nos::graphics::EnumNameHandedness(Frame.handedness()) << "\",\n";
		file << "\t\t\"euler_order\": \"" << nos::graphics::EnumNameEulerOrder(Frame.euler().order()) << "\",\n";
		file << "\t\t\"euler_sign\": [ " << (int)Frame.euler().sign_x() << ", " << (int)Frame.euler().sign_y()
			 << ", " << (int)Frame.euler().sign_z() << " ],\n";
		file << "\t\t\"meters_per_unit\": " << Frame.meters_per_unit() << "\n";
		file << "\t},\n";
		file << "\t\"location\": { \"x\": " << l.x() << ", \"y\": " << l.y() << ", \"z\": " << l.z() << " },\n";
		file << "\t\"rotation\": { \"x\": " << r.x() << ", \"y\": " << r.y() << ", \"z\": " << r.z() << " }\n";
		file << "}\n";

		nosEngine.LogI("ReOriginTrack: Saved origin to %s", utf8Path.c_str());
	}

	void UpdateStatus()
	{
		if (HasOrigin())
			SetNodeStatusMessage("Origin marked", fb::NodeStatusMessageType::INFO);
		else
			SetNodeStatusMessage("No origin (pass-through)", fb::NodeStatusMessageType::INFO);
	}

	static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns)
	{
		*count = 2;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;

		names[0] = NOS_NAME_STATIC("ReOriginTrack_MarkOrigin");
		fns[0] = [](void* ctx, nosFunctionExecuteParams* params) {
			auto* self = static_cast<ReOriginTrackContext*>(ctx);
			self->Origin = self->LatestInput;
			self->StoreOrigin();
			self->UpdateStatus();
			nosEngine.LogI("ReOriginTrack: Origin marked");

			nos::NodeExecuteParams execParams(params->FunctionNodeExecuteParams);
			auto* pathData = execParams[NSN_Path].Data;
			if (pathData && pathData->Size)
			{
				std::string path((const char*)pathData->Data, pathData->Size);
				path.resize(strlen(path.c_str())); // Drop the trailing null the string pin carries.
				if (!path.empty())
					self->SaveOrigin(path);
			}
			return NOS_RESULT_SUCCESS;
		};

		names[1] = NOS_NAME_STATIC("ReOriginTrack_ClearOrigin");
		fns[1] = [](void* ctx, nosFunctionExecuteParams*) {
			auto* self = static_cast<ReOriginTrackContext*>(ctx);
			self->Origin = nos::sys::track::TTrack{};
			self->StoreOrigin();
			self->UpdateStatus();
			nosEngine.LogI("ReOriginTrack: Origin cleared");
			return NOS_RESULT_SUCCESS;
		};

		return NOS_RESULT_SUCCESS;
	}
};

void RegisterReOriginTrack(nosNodeFunctions* fn)
{
	NOS_BIND_NODE_CLASS(NOS_NAME("nos.track.ReOriginTrack"), ReOriginTrackContext, fn);
}

} // namespace nos::track
