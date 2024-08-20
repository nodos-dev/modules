// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginAPI.h>
#include <Builtins_generated.h>
#include <Nodos/PluginHelpers.hpp>
#include <AppService_generated.h>
#include <AppEvents_generated.h>
#include <nosUtil/Thread.h>
#include <nosVulkanSubsystem/nosVulkanSubsystem.h>
#include <nosVulkanSubsystem/Helpers.hpp>
#include "Names.h"

#include <Windows.h>
#include <shellapi.h>  // must come after windows.h

#include <string>
#include <vector>

#include "rtc_base/checks.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/string_utils.h"  // For ToUtf8
#include "rtc_base/win32_socket_init.h"
#include "system_wrappers/include/field_trial.h"
#include "test/field_trial.h"
#include "CustomVideoSource.h"
#include <memory>

#include <string>
#include <vector>

#include "rtc_base/checks.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/string_utils.h"  // For ToUtf8
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/win32_socket_server.h"
#include "system_wrappers/include/field_trial.h"
#include "test/field_trial.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "WebRTCManager.h"
#include "WebRTCClient.h"
#include "YUV420toRGB.comp.spv.dat"
#include "LinearI420Buffer.h"
#include "I420Buffer.h"
#include "WebRTCCommon.h"

// nosNodes

enum EWebRTCPlayerStates {
	eNONE,
	eREQUESTED_TO_CONNECT_SERVER,
	eCONNECTED_TO_SERVER,
	eCONNECTED_TO_PEER,
	eDISCONNECTED_FROM_SERVER,
	eDISCONNECTED_FROM_PEER,
};

//The interface between medaiZ and WebRTC, stores the task qeueue and launches the connection thread
struct nosWebRTCPlayerInterface {
public:
	std::chrono::steady_clock::time_point frameReceived;
	rtc::scoped_refptr<nosWebRTCManager> manager;
	nosWebRTCClient client;
	rtc::scoped_refptr<nosCustomVideoSink> nosVideoSink;
	nosWebRTCPlayerInterface() {
		nosVideoSink = rtc::scoped_refptr<nosCustomVideoSink>( new nosCustomVideoSink());
		manager = rtc::scoped_refptr<nosWebRTCManager>(new nosWebRTCManager(&client));
		manager->AddVideoSink(nosVideoSink);
	}
	~nosWebRTCPlayerInterface() {
		manager->Dispose();
		Dispose();
	}
	
	void StartConnection(std::string server_port) {

		if(!RTCThread.joinable())
			RTCThread = std::thread([this]() {this->StartRTCThread(); });
		try {
			client.ConnectToServer(server_port);
		}
		catch (std::exception& E) {
			nosEngine.LogE(E.what());
		}
	}

	void SendOffer(int streamerID) {
		if (manager) {
			manager->SendOffer(streamerID);
		}
	}

	void Dispose() {
		isAlive = false;
		if (RTCThread.joinable())
			RTCThread.join();
	}
	

private:
	std::atomic<bool> isAlive = true;
	std::thread RTCThread;
	void StartRTCThread() {
		rtc::WinsockInitializer winsock_init;
		rtc::Win32SocketServer w32_ss;
		rtc::Win32Thread w32_thread(&w32_ss);
		rtc::ThreadManager::Instance()->SetCurrentThread(&w32_thread);

		while (isAlive) {
			client.Update();
			manager->MainLoop();
			w32_thread.ProcessMessages(1);
		}
		
		w32_thread.Quit();
		rtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
		
		//OpenSSL v1.1.1x should clean itself from memory but
		//just for safety we will call this
	}
};

std::pair<nos::Name, std::vector<uint8_t>> YUV420toRGBShader;

struct WebRTCPlayerNodeContext : nos::NodeContext {
	nosWebRTCStatsLogger PlayerBeginCopyToLogger;
	nosWebRTCStatsLogger PlayerOnFrameLogger;

	std::unique_ptr<nosWebRTCPlayerInterface> p_nosWebRTC;
	std::unique_ptr<RingProxy> RGBAConversionRing;
	std::unique_ptr<RingProxy> OutputRing;

	std::atomic<EWebRTCPlayerStates> currentState;
	nosUUID NodeID;
	nosUUID OutputPinUUID;
	nosUUID ConnectToServerID;
	nosUUID ConnectToPeerID;
	nosUUID DisconnectFromServerID;

	std::atomic<bool> shouldConvertFrame = false;
	std::atomic<bool> checkCallbacks = true;

	std::mutex WebRTCCallbacksMutex;
	std::condition_variable WebRTCCallbacksCV;

	std::mutex FrameConversionMutex;
	std::condition_variable FrameConversionCV;

	std::thread FrameConverterThread;
	std::thread CallbackHandlerThread;

	nosResourceShareInfo OutputRGBA8 = {};

	std::vector<nosResourceShareInfo> ConvertedTextures = {};
	std::vector<nosResourceShareInfo> OnFrameYBuffers;
	std::vector<nosResourceShareInfo> OnFrameUBuffers;
	std::vector<nosResourceShareInfo> OnFrameVBuffers;

	std::atomic_int PeerCount = 0;
	std::string server;
	std::mutex IsFrameCopyable;
	std::chrono::high_resolution_clock::time_point LastCopyTime;
	int FPS;
	std::chrono::microseconds timeLimit;


	//On Node Created
	WebRTCPlayerNodeContext(nos::fb::Node const* node) :NodeContext
	(node), currentState(EWebRTCPlayerStates::eNONE), PlayerBeginCopyToLogger("WebRTC Player BeginCopyFrom"), PlayerOnFrameLogger("WebRTC Player OnVideoFrame") {
		OutputRGBA8.Info.Texture.Format = NOS_FORMAT_R8G8B8A8_SRGB;
		OutputRGBA8.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
		OutputRGBA8.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
		OutputRGBA8.Info.Texture.Width = 1280;
		OutputRGBA8.Info.Texture.Height = 720;

		nosVulkan->CreateResource(&OutputRGBA8);

		for (int i = 0; i < 5; i++) {
			nosResourceShareInfo tex = {};
			tex.Info.Texture.Format = NOS_FORMAT_R8G8B8A8_SRGB;
			tex.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			tex.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			tex.Info.Texture.Width = OutputRGBA8.Info.Texture.Width;
			tex.Info.Texture.Height = OutputRGBA8.Info.Texture.Height;
			nosVulkan->CreateResource(&tex);
			ConvertedTextures.push_back(std::move(tex));

			nosResourceShareInfo tmpY = {};
			nosResourceShareInfo tmpU = {};
			nosResourceShareInfo tmpV = {};
			OnFrameYBuffers.push_back(std::move(tmpY));
			OnFrameUBuffers.push_back(std::move(tmpU));
			OnFrameVBuffers.push_back(std::move(tmpV));
		}

		RGBAConversionRing = std::make_unique<RingProxy>(OnFrameYBuffers.size(),"WebRTCPlayer RGBA Conversion Ring");
		
		OutputRing = std::make_unique<RingProxy>(ConvertedTextures.size(), "WebRTCPlayer Output Ring");

		for (auto pin : *node->pins()) {
			if (pin->show_as() == nos::fb::ShowAs::OUTPUT_PIN) {
				OutputPinUUID = *pin->id();
			}
			if (NSN_MaxFPS.Compare(pin->name()->c_str()) == 0)
			{
				FPS = *(float*)pin->data()->data();
				auto time = std::chrono::duration<float>(1.0f / FPS);
				timeLimit = std::chrono::round<std::chrono::microseconds>(time);
			}
		}
		for (auto func : *node->functions()) {
			if (strcmp(func->class_name()->c_str(), "ConnectToServer") == 0) {
				ConnectToServerID = *func->id();
			}
			else if (strcmp(func->class_name()->c_str(), "ConnectToPeer") == 0) {
				ConnectToPeerID = *func->id();
			}
			else if (strcmp(func->class_name()->c_str(), "DisconnectFromServer") == 0) {
				DisconnectFromServerID = *func->id();
			}
		}
		NodeID = *node->id();
		
		checkCallbacks = true;

		flatbuffers::FlatBufferBuilder fbb;

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &DisconnectFromServerID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));
	}

	~WebRTCPlayerNodeContext() override {
		
		ClearNodeInternals();

		if (CallbackHandlerThread.joinable()) {
			checkCallbacks = false;
			WebRTCCallbacksCV.notify_one();
			CallbackHandlerThread.join();
		}
		for (const auto& text : ConvertedTextures) {
			nosVulkan->DestroyResource(&text);
		}
	}

	void  OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override {
		if (pinName == NSN_MaxFPS) {
			FPS = *(static_cast<float*>(value.Data));
			auto time = std::chrono::duration<float>(1.0f / FPS);
			timeLimit = std::chrono::round<std::chrono::microseconds>(time);
		}
	}
	
	void InitializeNodeInternals() {

		p_nosWebRTC.reset(new nosWebRTCPlayerInterface());
		p_nosWebRTC->manager->SetPeerConnectedCallback([this]() {this->OnPeerConnected(); });
		p_nosWebRTC->manager->SetPeerDisconnectedCallback([this]() {this->OnPeerDisconnected(); });
		p_nosWebRTC->manager->SetServerConnectionSuccesfulCallback([this]() {this->OnConnectedToServer(); });
		p_nosWebRTC->manager->SetServerConnectionFailedCallback([this]() {this->OnDisconnectedFromServer(); });
		p_nosWebRTC->nosVideoSink->SetOnFrameCallback([this](const webrtc::VideoFrame& frame) {this->OnVideoFrame(frame); });

		if (!CallbackHandlerThread.joinable()) {
			CallbackHandlerThread = std::thread([this]() {this->HandleWebRTCCallbacks(); });
		}
	}

	void ClearNodeInternals() {
		if (FrameConverterThread.joinable()) {
			shouldConvertFrame = false;
			FrameConverterThread.join();
		}

		p_nosWebRTC.reset();

	}

	void OnVideoFrame(const webrtc::VideoFrame& frame) {
		
		PlayerOnFrameLogger.LogStats();
		auto buffer = frame.video_frame_buffer()->GetI420();
		if (!RGBAConversionRing->IsWriteable()) {
			nosEngine.LogW("WebRTC Player dropped a frame");
			return;
		}

		int writeIndex = RGBAConversionRing->GetNextWritable();

		if (OnFrameYBuffers[writeIndex].Info.Texture.Width != buffer->width() || OnFrameYBuffers[writeIndex].Info.Texture.Height != buffer->height()) {
			if(OnFrameYBuffers[writeIndex].Memory.Handle != NULL) {
				nosVulkan->DestroyResource(&OnFrameYBuffers[writeIndex]);
				nosVulkan->DestroyResource(&OnFrameUBuffers[writeIndex]);
				nosVulkan->DestroyResource(&OnFrameVBuffers[writeIndex]);
			}
			OnFrameYBuffers[writeIndex].Info.Texture.Format = NOS_FORMAT_R8_SRGB;
			OnFrameYBuffers[writeIndex].Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			OnFrameYBuffers[writeIndex].Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			OnFrameYBuffers[writeIndex].Info.Texture.Width = buffer->width();
			OnFrameYBuffers[writeIndex].Info.Texture.Height = buffer->height();
			nosVulkan->CreateResource(&OnFrameYBuffers[writeIndex]);

			OnFrameUBuffers[writeIndex].Info.Texture.Format = NOS_FORMAT_R8_SRGB;
			OnFrameUBuffers[writeIndex].Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			OnFrameUBuffers[writeIndex].Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			OnFrameUBuffers[writeIndex].Info.Texture.Width = buffer->width() / 2;
			OnFrameUBuffers[writeIndex].Info.Texture.Height = buffer->height() / 2;
			nosVulkan->CreateResource(&OnFrameUBuffers[writeIndex]);

			OnFrameVBuffers[writeIndex].Info.Texture.Format = NOS_FORMAT_R8_SRGB;
			OnFrameVBuffers[writeIndex].Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			OnFrameVBuffers[writeIndex].Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			OnFrameVBuffers[writeIndex].Info.Texture.Width = buffer->width() / 2;
			OnFrameVBuffers[writeIndex].Info.Texture.Height = buffer->height() / 2;
			nosVulkan->CreateResource(&OnFrameVBuffers[writeIndex]);
		}
		nosCmd cmd;
		nosVulkan->Begin("WebRTCPlayer Upload", &cmd);
		nosVulkan->ImageLoad(cmd,
							 buffer->DataY(),
							 nosVec2u(buffer->width(), buffer->height()),
							 NOS_FORMAT_R8_SRGB,
							 &OnFrameYBuffers[writeIndex]);
		nosVulkan->ImageLoad(cmd,
							 buffer->DataU(),
							 nosVec2u(buffer->width() / 2, buffer->height() / 2),
							 NOS_FORMAT_R8_SRGB,
							 &OnFrameUBuffers[writeIndex]);
		nosVulkan->ImageLoad(cmd,
							 buffer->DataV(),
							 nosVec2u(buffer->width() / 2, buffer->height() / 2),
							 NOS_FORMAT_R8_SRGB,
							 &OnFrameVBuffers[writeIndex]);
		RGBAConversionRing->SetWrote();
		FrameConversionCV.notify_one();
	}

	

	nosResult CopyFrom(nosCopyInfo* copyInfo) override{
		
		if (!OutputRing->IsReadable()) {
			return NOS_RESULT_FAILED;
		}
		auto now = std::chrono::high_resolution_clock::now();
		if (timeLimit.count() > std::chrono::duration_cast<std::chrono::microseconds>(now - LastCopyTime).count()) {
			//nosEngine.LogW("Too fast");
			return NOS_RESULT_FAILED;
		}
		LastCopyTime = std::chrono::high_resolution_clock::now();

		PlayerBeginCopyToLogger.LogStats();
		int readIndex = OutputRing->GetNextReadable();
		auto dst = nos::vkss::DeserializeTextureInfo(copyInfo->PinData->Data);
		nosCmd cmd;
		nosVulkan->Begin("WebRTC Copy From", &cmd);
		nosVulkan->Copy(cmd, &ConvertedTextures[readIndex], &dst, 0);
		nosGPUEvent event;
		nosCmdEndParams endParams{.ForceSubmit = true, .OutGPUEventHandle = &event};
		nosVulkan->End(cmd, &endParams);
		nosVulkan->WaitGpuEvent(&event, UINT64_MAX);

		OutputRing->SetRead();
		FrameConversionCV.notify_one();
		return NOS_RESULT_SUCCESS;
	}

	void ConvertFrames() {
		while (shouldConvertFrame) {
			RGBAConversionRing->LogRing();
			OutputRing->LogRing();
			if (!RGBAConversionRing->IsReadable() || !OutputRing->IsWriteable()) {
				//std::unique_lock<std::mutex> lock(FrameConversionMutex);
				//FrameConversionCV.wait(lock);
				continue;
			}

			int readIndex = RGBAConversionRing->GetNextReadable();
			auto t_start = std::chrono::high_resolution_clock::now();
			int writeIndex = OutputRing->GetNextWritable();
			std::vector<nosShaderBinding> inputs;
			inputs.emplace_back(nos::vkss::ShaderBinding(NSN_Output, ConvertedTextures[writeIndex]));
			inputs.emplace_back(nos::vkss::ShaderBinding(NSN_PlaneY, OnFrameYBuffers[readIndex]));
			inputs.emplace_back(nos::vkss::ShaderBinding(NSN_PlaneU, OnFrameUBuffers[readIndex]));
			inputs.emplace_back(nos::vkss::ShaderBinding(NSN_PlaneV, OnFrameVBuffers[readIndex]));


			nosCmd cmdRunPass;
			nosVulkan->Begin("WebRTCPlayer.YUVConversion", &cmdRunPass);
			auto t0 = std::chrono::high_resolution_clock::now();
			{
				nosRunComputePassParams pass = {};
				pass.Key = NSN_YUV420toRGB_Compute_Pass;
				pass.DispatchSize = nosVec2u(OutputRGBA8.Info.Texture.Width / 20, OutputRGBA8.Info.Texture.Height / 12);
				pass.Bindings = inputs.data();
				pass.BindingCount = inputs.size();
				pass.Benchmark = 0;
				nosVulkan->RunComputePass(cmdRunPass, &pass);
			}
			nosVulkan->End(cmdRunPass, NOS_FALSE);
			OutputRing->SetWrote();

			//auto t1 = std::chrono::high_resolution_clock::now();
			//nosEngine.WatchLog("WebRTC Player-Compute Pass Time(us)", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()).c_str());
			//auto t_end = std::chrono::high_resolution_clock::now();
			//
			//nosEngine.WatchLog("WebRTC Player Run Time(us)", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count()).c_str());
			RGBAConversionRing->SetRead();
		}
	}

	void OnPinConnected(nos::Name pinName, nosUUID connectedPin) override
	{

	}

	void OnConnectedToServer() {
		currentState = EWebRTCPlayerStates::eCONNECTED_TO_SERVER;
		WebRTCCallbacksCV.notify_one();
	}

	void OnDisconnectedFromServer() {
		currentState = EWebRTCPlayerStates::eDISCONNECTED_FROM_SERVER;
		WebRTCCallbacksCV.notify_one();
		//p_nosWebRTC.reset(new nosWebRTCInterface());
	}

	void OnPeerConnected() {
		++PeerCount;
		currentState = EWebRTCPlayerStates::eCONNECTED_TO_PEER;
		WebRTCCallbacksCV.notify_one();
	}

	void OnPeerDisconnected() {
		--PeerCount;
		currentState = EWebRTCPlayerStates::eDISCONNECTED_FROM_PEER;
		WebRTCCallbacksCV.notify_one();
	}

	static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns) {
		*count = 3;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;

		names[0] = NOS_NAME_STATIC("ConnectToServer");
		fns[0] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCPlayerNodeContext* playerNode = static_cast<WebRTCPlayerNodeContext*>(ctx)) {
				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);

				playerNode->InitializeNodeInternals();
				playerNode->server = nos::GetPinValue<const char>(values, NSN_ServerIP);
				playerNode->p_nosWebRTC->StartConnection(playerNode->server);
			}
			return NOS_RESULT_SUCCESS;
			};

		names[1] = NOS_NAME_STATIC("DisconnectFromServer");
		fns[1] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCPlayerNodeContext* playerNode = static_cast<WebRTCPlayerNodeContext*>(ctx)) {
				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);

				playerNode->currentState = EWebRTCPlayerStates::eDISCONNECTED_FROM_SERVER;
				playerNode->WebRTCCallbacksCV.notify_one();
			}
			return NOS_RESULT_SUCCESS;
		};

		names[2] = NOS_NAME_STATIC("ConnectToPeer");
		fns[2] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCPlayerNodeContext* playerNode = static_cast<WebRTCPlayerNodeContext*>(ctx)) {
				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);

				playerNode->p_nosWebRTC->SendOffer(*nos::GetPinValue<int>(values, NSN_StreamerID));
			}
			return NOS_RESULT_SUCCESS;
		};

		return NOS_RESULT_SUCCESS;
	}

	void HandleWebRTCCallbacks() {
		while (checkCallbacks) {
			
			std::unique_lock<std::mutex> lck(WebRTCCallbacksMutex);
			WebRTCCallbacksCV.wait(lck);
			
			switch (currentState) {
				case EWebRTCPlayerStates::eNONE: 
				{
					//Idle
					break;
				}
				case EWebRTCPlayerStates::eREQUESTED_TO_CONNECT_SERVER:
				{
					p_nosWebRTC->StartConnection(server);
					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}
				case EWebRTCPlayerStates::eCONNECTED_TO_SERVER:
				{
					nosEngine.LogI("WebRTC Player connected to server");

					flatbuffers::FlatBufferBuilder fbb;
					HandleEvent(
						nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &ConnectToServerID, 
							nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));

					HandleEvent(
						nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &DisconnectFromServerID,
							nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, false))));

					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}
				case EWebRTCPlayerStates::eCONNECTED_TO_PEER: 
				{
					if (!FrameConverterThread.joinable()) {
						shouldConvertFrame = true;
						FrameConverterThread = std::thread([this]() {ConvertFrames(); });
					}
					flatbuffers::FlatBufferBuilder fbb;
					HandleEvent(
						nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &ConnectToPeerID,
							nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));
					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}
				case EWebRTCPlayerStates::eDISCONNECTED_FROM_SERVER:
				{
					nosEngine.LogI("WebRTC Player disconnected from server");

					flatbuffers::FlatBufferBuilder fbb;
					HandleEvent(
						nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &ConnectToServerID, nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, false))));
					
					HandleEvent(
						nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &DisconnectFromServerID,
							nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));
					
					ClearNodeInternals();
					
					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}
				case EWebRTCPlayerStates::eDISCONNECTED_FROM_PEER:
				{
					//shouldSendFrame = false;
					flatbuffers::FlatBufferBuilder fbb;
					HandleEvent(
						nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &ConnectToPeerID,
							nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, false))));

					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}

			}
		}
	}
};

nosResult RegisterWebRTCPlayer(nosNodeFunctions* outFunctions) {
	NOS_BIND_NODE_CLASS(NSN_WebRTCPlayer, WebRTCPlayerNodeContext, outFunctions);

	YUV420toRGBShader = { NSN_YUV420toRGB_Compute_Shader, {std::begin(YUV420toRGB_comp_spv), std::end(YUV420toRGB_comp_spv)} };

	nosShaderInfo YUV420toRGBShaderInfo = {
		.Key = YUV420toRGBShader.first,
		.Source = {.SpirvBlob = {YUV420toRGBShader.second.data(), YUV420toRGBShader.second.size()}},
	};
	nosResult res = nosVulkan->RegisterShaders(1, &YUV420toRGBShaderInfo);
	if (res != NOS_RESULT_SUCCESS)
		return res;
	nosPassInfo YUV420toRGBPassInfo = {
		.Key = NSN_YUV420toRGB_Compute_Pass, .Shader = NSN_YUV420toRGB_Compute_Shader, .MultiSample = 1};
	return nosVulkan->RegisterPasses(1, &YUV420toRGBPassInfo);
}
