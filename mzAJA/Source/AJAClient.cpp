#include "AJAClient.h"
#include "CopyThread.h"
#include "Ring.h"
#include "glm/glm.hpp"
#include <random>
#include <uuid.h>

namespace mz
{

fb::UUID GenerateUUID()
{
    static std::mt19937 eng = std::mt19937(std::random_device()());
    static uuids::uuid_random_generator gen (&eng);
    return *(fb::UUID*)gen().as_bytes().data();
}

static NTV2Channel ParseChannel(std::string const &name)
{
    size_t idx = name.find("Link");
    return NTV2Channel(name[idx + sizeof("Link")] - '1');
}

std::vector<u8> StringValue(std::string const &str)
{
    return std::vector<u8>((u8 *)str.data(), (u8 *)str.data() + str.size() + 1);
}

std::string GetQuadName(NTV2Channel channel)
{
    const char *links[8] = {"1234", "5678"};
    return (std::string) "QuadLink " + links[channel / 4];
}

std::string GetChannelStr(NTV2Channel channel, AJADevice::Mode mode)
{
    switch (mode)
    {
    default:
        return GetQuadName(channel);
    case AJADevice::SL:
        return "SingleLink " + std::to_string(channel + 1);
    }
}

const u8 *AddIfNotFound(std::string name, std::string tyName, std::vector<u8> val,
                        std::map<std::string, const mz::fb::Pin *> &pins,
                        std::vector<flatbuffers::Offset<mz::fb::Pin>> &toAdd, flatbuffers::FlatBufferBuilder &fbb,
                        mz::fb::ShowAs showAs, mz::fb::CanShowAs canShowAs)
{
    if (auto pin = pins[name])
    {
        return pin->data()->Data();
    }
    toAdd.push_back(
        mz::fb::CreatePinDirect(fbb, generator(), name.c_str(), tyName.c_str(), showAs, canShowAs, 0, 0, &val));
    return 0;
}

template <class K, class V> using SeqMap = std::vector<std::pair<K, V>>;

auto EnumerateFormats()
{
    struct FormatDescriptor
    {
        NTV2VideoFormat fmt;
        NTV2FrameRate fps;
        u32 w, h;
        u8 Interlaced : 1;
        u8 ALevel : 1;
        u8 BLevel : 1;
    };

    std::map<u64, std::map<NTV2FrameRate, std::vector<FormatDescriptor>>> re;

    for (auto fmt = NTV2_FORMAT_FIRST_HIGH_DEF_FORMAT; fmt < NTV2_MAX_NUM_VIDEO_FORMATS; fmt = NTV2VideoFormat(fmt + 1))
    {
        if (IsPSF(fmt))
            continue;
        u32 w = GetDisplayWidth(fmt);
        u32 h = GetDisplayHeight(fmt);
        bool i = !IsProgressiveTransport(fmt);
        NTV2FrameRate fps = GetNTV2FrameRateFromVideoFormat(fmt);
        auto desc = FormatDescriptor{
            .fmt = fmt,
            .fps = fps,
            .w = w,
            .h = h,
            .Interlaced = !IsProgressiveTransport(fmt),
            .ALevel = NTV2_VIDEO_FORMAT_IS_A(fmt),
            .BLevel = NTV2_VIDEO_FORMAT_IS_B(fmt),
        };

        u64 extent = ((u64(w) << u64(32)) | u64(h));
        re[extent][fps].push_back(desc);
    }

    SeqMap<glm::uvec2, SeqMap<f64, std::vector<FormatDescriptor>>> re2;

    std::transform(re.begin(), re.end(), std::back_inserter(re2), [](auto &p) {
        auto extent = p.first;
        auto &container = p.second;
        SeqMap<f64, std::vector<FormatDescriptor>> XX;
        std::transform(container.begin(), container.end(), std::back_inserter(XX),
                       [](auto &p) { return std::pair(GetFramesPerSecond(p.first), std::move(p.second)); });
        std::sort(XX.begin(), XX.end(), [](auto &a, auto &b) {
            if (a.first == 50)
                return true;
            if (b.first == 50)
                return false;
            return a.first > b.first;
        });
        return std::pair(glm::uvec2((extent >> 32) & 0xFFFFFFFF, extent & 0xFFFFFFFF), std::move(XX));
    });

    std::sort(re2.begin(), re2.end(), [](auto &a, auto &b) {
        if (a.first.x == 1920)
            return true;
        if (b.first.x == 1920)
            return false;
        return a.first.x > b.first.x;
    });
    return re2;
}

AJAClient::AJAClient(bool input, AJADevice *device) : Input(input), Device(device)
{
    Ctx.Add(this);
    (input ? device->HasInput : device->HasOutput) = true;
    device->GetReferenceAndFrameRate(Ref, FR);
}

AJAClient::~AJAClient()
{
    Ctx.Remove(this);
}

u32 AJAClient::BitWidth() const
{
    switch (Shader)
    {
    case ShaderType::Comp10:
        return 10;
    default:
        return 8;
    }
}

PinMapping *AJAClient::operator->()
{
    return &Mapping;
}

fb::UUID AJAClient::GetPinId(std::string const &pinName) const
{
    return Mapping.GetPinId(pinName);
}

void AJAClient::GeneratePinIDSet(std::string const &pinName, AJADevice::Mode mode, std::vector<mz::fb::UUID> &ids)
{

    ids.push_back(GetPinId(pinName));
    ids.push_back(GetPinId(pinName + " Ring Size"));
    ids.push_back(GetPinId(pinName + " Ring Spare Count"));
    ids.push_back(GetPinId(pinName + " Video Format"));
    ids.push_back(GetPinId(pinName + " Colorspace"));
    ids.push_back(GetPinId(pinName + " Gamma Curve"));
    ids.push_back(GetPinId(pinName + " Narrow Range"));

    if (AJADevice::IsQuad(mode))
        ids.push_back(GetPinId(pinName + " Mode"));
}

std::vector<mz::fb::UUID> AJAClient::GeneratePinIDSet(std::string const &pinName, AJADevice::Mode mode)
{
    std::vector<mz::fb::UUID> ids;
    GeneratePinIDSet(pinName, mode, ids);
    return ids;
}

std::shared_ptr<CopyThread> AJAClient::FindChannel(NTV2Channel channel)
{
    for (auto &p : Pins)
    {
        if (p->Channel == channel)
        {
            return p;
        }
    }
    return 0;
}

NTV2FrameBufferFormat AJAClient::FBFmt() const
{
    return Shader == ShaderType::Comp10 ? NTV2_FBF_10BIT_YCBCR : NTV2_FBF_8BIT_YCBCR;
}

void AJAClient::StopAll()
{
    for (auto& th : Pins)
        th->Stop();
}

void AJAClient::StartAll()
{
    for (auto& th : Pins)
        th->StartThread();
}

void AJAClient::UpdateDeviceStatus()
{
    UpdateReferenceValue();
    if (!Input)
        return;

    bool mfmt = false;
    if (Device->GetMultiFormatMode(mfmt))
    {
        if (!mfmt)
        {
            Device->SetMultiFormatMode(true);
        }
    }
}

void AJAClient::UpdateDeviceValue()
{
    flatbuffers::FlatBufferBuilder fbb;
    auto pinId = GetPinId("Device");
    std::vector<u8> value = StringValue(Device->GetDisplayName());
	mzEngine.HandleEvent(CreateAppEvent(fbb, mz::CreatePinValueChangedDirect(fbb, &pinId, &value)));
    UpdateReferenceValue();
}

void AJAClient::UpdateReferenceValue()
{
    if (Input)
        return;
    auto oldRef = Ref;
    auto oldFr = FR;
    Device->GetReferenceAndFrameRate(Ref, FR);
    if (oldRef == Ref && oldFr == FR)
        return;

    flatbuffers::FlatBufferBuilder fbb;
    mz::fb::UUID pinId;
    if (auto id = Mapping.PinName2Id.right("ReferenceSource"))
        pinId = *id;
    else
        return;

    std::vector<u8> value = StringValue(NTV2ReferenceSourceToString(Ref, true));
    mzEngine.HandleEvent(CreateAppEvent(fbb, mz::CreatePinValueChangedDirect(fbb, &pinId, &value)));
    UpdateStatus();
}

void AJAClient::UpdateStatus()
{
    std::vector<flatbuffers::Offset<mz::fb::NodeStatusMessage>> msg;
    flatbuffers::FlatBufferBuilder fbb;
    UpdateStatus(fbb, msg);
    mzEngine.HandleEvent(CreateAppEvent(
        fbb, mz::CreatePartialNodeUpdateDirect(fbb, &Mapping.NodeId, ClearFlags::NONE, 0, 0, 0, 0, 0, 0, &msg)));
}

void AJAClient::UpdateStatus(flatbuffers::FlatBufferBuilder &fbb,
                             std::vector<flatbuffers::Offset<mz::fb::NodeStatusMessage>> &msg)
{
    msg.push_back(
        fb::CreateNodeStatusMessageDirect(fbb, Device->GetDisplayName().c_str(), fb::NodeStatusMessageType::INFO));

    if (!Input)
    {
        Device->GetReferenceAndFrameRate(Ref, FR);
        msg.push_back(fb::CreateNodeStatusMessageDirect(
            fbb, ("Ref : " + NTV2ReferenceSourceToString(Ref, true) + " - " + NTV2FrameRateToString(FR, true)).c_str(),
            fb::NodeStatusMessageType::INFO));
    }

    // services.HandleEvent(CreateAppEvent(fbb, mz::CreatePartialNodeUpdateDirect(fbb, &mapping.NodeId,
    // ClearFlags::NONE, 0, 0, 0, 0, 0, 0, &msg)));
}

void AJAClient::SetReference(std::string const &val)
{
    auto src = NTV2_REFERENCE_INVALID;
    if (isdigit(val.back()))
    {
        src = AJADevice::ChannelToRefSrc(NTV2Channel(val.back() - '1'));
    }
    else if (val == "Free Run")
    {
        src = NTV2_REFERENCE_FREERUN;
    }
    else if (val == "Reference In")
    {
        src = NTV2_REFERENCE_EXTERNAL;
    }
    if (src != NTV2_REFERENCE_INVALID)
    {
        Device->SetReference(src);
    }
    this->Ref = src;
}

void AJAClient::OnNodeUpdate(mz::fb::Node const &event)
{
    PinMapping mapping;
    auto pins = mapping.Load(event);
    std::vector<mz::fb::UUID> pinsToDelete;
    OnNodeUpdate(std::move(mapping), pins, pinsToDelete);
    if (!pinsToDelete.empty())
    {
        flatbuffers::FlatBufferBuilder fbb;
        mzEngine.HandleEvent(
            CreateAppEvent(fbb, mz::CreatePartialNodeUpdateDirect(fbb, &mapping.NodeId, ClearFlags::NONE, &pinsToDelete,
                                                                  0, 0, 0, 0, 0, 0)));
    }
}

void AJAClient::OnNodeUpdate(PinMapping &&newMapping, std::map<std::string, const mz::fb::Pin *> &tmpPins,
                             std::vector<mz::fb::UUID> &pinsToDelete)
{
    Mapping = std::move(newMapping);

    struct StreamData
    {
        const mz::fb::Pin *pin = 0;
        const mz::fb::Pin *size = 0;
        const mz::fb::Pin *spare_count = 0;
        const mz::fb::Pin *frame_rate = 0;
        const mz::fb::Pin *quad_mode = 0;
        const mz::fb::Pin *colorspace = 0;
        const mz::fb::Pin *curve = 0;
        const mz::fb::Pin *narrow_range = 0;
    };

    std::map<NTV2Channel, StreamData> prs;

    for (auto &[name, pin] : tmpPins)
    {
        if (!pin)
            continue;
        const auto tyname = pin->type_name()->str();
        NTV2Channel channel = ParseChannel(name);
        if (name.ends_with("Ring Size"))
        {
            prs[channel].size = pin;
        }
        else if (name.ends_with("Ring Spare Count"))
        {
            prs[channel].spare_count = pin;
        }
        else if (tyname == "mz.fb.Texture")
        {
            prs[channel].pin = pin;
        }
        else if (tyname == "mz.fb.VideoFormat")
        {
            prs[channel].frame_rate = pin;
        }
        else if (tyname == "mz.fb.QuadLinkMode" || tyname == "mz.fb.QuadLinkInputMode")
        {
            prs[channel].quad_mode = pin;
        }
        else if (tyname == "AJA.Colorspace")
        {
            prs[channel].colorspace = pin;
        }
        else if (tyname == "AJA.GammaCurve")
        {
            prs[channel].curve = pin;
        }
        else if (name.ends_with("Narrow Range"))
        {
            prs[channel].narrow_range = pin;
        }
    }

    {
        auto tmp = Pins;
        for (auto &th : tmp)
        {
            if (!Mapping.PinName2Id.left(th->id))
            {
                DeleteTexturePin(th);
            }
        }
    }

    for (auto [channel, pr] : prs)
    {
        auto pin = pr.pin;
        auto tex = flatbuffers::GetRoot<mz::fb::Texture>(pin->data()->Data());
        auto id = *pin->id();
        auto it = Pins.find(id);

        if (it != Pins.end())
        {
            // it->second->Restart(tex);
        }
        else
        {
            NTV2VideoFormat fmt = NTV2_FORMAT_UNKNOWN;
            switch (pin->show_as())
            {
            case mz::fb::ShowAs::INPUT_PIN: {
                if (pr.frame_rate && flatbuffers::IsFieldPresent(pr.frame_rate, mz::fb::Pin::VT_DATA))
                {
                    fmt = (NTV2VideoFormat)(*pr.frame_rate->data()->Data());
                    mzEngine.Log(("AJA: Route output " + NTV2ChannelToString(channel, true) + " with framerate " +
								  NTV2VideoFormatToString(fmt, true))
									 .c_str(),
                                 "");
                }
                break;
            }

            case mz::fb::ShowAs::OUTPUT_PIN:
				mzEngine.Log(("AJA: Route input " + NTV2ChannelToString(channel, true)).c_str(), "");
                break;
            }
            auto mode = pr.quad_mode ? *(AJADevice::Mode *)pr.quad_mode->data()->Data() : AJADevice::SL;
            if (Device->RouteSignal(channel, fmt, Input, mode, FBFmt()))
            {
                auto cs = *(Colorspace *)pr.colorspace->data()->Data();
                auto gc = *(GammaCurve *)pr.curve->data()->Data();
                auto range = *(bool *)pr.narrow_range->data()->Data();
                auto spareCount = pr.spare_count ? *(u32*)pr.spare_count->data()->Data() : 0;

                AddTexturePin(pin, *(u32*)pr.size->data()->Data(), channel, tex, fmt, mode, cs, gc, range, spareCount);
            }
            else
            {
                GeneratePinIDSet(pin->name()->str(), mode, pinsToDelete);
                auto it = Pins.find(id);
                DeleteTexturePin(*it);
                continue;
            }
        }
    }
}

void AJAClient::OnPinMenuFired(MzContextMenuRequest const &request)
{
    flatbuffers::FlatBufferBuilder fbb;
    auto name = Mapping.GetPinName(*request.item_id());

    if (auto pin = FindChannel(ParseChannel(name)))
    {
        std::vector<flatbuffers::Offset<mz::ContextMenuItem>> remove = {
            mz::CreateContextMenuItemDirect(fbb, "Remove",
                                            AjaAction{
                                                .Action = AjaAction::DELETE_CHANNEL,
                                                .DeviceIndex = Device->GetIndexNumber(),
                                                .Channel = pin->Channel,
                                            })};

        mzEngine.HandleEvent(CreateAppEvent(
            fbb, mz::CreateContextMenuUpdateDirect(fbb, request.item_id(), request.pos(), request.instigator(), &remove)));
    }
}

void AJAClient::OnMenuFired(MzContextMenuRequest const&request)
{
    if (0 != memcmp(request.item_id(), &Mapping.NodeId, 16))
    {
        return OnPinMenuFired(request);
    }

    flatbuffers::FlatBufferBuilder fbb;

    std::vector<flatbuffers::Offset<mz::ContextMenuItem>> items;
    std::vector<flatbuffers::Offset<mz::ContextMenuItem>> devices;

    for (auto &d : AJADevice::Devices)
    {
        if (d.get() != Device && ((Input && !d->HasInput) || (!Input && !d->HasOutput)))
        {
            devices.push_back(mz::CreateContextMenuItemDirect(fbb, d->GetDisplayName().c_str(),
                                                              AjaAction{
                                                                  .Action = AjaAction::SELECT_DEVICE,
                                                                  .DeviceIndex = d->GetIndexNumber(),
                                                              }));
        }
    }

    static auto Descriptors = EnumerateFormats();

    for (u32 i = NTV2_CHANNEL1; i < NTV2_MAX_NUM_CHANNELS; ++i)
    {

        AJADevice::Mode modes[2] = {AJADevice::SL, AJADevice::AUTO};
        for (auto mode : modes)
        {
            NTV2Channel channel = NTV2Channel(AJADevice::SL == mode ? i : NTV2_MAX_NUM_CHANNELS - i - 1);
            AjaAction action = {
                .Action = (AJADevice::SL == mode) ? AjaAction::ADD_CHANNEL : AjaAction::ADD_QUAD_CHANNEL,
                .DeviceIndex = Device->GetIndexNumber(),
                .Channel = channel,
            };
            auto it = AJADevice::SL != mode ? items.begin() : items.end();
            auto channelStr = GetChannelStr(channel, mode);
            if (Input)
            {
                if (Device->ChannelIsValid(channel, Input, NTV2_FORMAT_UNKNOWN, mode))
                {
                    items.insert(it, mz::CreateContextMenuItemDirect(fbb, channelStr.c_str(), action));
                }
            }
            else
            {
                std::vector<flatbuffers::Offset<mz::ContextMenuItem>> extents;
                for (auto &[extent, Container0] : Descriptors)
                {
                    std::vector<flatbuffers::Offset<mz::ContextMenuItem>> frameRates;
                    for (auto &[fps, Container1] : Container0)
                    {
                        std::vector<flatbuffers::Offset<mz::ContextMenuItem>> formats;
                        for (auto &desc : Container1)
                        {
                            if (Device->ChannelIsValid(channel, false, desc.fmt, mode))
                            {
                                action.Format = desc.fmt;
                                std::string name = (desc.Interlaced ? "Interlaced" : "Progressive");
                                if (desc.ALevel)
                                    name += "-A";
                                if (desc.BLevel)
                                    name += "-B";
                                formats.push_back(mz::CreateContextMenuItemDirect(fbb, name.c_str(), action));
                            }
                        }

                        if (!formats.empty())
                        {
                            GetNTV2FrameRateFromNumeratorDenominator(0, 0);
                            char buf[16] = {};
                            std::sprintf(buf, "%.2f", fps);
                            frameRates.push_back(mz::CreateContextMenuItemDirect(fbb, buf, 0, &formats));
                        }
                    }

                    if (!frameRates.empty())
                    {
                        char buf[32] = {};
                        std::sprintf(buf, "%dx%d", extent.x, extent.y);
                        extents.push_back(mz::CreateContextMenuItemDirect(fbb, buf, 0, &frameRates));
                    }
                }

                if (!extents.empty())
                {
                    items.insert(it, mz::CreateContextMenuItemDirect(fbb, channelStr.c_str(), 0, &extents));
                }
            }
        }
    }

    if (!devices.empty())
    {
        items.push_back(mz::CreateContextMenuItemDirect(fbb, "Select Device", 0, &devices));
    }

    if (items.empty())
    {
        return;
    }

    mzEngine.HandleEvent(CreateAppEvent(
        fbb, mz::CreateContextMenuUpdateDirect(fbb, &Mapping.NodeId, request.pos(), request.instigator(), &items)));
}

void AJAClient::OnCommandFired(u32 cmd)
{
    if (!cmd)
    {
        return;
    }
    AjaAction action = (AjaAction &)cmd;
    auto channel = action.Channel;

    switch (action.Action)
    {
    case AjaAction::SELECT_DEVICE: {
        for (auto& pin : Pins)
        {
            pin->SendDeleteRequest();
        }

        auto newDev = AJADevice::GetDevice(action.DeviceIndex).get();
        ;
        (Input ? Device->HasInput : Device->HasOutput) = false;
        Device = newDev;
        (Input ? Device->HasInput : Device->HasOutput) = true;
        UpdateDeviceValue();
        UpdateStatus();
        break;
    }
    case AjaAction::DELETE_CHANNEL:
        for (auto &pin : Pins)
        {
            if (pin->Channel == channel)
            {
                pin->SendDeleteRequest();
                break;
            }
        }
        break;

    case AjaAction::ADD_QUAD_CHANNEL:
    case AjaAction::ADD_CHANNEL: {
        const bool isQuad = action.Action == AjaAction::ADD_QUAD_CHANNEL;
        const AJADevice::Mode mode = isQuad ? (Input ? AJADevice::AUTO : AJADevice::TSI) : AJADevice::SL;
        NTV2VideoFormat format = action.Format;
        if (Input)
        {
            format = Device->GetInputVideoFormat(action.Channel);
        }

        u32 width = 1920 * (1 + isQuad);
        u32 height = 1080 * (1 + isQuad);
        Device->GetExtent(format, mode, width, height);
        bool validates = !IsProgressiveTransport(format); // interlaced input and output both validate

        mz::fb::TTexture tex;
        tex.size = mz::fb::SizePreset::CUSTOM;
        tex.width = width;
        tex.height = height;
        tex.unscaled = true;    // Prevent auto-scaling when an output pin is connected to this pin.
        tex.unmanaged = !Input; // do not create resource for this pin, do not assign test signal as well
        tex.format = mz::fb::Format::R16G16B16A16_UNORM;
        mz::fb::ShowAs showAs = Input ? mz::fb::ShowAs::OUTPUT_PIN : mz::fb::ShowAs::INPUT_PIN;
        mz::fb::CanShowAs canShowAs = Input ? mz::fb::CanShowAs::OUTPUT_PIN_ONLY : mz::fb::CanShowAs::INPUT_PIN_ONLY;
        std::string pinName = (isQuad ? GetQuadName(channel) : ("SingleLink " + std::to_string(channel + 1)));
        std::vector<u8> data = mz::Buffer::From(tex);
        std::vector<u8> ringData = mz::Buffer::From(2);
        std::vector<u8> spareCountData = mz::Buffer::From(0);
        std::vector<u8> ringDataMin = mz::Buffer::From(1);
        std::vector<u8> ringDataMax = mz::Buffer::From(120);
        flatbuffers::FlatBufferBuilder fbb;
        std::vector<u8> fmtData = mz::Buffer::From(format);
        std::vector<u8> colorspaceData = mz::Buffer::From(Colorspace::REC709);
        std::vector<u8> curveData = mz::Buffer::From(GammaCurve::REC709);
        std::vector<u8> narrowRangeData = mz::Buffer::From(true);

        std::vector<flatbuffers::Offset<mz::fb::Pin>> pins = {
            mz::fb::CreatePinDirect(fbb, generator(), pinName.c_str(), "mz.fb.Texture", showAs, canShowAs, 0, 0, &data,
                                    0, 0, 0, 0, 0, 0, 0, 0, 0, true, fb::PinContents::NONE, 0, 0, validates),
            mz::fb::CreatePinDirect(fbb, generator(), (pinName + " Ring Size").c_str(), "uint",
                                    mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY, 0, 0,
                                    &ringData, 0, &ringDataMin, &ringDataMax, nullptr, .0f, Input),
            mz::fb::CreatePinDirect(fbb, generator(), (pinName + " Ring Spare Count").c_str(), "uint",
                                    mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY, 0, 0,
                                    &spareCountData, 0, &spareCountData, &ringDataMax, nullptr, .0f),
            mz::fb::CreatePinDirect(fbb, generator(), (pinName + " Video Format").c_str(), "mz.fb.VideoFormat",
                                    mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::OUTPUT_PIN_OR_PROPERTY, 0, 0, &fmtData,
                                    0, 0, 0, 0, 0, true),
            mz::fb::CreatePinDirect(fbb, generator(), (pinName + " Colorspace").c_str(), "AJA.Colorspace",
                                    mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::INPUT_OUTPUT_PROPERTY, 0, 0,
                                    &colorspaceData, 0, 0, 0, 0, 0, false),
            mz::fb::CreatePinDirect(fbb, generator(), (pinName + " Gamma Curve").c_str(), "AJA.GammaCurve",
                                    mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::INPUT_OUTPUT_PROPERTY, 0, 0,
                                    &curveData, 0, 0, 0, 0, 0, false),
            mz::fb::CreatePinDirect(fbb, generator(), (pinName + " Narrow Range").c_str(), "bool",
                                    mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::INPUT_OUTPUT_PROPERTY, 0, 0,
                                    &narrowRangeData, 0, 0, 0, 0, 0, false),
        };

        if (isQuad)
        {
            std::vector<u8> data = mz::Buffer::From(mode);
            pins.push_back(mz::fb::CreatePinDirect(
                fbb, generator(), (pinName + " Mode").c_str(), Input ? "mz.fb.QuadLinkInputMode" : "mz.fb.QuadLinkMode",
                mz::fb::ShowAs::PROPERTY, mz::fb::CanShowAs::PROPERTY_ONLY, 0, 0, &data));
        }

        mzEngine.HandleEvent(
            CreateAppEvent(fbb, mz::CreatePartialNodeUpdateDirect(fbb, &Mapping.NodeId, ClearFlags::NONE, 0, &pins)));
        break;
    }
    }
}

void AJAClient::OnNodeRemoved()
{
    for (auto& th : Pins)
        // Because there can be references to the CopyThread shared pointers in 'pins' list,
        // CopyThread destructor will not be called in the subsequent pins.clear call. So we need to stop the thread
        // here to avoid accessing the device (in CopyThread) after it has been destroyed.
        th->Stop();
    Pins.clear();
    (Input ? Device->HasInput : Device->HasOutput) = false;
}

struct AjaRestartCommandParams {
    enum Flags : u32 {
        UpdateRingSize = 1 << 0,
        UpdateSpareCount = 1 << 1,
    };
    u32 UpdateFlags;
    u32 RingSize;
    u32 SpareCount;
};

void AJAClient::OnPathCommand(mz::fb::UUID pinID, app::PathCommand command, Buffer params)
{
	auto result = Pins.find(mz::UUID(pinID));
	if (result == Pins.end())
	{
        mzEngine.Log("path command on unknown pin: %s", UUID2STR(pinID).c_str());
		return;
	}
    auto ringThread = *result;

    switch (command)
    {
    case app::PathCommand::RESTART: {
        auto* res = params.As<AjaRestartCommandParams>();
        if (res->UpdateFlags & AjaRestartCommandParams::UpdateRingSize) 
            ringThread->Resize(res->RingSize);
        break;
    }
    case app::PathCommand::NOTIFY_NEW_CONNECTION:
        {
            if (ringThread->IsInput() ||
                !ringThread->th.joinable())
                break;

            // there is a new connection path
            // we need to send a restart signal
		    flatbuffers::FlatBufferBuilder fbb;
            auto id = GetPinId(ringThread->Name());
            AjaRestartCommandParams restartParams;
            restartParams.RingSize = ringThread->gpuRing->Size;
            restartParams.UpdateFlags = AjaRestartCommandParams::UpdateRingSize;
		    UByteSequence byteBuffer = Buffer::From(restartParams);
		    mzEngine.HandleEvent(CreateAppEvent(fbb, app::CreateExecutePathCommandDirect(fbb, &id, app::PathCommand::RESTART, app::PathCommandType::WALKBACK,  &byteBuffer)));
            break;
        }
    }
}

void AJAClient::OnPinValueChanged(mz::fb::UUID id, void *value)
{
    if (!value)
    {
        return;
    }
    auto pinName = Mapping.GetPinName(id);

    if (pinName == "Shader Type")
    {
        StopAll();
        Shader = *(ShaderType *)value;
        for (auto& th : Pins)
        {
            th->Refresh();
            th->UpdateCurve(th->GammaCurve);
        }
        StartAll();
        return;
    }

    if (pinName == "Dispatch Size")
    {
        DispatchSizeX = ((mz::fb::vec2u *)value)->x();
        DispatchSizeY = ((mz::fb::vec2u *)value)->y();
        return;
    }

    if (pinName == "Debug")
    {
        Debug = *(u32 *)value;
        return;
    }

    if (!Input && pinName == "ReferenceSource")
    {
        SetReference((char *)value);
        UpdateStatus();
        return;
    }

    if (pinName.ends_with("Narrow Range"))
    {
        FindChannel(ParseChannel(pinName))->NarrowRange = *(bool *)value;
        return;
    }

    if (pinName.ends_with("Colorspace"))
    {
        FindChannel(ParseChannel(pinName))->Colorspace = *(Colorspace *)value;
        return;
    }

    if (pinName.ends_with("Gamma Curve"))
    {
        FindChannel(ParseChannel(pinName))->UpdateCurve(*(GammaCurve *)value);
        return;
    }

    if (pinName.ends_with("Mode"))
    {
        auto pin = FindChannel(ParseChannel(pinName));
        auto mode = *(AJADevice::Mode*)value;
        if (mode != pin->mode)
        {
            pin->Stop();
            pin->mode = mode;
            pin->Refresh();
            pin->StartThread();
        }
        return;
    }

    if (pinName.ends_with("Ring Size"))
    {
        flatbuffers::FlatBufferBuilder fbb;
        AjaRestartCommandParams restartParams;
        restartParams.RingSize = *(u32*)value;
        restartParams.UpdateFlags = AjaRestartCommandParams::UpdateRingSize;
        UByteSequence byteBuffer = Buffer::From(restartParams);
        auto id = GetPinId(std::string(pinName.begin(), pinName.end() - sizeof("Ring Size")));
        mzEngine.HandleEvent(CreateAppEvent(fbb, app::CreateExecutePathCommandDirect(fbb, &id, app::PathCommand::RESTART, app::PathCommandType::WALKBACK, &byteBuffer)));
    }
    if (pinName.ends_with("Ring Spare Count"))
    {
		auto channel = ParseChannel(pinName);
		auto pin = FindChannel(channel);
        pin->SpareCount = *(u32*)value;
    }
}

void AJAClient::OnExecute()
{
}

bool AJAClient::BeginCopyFrom(MzCopyInfo &cpy)
{
    GPURing::Resource *slot = 0;
    auto it = Pins.find(cpy.ID); 
    if (it == Pins.end()) 
        return true;
    
    auto th = *it;
	if ((slot = th->gpuRing->TryPop(cpy.FrameNumber, th->SpareCount)))
	{
		cpy.CopyTextureFrom = slot->Res;

        slot->Res;
        cpy.ShouldSetSourceFrameNumber = true;

		//if (-1 == *cpy.PathState)
		//	*cpy.PathState = (uint32_t)flatbuffers::GetRoot<fb::Texture>(cpy.CopyTextureFrom.Data)->field_type();
		//else
		//{
		//	*cpy.PathState &= (uint32_t)slot->Res->field_type();
		//}
	}
    return cpy.ShouldCopyTexture = !!(cpy.Data = slot);
}

bool AJAClient::BeginCopyTo(MzCopyInfo &cpy)
{
    GPURing::Resource *slot = 0;
    auto it = Pins.find(cpy.ID); 
    if (it == Pins.end()) 
        return true;
    
    auto th = *it;
	if ((slot = th->gpuRing->BeginPush()))
	{
		slot->FrameNumber = cpy.FrameNumber;
		if (-1 != *cpy.PathState)
		{
            // slot->Res->mutate_field_type(fb::FieldType(*cpy.PathState));
		}

        cpy.CopyTextureTo = slot->Res;
	}
    return cpy.ShouldCopyTexture = !!(cpy.Data = slot);
}

void AJAClient::EndCopyFrom(MzCopyInfo &cpy)
{
    if(!cpy.Data) 
        return;
    auto it = Pins.find(cpy.ID); 
    if (it == Pins.end()) 
        return;

    auto th = *it;
    auto res = (GPURing::Resource *)cpy.Data;
    th->gpuRing->EndPop(res);
}

void AJAClient::EndCopyTo(MzCopyInfo &cpy)
{
    if(!cpy.Data) 
        return;
    auto it = Pins.find(cpy.ID); 
    if (it == Pins.end()) 
        return;

    auto th = *it;
    auto res = (GPURing::Resource *)cpy.Data;
	// if (-1 != *cpy.PathState) res->Res->mutate_field_type((mz::fb::FieldType)*cpy.PathState);
    th->gpuRing->EndPush(res);
    cpy.Stop = th->cpuRing->IsFull();
    th->Worker->Enqueue({});
}

void AJAClient::AddTexturePin(const mz::fb::Pin* pin, u32 ringSize, NTV2Channel channel,
    const fb::Texture* tex, NTV2VideoFormat fmt, AJADevice::Mode mode, Colorspace cs, GammaCurve gc, bool range, unsigned spareCount)
{
    auto th = MakeShared<CopyThread>(*pin->id(), this, ringSize, spareCount, 
                                     pin->show_as(), channel, fmt, mode, cs, gc, range, tex);
    Pins.insert(std::move(th));
}

void AJAClient::DeleteTexturePin(rc<CopyThread> const& c)
{
    c->Stop();
    Pins.erase(c);
}

} // namespace mz