// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <stddef.h>
#include <iostream>
#include <stdint.h>
#include <memory>
#include <utility>
#include <vector>
#include <absl/memory/memory.h>
#include <absl/types/optional.h>
#include <api/audio/audio_mixer.h>
#include <api/audio_codecs/audio_decoder_factory.h>
#include <api/audio_codecs/audio_encoder_factory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/audio_options.h>
#include <api/create_peerconnection_factory.h>
#include <api/rtp_sender_interface.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/video_decoder_factory.h>
#include <api/video_codecs/video_encoder_factory.h>
#include <modules/audio_device/include/audio_device.h>
#include <modules/audio_processing/include/audio_processing.h>
#include <modules/video_capture/video_capture.h>
#include <modules/video_capture/video_capture_factory.h>
#include <p2p/base/port_allocator.h>
#include <pc/video_track_source.h>
#include <rtc_base/checks.h>
#include <rtc_base/logging.h>
#include <rtc_base/ref_counted_object.h>
#include <rtc_base/rtc_certificate_generator.h>
#include <rtc_base/strings/json.h>
#include <test/vcm_capturer.h>
#include <nlohmann/json.hpp>
#include "WebRTCManager.h"
#include "CustomVideoSource.h"
#include "WebRTCJsonConfig.h"
#include "Nodos/Modules.h"

using json = nlohmann::json;

nosWebRTCManager::nosWebRTCManager(nosWebRTCClient* client) :p_nosWebRTCClient(client)
{
    //TODO: switch to factory pattern in future for checking 
    //if this is succesful or not:
    if (!p_PeerConnectionFactory) {
        if (!SignalingThread) {
            SignalingThread = rtc::Thread::Create();
            SignalingThread->SetName("nosWebRTCSignalingThread",nullptr);
            SignalingThread->Start();
        }
        if (!WorkerThread) {
            WorkerThread = rtc::Thread::Create();
            WorkerThread->SetName("nosWebRTCWorkerThread", nullptr);
            WorkerThread->Start();
        }
		p_PeerConnectionFactory = webrtc::CreatePeerConnectionFactory(nullptr /* network_thread */,
																	  WorkerThread.get() /* worker_thread */,
																	  SignalingThread.get() /* signaling_thread */,
																	  nullptr /* default_adm */,
																	  webrtc::CreateBuiltinAudioEncoderFactory(),
																	  webrtc::CreateBuiltinAudioDecoderFactory(),
																	  webrtc::CreateBuiltinVideoEncoderFactory(),
																	  webrtc::CreateBuiltinVideoDecoderFactory(),
																	  nullptr /* audio_mixer */,
																	  nullptr /* audio_processing */);
    }


    RegisterToWebRTCClientCallbacks();
}

nosWebRTCManager::~nosWebRTCManager()
{
    Dispose();
}

bool nosWebRTCManager::MainLoop(int cms)
{
    /*if (SignalingThread) {
        SignalingThread->ProcessMessages(cms);
    }
    if (WorkerThread) {
        WorkerThread->ProcessMessages(cms);
    }*/
    return true;
}


bool nosWebRTCManager::AddPeerConnection(int& connectionID) {
    connectionID = -1;
    if (!p_PeerConnectionFactory)
        return false;
    
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.enable_dtls_srtp = true;
    webrtc::PeerConnectionInterface::IceServer server;
    server.uri = "stun:stun.l.google.com:19302"; //Use google STUN server for now.
    config.servers.push_back(server);

    {
        std::lock_guard lock(PeerConnectionsMutex);
        connectionID = NextPeerConnectionID++;
    }

    auto observer = std::make_shared<nosPeerConnectionObserver>(connectionID);
    RegisterToPeerConnectionObserverCallbacks(observer.get());

    rtc::scoped_refptr<nosCreateSDPObserver> createSDPObserver = new nosCreateSDPObserver(connectionID);
    RegisterToCreateSDPObserverCallbacks(createSDPObserver.get());

    rtc::scoped_refptr<nosSetSDPObserver> setSDPObserver = new nosSetSDPObserver(connectionID);
    RegisterToSetSDPObserverCallbacks(setSDPObserver.get());

    PeerConnectionPtr peerConnection = p_PeerConnectionFactory->CreatePeerConnection(
        config, nullptr, nullptr, observer.get());

    if (!peerConnection)
        return false;

    {
        nosPeerConnectionState state;
        state.Observer = observer;
        state.CreateSDPObserver = createSDPObserver;
        state.SetSDPObserver = setSDPObserver;
        state.PeerConnection = peerConnection;

        std::lock_guard lock(PeerConnectionsMutex);
        p_PeerConnections.emplace(connectionID, std::move(state));
    }
    
    //Add video tracks
    for (auto track : p_VideoTracks) {
        auto err = peerConnection->AddTrack(track, {"NodosStreamID"} );
        //TODO: handle errors?
    }

    return true;
}

void nosWebRTCManager::RemovePeerConnection(int connectionID)
{
    nosPeerConnectionState removedState;
    ReleasePeerConnectionState(connectionID, &removedState);

    if (!removedState.PeerConnection)
        return;

    if (removedState.Observer) {
        removedState.Observer->ClearCallbacks();
    }
    if (removedState.CreateSDPObserver) {
        removedState.CreateSDPObserver->ClearCallbacks();
    }
    if (removedState.SetSDPObserver) {
        removedState.SetSDPObserver->ClearCallbacks();
    }

    for (const auto& sender : removedState.PeerConnection->GetSenders()) {
        for (auto& videoTrack : p_VideoTracks) {
            if (videoTrack == sender->track()) {
                removedState.PeerConnection->RemoveTrack(sender);
            }
        }
    }
    removedState.PeerConnection->Close();
}

void nosWebRTCManager::Dispose()
{
    std::vector<nosPeerConnectionState> peerConnectionStates;
    {
        std::lock_guard lock(PeerConnectionsMutex);
        if (IsDisposed)
            return;

        IsDisposed = true;
        peerConnectionStates.reserve(p_PeerConnections.size());
        for (auto& [connectionID, state] : p_PeerConnections) {
            peerConnectionStates.push_back(std::move(state));
        }
        p_PeerConnections.clear();
        p_PeerIDToConnectionID.clear();
    }

    // PeerConnectionInterface keeps a raw PeerConnectionObserver pointer. Keep the
    // complete state alive until Close() has run and the WebRTC threads have stopped.
    for (auto& state : peerConnectionStates) {
        if (state.Observer) {
            state.Observer->ClearCallbacks();
        }
        if (state.CreateSDPObserver) {
            state.CreateSDPObserver->ClearCallbacks();
        }
        if (state.SetSDPObserver) {
            state.SetSDPObserver->ClearCallbacks();
        }

        if (!state.PeerConnection)
            continue;

        for (const auto& sender : state.PeerConnection->GetSenders()) {
            for (auto& videoTrack : p_VideoTracks) {
                if (videoTrack == sender->track()) {
                    state.PeerConnection->RemoveTrack(sender);
                    videoTrack->set_enabled(false);
                }
            }
        }
        state.PeerConnection->Close();
        state.PeerConnection = nullptr;
    }

    // scoped_refptr overloads operator= so that it releases the old pointer and
    // decrements its reference count when assigned a new one.
    for (auto& videoTrack : p_VideoTracks) {
        videoTrack = nullptr;
    }
    p_VideoTracks.clear();

    for (auto& videoSource : p_VideoSources) {
        videoSource = nullptr;
    }

    p_VideoSources.clear();
    p_PeerConnectionFactory = nullptr;

    if (SignalingThread) {
        SignalingThread->Stop();
    }
    if (WorkerThread) {
        WorkerThread->Stop();
    }

    p_nosWebRTCClient = nullptr;
}

void nosWebRTCManager::OnImageEncoded()
{
    ImageEncodeCompletedCallback();
}

void nosWebRTCManager::SendOffer(int id)
{
    int existingConnectionID = -1;
    if (TryGetConnectionIDFromPeerID(existingConnectionID, id)) {
        RemovePeerConnection(existingConnectionID);
    }

    int connectionID = -1;
    if (!AddPeerConnection(connectionID))
        return;

    AttachPeerIDToConnection(connectionID, id);

    nosPeerConnectionState state;
    if (!TryGetPeerConnectionState(connectionID, state) || !state.PeerConnection || !state.CreateSDPObserver)
        return;

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions offerOptions;
    offerOptions.offer_to_receive_video = 1;

    state.PeerConnection->CreateOffer(state.CreateSDPObserver.get(), offerOptions);
}

void nosWebRTCManager::UpdateBitrates(int bitrateKBPS)
{
    targetKbps = bitrateKBPS;

    for (auto& peerConnection : GetPeerConnectionsSnapshot()) {
        if (!peerConnection)
            continue;

        for (auto& sender : peerConnection->GetSenders()) {
            auto bitrate = sender->GetParameters();
            for (auto& encoding : bitrate.encodings) {
                encoding.max_framerate = 300;
                encoding.max_bitrate_bps = bitrateKBPS*10*1000;
                encoding.min_bitrate_bps = bitrateKBPS*5*1000;
            }
            sender->SetParameters(bitrate);
        }
    }
}

void nosWebRTCManager::AddVideoSource(rtc::scoped_refptr<nosCustomVideoSource> source)
{
    if (!p_PeerConnectionFactory)
        return;
    p_VideoSources.push_back(source);
    p_VideoTracks.push_back(p_PeerConnectionFactory->CreateVideoTrack(kVideoLabel, source));

}

void nosWebRTCManager::AddVideoSink(rtc::scoped_refptr<nosCustomVideoSink> sink)
{
    p_VideoSinks.push_back(sink);
}

void nosWebRTCManager::SetPeerConnectedCallback(std::function<void()> callback)
{
    PeerConnectedCallback = callback;
}

void nosWebRTCManager::SetPeerDisconnectedCallback(std::function<void()> callback)
{
    PeerDisconnectedCallback = callback;
}

void nosWebRTCManager::SetServerConnectionSuccesfulCallback(std::function<void()> callback)
{
    ServerConnectionSuccesfulCallback = callback;
}

void nosWebRTCManager::SetServerConnectionFailedCallback(std::function<void()> callback)
{
    ServerConnectionFailedCallback = callback;
}

void nosWebRTCManager::AddRef() const
{
    ref_count_.IncRef();
}

rtc::RefCountReleaseStatus nosWebRTCManager::Release() const
{
    const auto status = ref_count_.DecRef();
    if (status == rtc::RefCountReleaseStatus::kDroppedLastRef) {
        delete this;
    }
    return status;
}

#pragma region CreateSDPObserver Implementation
void nosWebRTCManager::RegisterToCreateSDPObserverCallbacks(nosCreateSDPObserver* observer)
{
    observer->SetSuccessCallback([this](auto desc, int id) {this->OnSDPCreateSuccess(desc, id); });
    observer->SetFailureCallback([this](auto error, int id) {this->OnSDPCreateFailure(error, id); });
}

void nosWebRTCManager::OnSDPCreateSuccess(webrtc::SessionDescriptionInterface* desc, int id)
{
    nosPeerConnectionState state;
    if (!TryGetPeerConnectionState(id, state) || !state.PeerConnection || !state.SetSDPObserver)
        return;

    if (state.PeerID < 0)
        return;

    state.PeerConnection->SetLocalDescription(state.SetSDPObserver.get(), desc);

    //TODO: there might be an issue if the encryption is disabled, so you may need to implement a loopback mechanism
    std::string SDP;
    desc->ToString(&SDP);
    json jsonSDP;
    size_t bwdth = SDP.find("b=AS:30");
    if (bwdth != std::string::npos) {
        std::string newbw = "b=AS:10000";
        SDP.replace(bwdth, 7, newbw);
    }

    jsonSDP[nosWebRTCJsonConfig::typeKey] = webrtc::SdpTypeToString(desc->GetType());
    jsonSDP[nosWebRTCJsonConfig::sdpKey] = SDP;
    jsonSDP[nosWebRTCJsonConfig::peerIDKey] = std::to_string(state.PeerID);
    
    p_nosWebRTCClient->SendMessageToServer(jsonSDP.dump());


    if (state.PeerConnection->GetSenders().size() > 0) {
        auto bitrate = state.PeerConnection->GetSenders()[0]->GetParameters();
        for (auto& encoding : bitrate.encodings) {
            encoding.max_framerate = 300;
            encoding.max_bitrate_bps = targetKbps * 2 * 10 * 1000;
            encoding.min_bitrate_bps = targetKbps * 2 * 5 * 1000;
        }
        state.PeerConnection->GetSenders()[0]->SetParameters(bitrate);
    }

}

void nosWebRTCManager::OnSDPCreateFailure(webrtc::RTCError error, int id)
{
    RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
}
#pragma endregion

void nosWebRTCManager::RegisterToSetSDPObserverCallbacks(nosSetSDPObserver* observer)
{
    observer->SetSuccessCallback([this](int id) {OnSDPSetSuccess(id); });
    observer->SetFailureCallback([this](auto error,int id) {OnSDPSetFailure(error, id); });
}

#pragma region SetSDP Callbacks
void nosWebRTCManager::OnSDPSetSuccess(int id)
{
    // no problem
}

void nosWebRTCManager::OnSDPSetFailure(webrtc::RTCError error, int id)
{
    //TODO: should handle some stuff for SDPSetFailure
}

bool nosWebRTCManager::TryGetPeerConnectionState(int connectionID, nosPeerConnectionState& state) const
{
    std::lock_guard lock(PeerConnectionsMutex);
    auto it = p_PeerConnections.find(connectionID);
    if (it == p_PeerConnections.end())
        return false;

    state = it->second;
    return true;
}

bool nosWebRTCManager::TryGetConnectionIDFromPeerID(int& connectionID, int peerID) const
{
    std::lock_guard lock(PeerConnectionsMutex);
    auto it = p_PeerIDToConnectionID.find(peerID);
    if (it == p_PeerIDToConnectionID.end())
        return false;

    connectionID = it->second;
    return true;
}

bool nosWebRTCManager::RemovePeerConnectionByPeerID(int peerID)
{
    int connectionID = -1;
    if (!TryGetConnectionIDFromPeerID(connectionID, peerID))
        return false;

    RemovePeerConnection(connectionID);
    return true;
}

void nosWebRTCManager::AttachPeerIDToConnection(int connectionID, int peerID)
{
    std::lock_guard lock(PeerConnectionsMutex);
    auto it = p_PeerConnections.find(connectionID);
    if (it == p_PeerConnections.end())
        return;

    if (it->second.PeerID >= 0) {
        p_PeerIDToConnectionID.erase(it->second.PeerID);
    }

    it->second.PeerID = peerID;
    p_PeerIDToConnectionID[peerID] = connectionID;
}

void nosWebRTCManager::ReleasePeerConnectionState(int connectionID, nosPeerConnectionState* removedState)
{
    std::lock_guard lock(PeerConnectionsMutex);
    auto it = p_PeerConnections.find(connectionID);
    if (it == p_PeerConnections.end())
        return;

    if (it->second.PeerID >= 0) {
        p_PeerIDToConnectionID.erase(it->second.PeerID);
    }

    if (removedState) {
        *removedState = std::move(it->second);
    }

    p_PeerConnections.erase(it);
}

std::vector<PeerConnectionPtr> nosWebRTCManager::GetPeerConnectionsSnapshot() const
{
    std::vector<PeerConnectionPtr> peerConnections;
    std::lock_guard lock(PeerConnectionsMutex);
    peerConnections.reserve(p_PeerConnections.size());
    for (const auto& [connectionID, state] : p_PeerConnections) {
        if (state.PeerConnection) {
            peerConnections.push_back(state.PeerConnection);
        }
    }
    return peerConnections;
}
#pragma endregion


#pragma region nosWebRTCClient Implementation
void nosWebRTCManager::RegisterToWebRTCClientCallbacks()
{
    if (!p_nosWebRTCClient)
        return;

    p_nosWebRTCClient->SetConnectionSuccesfulCallback([this]() {this->OnServerConnectionSuccesful(); });
    p_nosWebRTCClient->SetConnectionErrorCallback([this]() {this->OnServerConnectionError(); });
    p_nosWebRTCClient->SetConnectionClosedCallback([this]() {this->OnServerConnectionClosed(); });
    p_nosWebRTCClient->SetRawMessageReceivedCallback([this](void* data, size_t length) {this->OnRawMessageReceived(data, length); });
    p_nosWebRTCClient->SetSDPOfferReceivedCallback([this](std::string&& message) {this->OnSDPOfferReceived(std::move(message)); });
    p_nosWebRTCClient->SetSDPAnswerReceivedCallback([this](std::string&& message) {this->OnSDPAnswerReceived(std::move(message)); });
    p_nosWebRTCClient->SetICECandidateReceivedCallback([this](std::string&& message) {this->OnICECandidateReceived(std::move(message)); });
    p_nosWebRTCClient->SetPeerDisconnectedReceivedCallback([this](std::string&& message) {this->OnPeerDisconnectedReceived(std::move(message)); });
}

void nosWebRTCManager::OnServerConnectionSuccesful()
{
    if (ServerConnectionSuccesfulCallback) {

        ServerConnectionSuccesfulCallback();
    }
}

void nosWebRTCManager::OnServerConnectionError()
{
    if (ServerConnectionFailedCallback) {
        Dispose();
        ServerConnectionFailedCallback();
    }
}

void nosWebRTCManager::OnServerConnectionClosed()
{
    if (ServerConnectionFailedCallback) {
        Dispose();
        ServerConnectionFailedCallback();
    }
}

void nosWebRTCManager::OnRawMessageReceived(void* data, size_t length)
{
}

void nosWebRTCManager::OnSDPOfferReceived(std::string&& offer)
{
    json jsonOffer = json::parse(offer);
    std::string peerIDstr = jsonOffer[nosWebRTCJsonConfig::peerIDKey];
    int peerID = std::stoi(peerIDstr);

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> webRTCSessionDescription =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, jsonOffer[nosWebRTCJsonConfig::sdpKey].get<std::string>(), &error);

    if (!webRTCSessionDescription) {
        std::cout << "SDP parsing failed: " << error.description << std::endl;
        return;
    }

    int existingConnectionID = -1;
    if (TryGetConnectionIDFromPeerID(existingConnectionID, peerID)) {
        RemovePeerConnection(existingConnectionID);
    }

    int connectionID = -1;
    if (!AddPeerConnection(connectionID))
        return;

    AttachPeerIDToConnection(connectionID, peerID);

    nosPeerConnectionState state;
    if (!TryGetPeerConnectionState(connectionID, state) || !state.PeerConnection
        || !state.SetSDPObserver || !state.CreateSDPObserver) {
        RemovePeerConnection(connectionID);
        return;
    }

    state.PeerConnection->SetRemoteDescription(
        state.SetSDPObserver.get(), webRTCSessionDescription.release());

    state.PeerConnection->CreateAnswer(
        state.CreateSDPObserver.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
}

void nosWebRTCManager::OnSDPAnswerReceived(std::string&& answer)
{
    //Get the peerID from the answer and use the corresponding PeerConnection for the rest
    json jsonAnswer = json::parse(answer);
    {
        std::string peerIDstr = jsonAnswer[nosWebRTCJsonConfig::peerIDKey];
        int peerID = std::stoi(peerIDstr);
        int internalID = -1;
        if(!TryGetConnectionIDFromPeerID(internalID, peerID)){
            //oops..
            std::cerr << "No corresponding peer connection found for the answer from peer: " << peerID;
            return;
        }


        webrtc::SdpParseError error;

        std::unique_ptr<webrtc::SessionDescriptionInterface> webRTCSessionDescription =
            webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, jsonAnswer[nosWebRTCJsonConfig::sdpKey].get<std::string>(), &error);

        if (!webRTCSessionDescription) {
            RemovePeerConnection(internalID);
            std::cout << "SDP parsing failed: " << error.description << std::endl;
            return;
        }

        nosPeerConnectionState state;
        if (!TryGetPeerConnectionState(internalID, state) || !state.PeerConnection || !state.SetSDPObserver)
            return;

        //Forward answer to webrtc internal stuff
        state.PeerConnection->SetRemoteDescription(
            state.SetSDPObserver.get(), webRTCSessionDescription.release());


    }
}

void nosWebRTCManager::OnICECandidateReceived(std::string&& iceCandidate)
{
    json jsonICE = json::parse(iceCandidate);
    int peerID = std::stoi(jsonICE[nosWebRTCJsonConfig::peerIDKey].get<std::string>());
    int internalID;
    if (!TryGetConnectionIDFromPeerID(internalID, peerID)) {
        nosEngine.LogI("No corresponding peer connection found for the ICE candidate from peer: %d", peerID);
        return;
    }
    nosEngine.LogI("Received ICE candidate for peerID: %d", peerID);

    int sdpMidLineIdx = jsonICE[nosWebRTCJsonConfig::candidateKey][nosWebRTCJsonConfig::sdpMidLineIndexKey].get<int>();
    std::string sdpMid = jsonICE[nosWebRTCJsonConfig::candidateKey][nosWebRTCJsonConfig::sdpMidKey].get<std::string>();
    std::string candidateStr = jsonICE[nosWebRTCJsonConfig::candidateKey][nosWebRTCJsonConfig::candidateKey].get<std::string>();
    webrtc::SdpParseError error;

    std::unique_ptr<webrtc::IceCandidateInterface> candidate(webrtc::CreateIceCandidate(sdpMid, sdpMidLineIdx, candidateStr, &error));
    if (!candidate) {
        nosEngine.LogI("Received candidate message parse failed: %s", error.description.c_str());
        return;
    }
    nosEngine.LogI("Adding ICE candidate to peer connection for peerID: %d", peerID);

    nosPeerConnectionState state;
    if (!TryGetPeerConnectionState(internalID, state) || !state.PeerConnection)
        return;

    state.PeerConnection->AddIceCandidate(candidate.get());
    //check for error when adding?
}

void nosWebRTCManager::OnPeerDisconnectedReceived(std::string&& peerDisconnected)
{
    json jsonMessage = json::parse(peerDisconnected);
    int peerID = std::stoi(jsonMessage[nosWebRTCJsonConfig::peerIDKey].get<std::string>());
    if (!RemovePeerConnectionByPeerID(peerID))
        return;

    if (PeerDisconnectedCallback) {
        PeerDisconnectedCallback();
    }
}
#pragma endregion

#pragma region PeerConnectionObserver Implementation
void nosWebRTCManager::RegisterToPeerConnectionObserverCallbacks(nosPeerConnectionObserver* observer)
{
    observer->SetSignalingChangeCallback(
        [this](auto new_state, int id) {this->OnSignalingChange(new_state, id); });
    observer->SetAddTrackCallback(
        [this](auto receiver, auto& streams, int id) {OnAddTrack(receiver, streams, id); });
    observer->SetRemoveTrackCallback(
        [this](auto receiver, int id) {this->OnRemoveTrack(receiver, id); });
    observer->SetDataChannelCallback(
        [this](auto channel, int id) {this->OnDataChannel(channel, id); });
    observer->SetRenegotiationCallback(
        [this](int id) {this->OnRenegotiationNeeded(id); });
    observer->SetICEConnectionChangeCallback(
        [this](auto new_state, int id) {this->OnIceConnectionChange(new_state, id); });
    observer->SetICEGatheringChangeCallback(
        [this](auto new_state, int id) {this->OnIceGatheringChange(new_state, id); });
    observer->SetICECandidateCallback(
        [this](auto candidate, int id) {this->OnIceCandidate(candidate, id); });

}

void nosWebRTCManager::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state, int id)
{
}

void nosWebRTCManager::OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams, int id)
{
    if (receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        auto track = receiver->track().release();
        auto* videoTrack = static_cast<webrtc::VideoTrackInterface*>(track);
        for (const auto& videoSink : p_VideoSinks) {
            if (videoSink->IsAvailable) {
                videoTrack->AddOrUpdateSink(videoSink, rtc::VideoSinkWants());
                videoSink->IsAvailable = false;
            }
        }
        receiver->SetJitterBufferMinimumDelay(0.05);
    }
}

void nosWebRTCManager::OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, int id)
{
}

void nosWebRTCManager::OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> channel, int id)
{
}

void nosWebRTCManager::OnRenegotiationNeeded(int id)
{
}

void nosWebRTCManager::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state, int id)
{
    nosPeerConnectionState state;
    if (!TryGetPeerConnectionState(id, state))
        return;

    if (new_state == webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected
        && PeerConnectedCallback) {
        PeerConnectedCallback();
    }
    else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionFailed) {
        if (PeerDisconnectedCallback) {
            PeerDisconnectedCallback();
        }
        RemovePeerConnection(id);
    }
    else if (new_state == webrtc::PeerConnectionInterface::kIceConnectionClosed) {
        RemovePeerConnection(id);
    }
}

void nosWebRTCManager::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state, int id)
{
}

void nosWebRTCManager::OnIceCandidate(const webrtc::IceCandidateInterface* candidate, int id)
{
    //This callback indicates that ice candidates for US is ready and we should send it to the server.
    /*
    * Here is the expected structure:
     {
        "type": "iceCandidate",
        "playerId": "101",
        "candidate": {
            "candidate": "candidate:884439561 1 udp 2122260223 172.30.32.1 59614 typ host generation 0 ufrag q2YZ network-id 1",
            "sdpMLineIndex": 0,
            "sdpMid": "0"
         }
      }
    
    */
    
    std::string ICEcandidate;
    candidate->ToString(&ICEcandidate);
    json jsonICE;

    nosPeerConnectionState state;
    if (!TryGetPeerConnectionState(id, state) || state.PeerID < 0)
        return;

    jsonICE[nosWebRTCJsonConfig::peerIDKey] = std::to_string(state.PeerID);
    jsonICE[nosWebRTCJsonConfig::typeKey] = nosWebRTCJsonConfig::typeICE;
    jsonICE[nosWebRTCJsonConfig::candidateKey][nosWebRTCJsonConfig::sdpMidKey] = candidate->sdp_mid();
    jsonICE[nosWebRTCJsonConfig::candidateKey][nosWebRTCJsonConfig::sdpMidLineIndexKey] = candidate->sdp_mline_index();
    jsonICE[nosWebRTCJsonConfig::candidateKey][nosWebRTCJsonConfig::candidateKey] = ICEcandidate;
    p_nosWebRTCClient->SendMessageToServer(jsonICE.dump());
}
#pragma endregion
