﻿#pragma once

#include <Nodos/PluginHelpers.hpp>

#include <ntv2enums.h>

#include "AJA_generated.h"
#include "AJADevice.h"

namespace nos::aja
{
struct AJASelectChannelCommand
{
	uint32_t DeviceIndex : 4;
	NTV2Channel Channel : 5;
	NTV2VideoFormat Format : 12;
	uint32_t Input : 1;
	uint32_t IsQuad : 1;
	operator uint32_t() const { return *(uint32_t*)this; }
};
static_assert(sizeof(AJASelectChannelCommand) == sizeof(uint32_t));

struct Channel
{
	nosUUID ChannelPinId;
	NodeContext* Context;

	Channel(NodeContext* context) : Context(context) {}

	TChannelInfo Info{};

	std::shared_ptr<AJADevice> GetDevice() const;

	NTV2Channel GetChannel() const;

	AJADevice::Mode GetMode() const;

	bool Open();

	void Close() const;

	bool Update(TChannelInfo newChannelInfo, bool setPinValue);

	void SetStatus(fb::NodeStatusMessageType type, std::string text);
};

void EnumerateOutputChannels(flatbuffers::FlatBufferBuilder& fbb, std::vector<flatbuffers::Offset<nos::ContextMenuItem>>& devices);
void EnumerateInputChannels(flatbuffers::FlatBufferBuilder& fbb, std::vector<flatbuffers::Offset<nos::ContextMenuItem>>& devices);
}