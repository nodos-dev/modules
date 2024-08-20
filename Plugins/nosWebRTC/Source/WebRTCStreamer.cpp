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
#include "RGBtoYUV420_Linearized.comp.spv.dat"
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
struct nosWebRTCStreamerInterface {
public:
	rtc::scoped_refptr<nosWebRTCManager> manager;
	nosWebRTCClient client;
	rtc::scoped_refptr<nosCustomVideoSource> nosVideoSource;

	nosWebRTCStreamerInterface() {
		nosVideoSource = rtc::scoped_refptr<nosCustomVideoSource>( new nosCustomVideoSource());
		manager = rtc::scoped_refptr<nosWebRTCManager>(new nosWebRTCManager(&client));
		manager->AddVideoSource(nosVideoSource);
	}
	~nosWebRTCStreamerInterface() {
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

	void SetTargetBitrate(int kbps) {
		if (manager) {
			manager->UpdateBitrates(kbps);
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

//TODO: mvoe this to node context to allow multiple streamers!!!
std::pair<nos::Name, std::vector<uint8_t>> RGBtoYUV420Shader;

struct WebRTCNodeContext : nos::NodeContext {
	
	nosWebRTCStatsLogger encodeLogger;
	nosWebRTCStatsLogger copyToLogger;

	std::unique_ptr<nosWebRTCStreamerInterface> p_nosWebRTC;
	std::unique_ptr<RingProxy> InputRing;
	std::chrono::microseconds interFrameTimeDelta;
	std::chrono::microseconds timeLimit;
	std::chrono::steady_clock::time_point encodeStartTime;

	size_t nextBufferToCopyIndex;
	std::vector<rtc::scoped_refptr<nosI420Buffer>> buffers;
	std::atomic<size_t> FreeBuffers;

	std::mutex EncodeMutex;
	std::condition_variable EncodeCompletedCV;

	std::mutex CopyCompletedMutex;
	std::atomic_bool CopyCompleted = false;
	nosGPUEvent CopyCompletedEvent{};

	std::atomic<EWebRTCPlayerStates> currentState;
	nosUUID InputPinUUID;
	nosUUID NodeID;
	nosUUID ConnectToServerID;
	nosUUID DisconnectFromServerID;

	std::atomic<bool> shouldSendFrame = false;
	std::atomic<bool> shouldSendHunger = true;
	std::atomic<bool> checkCallbacks = true;

	std::mutex WebRTCCallbacksMutex;
	std::condition_variable WebRTCCallbacksCV;

	std::mutex SendFrameMutex;
	std::condition_variable SendFrameCV;

	std::thread FrameSenderThread;
	std::thread CallbackHandlerThread;

	std::mutex RingNewFrameMutex;
	std::condition_variable RingNewFrameCV;

	nosResourceShareInfo InputRGBA8 = {};
	nosResourceShareInfo DummyInput = {}; 
	
	std::vector<std::pair<nosGPUEvent, nosResourceShareInfo>> InputBuffers = {};
	std::vector<nosResourceShareInfo> YUVPlanes = {};
	std::vector<nosResourceShareInfo> YUVBuffers = {};

	float FPS;
	std::atomic_int PeerCount = 0;
	std::string server;
	uint32_t LastFrameID = 0;

	nosUUID OutputPinUUID;

	//On Node Created
	WebRTCNodeContext(nos::fb::Node const* node) :NodeContext(node), currentState(EWebRTCPlayerStates::eNONE), encodeLogger("WebRTC Streamer Encode"), copyToLogger("WebRTC Stramer BeginCopyTo") {
		InputRGBA8.Info.Texture.Format = NOS_FORMAT_B8G8R8A8_SRGB;
		InputRGBA8.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
		InputRGBA8.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
		InputRGBA8.Info.Texture.Width = 1280;
		InputRGBA8.Info.Texture.Height = 720;

		nosVulkan->CreateResource(&InputRGBA8);

		DummyInput.Info.Texture.Format = NOS_FORMAT_B8G8R8A8_SRGB;
		DummyInput.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;

		buffers.push_back(new nosI420Buffer(InputRGBA8.Info.Texture.Width, InputRGBA8.Info.Texture.Height));
		buffers.push_back(new nosI420Buffer(InputRGBA8.Info.Texture.Width, InputRGBA8.Info.Texture.Height));
		buffers.push_back(new nosI420Buffer(InputRGBA8.Info.Texture.Width, InputRGBA8.Info.Texture.Height));
		buffers.push_back(new nosI420Buffer(InputRGBA8.Info.Texture.Width, InputRGBA8.Info.Texture.Height));
		buffers.push_back(new nosI420Buffer(InputRGBA8.Info.Texture.Width, InputRGBA8.Info.Texture.Height));

		for (int i = 0; i < buffers.size(); i++) {
			nosResourceShareInfo PlaneY = {};
			PlaneY.Info.Texture.Format = NOS_FORMAT_R8_SRGB;
			PlaneY.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			PlaneY.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			PlaneY.Info.Texture.Width = InputRGBA8.Info.Texture.Width;
			PlaneY.Info.Texture.Height = InputRGBA8.Info.Texture.Height + InputRGBA8.Info.Texture.Height / 2;
			nosVulkan->CreateResource(&PlaneY);
			
			nosResourceShareInfo BufY  = {};
			BufY.Info.Type = NOS_RESOURCE_TYPE_BUFFER;
			BufY.Info.Buffer.Size = PlaneY.Info.Texture.Width * PlaneY.Info.Texture.Height * sizeof(uint8_t);
			BufY.Info.Buffer.Usage = nosBufferUsage(NOS_BUFFER_USAGE_TRANSFER_SRC | NOS_BUFFER_USAGE_TRANSFER_DST);
			BufY.Info.Buffer.MemoryFlags = nosMemoryFlags(NOS_MEMORY_FLAGS_HOST_VISIBLE);
			nosVulkan->CreateResource(&BufY);

			nosResourceShareInfo Input = {};
			Input.Info.Texture.Format = NOS_FORMAT_B8G8R8A8_SRGB;
			Input.Info.Type = NOS_RESOURCE_TYPE_TEXTURE;
			Input.Info.Texture.Usage = nosImageUsage(NOS_IMAGE_USAGE_TRANSFER_SRC | NOS_IMAGE_USAGE_TRANSFER_DST);
			Input.Info.Texture.Width = InputRGBA8.Info.Texture.Width;
			Input.Info.Texture.Height = InputRGBA8.Info.Texture.Height;
			nosVulkan->CreateResource(&Input);

			YUVBuffers.push_back(std::move(BufY));
			YUVPlanes.push_back(std::move(PlaneY));
			InputBuffers.push_back({0, Input});
		}

		InputRing = std::make_unique<RingProxy>(InputBuffers.size(), "WebRTC Streamer Input Ring");
		InputRing->SetConditionVariable(&RingNewFrameCV);

		FreeBuffers = buffers.size();
		nextBufferToCopyIndex = 0;

		for (auto pin : *node->pins()) {
			if (pin->show_as() == nos::fb::ShowAs::INPUT_PIN) {
				InputPinUUID = *pin->id();
			}
			else if (NSN_MaxFPS.Compare(pin->name()->c_str()) == 0)
			{
				FPS = *(float*)pin->data()->data();
				auto time = std::chrono::duration<float>(1.0f / FPS);
				timeLimit = std::chrono::round<std::chrono::microseconds>(time);
			}
			else if (pin->show_as() == nos::fb::ShowAs::OUTPUT_PIN) {
				OutputPinUUID = *pin->id();
			}
		}
		for (auto func : *node->functions()) {
			if (strcmp(func->class_name()->c_str(), "ConnectToServer") == 0) {
				ConnectToServerID = *func->id();
			}
			else if (strcmp(func->class_name()->c_str(), "DisconnectFromServer") == 0) {
				DisconnectFromServerID = *func->id();
			}
		}
		NodeID = *node->id();
		
		checkCallbacks = true;
		
		nosVec2u deltaSec{10'000u, (uint32_t)std::floor(FPS * 10'000)};

		flatbuffers::FlatBufferBuilder fbb;

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &DisconnectFromServerID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));
	}

	~WebRTCNodeContext() override {
		
		ClearNodeInternals();

		if (CallbackHandlerThread.joinable()) {
			checkCallbacks = false;
			WebRTCCallbacksCV.notify_one();
			CallbackHandlerThread.join();
		}
		for (auto& yuvBuf : YUVBuffers) {
			nosVulkan->DestroyResource(&yuvBuf);
		}
		for (auto& yuvPlane : YUVPlanes) {
			nosVulkan->DestroyResource(&yuvPlane);
		}
	}

	void InitializeNodeInternals() {

		p_nosWebRTC.reset(new nosWebRTCStreamerInterface());
		p_nosWebRTC->manager->SetPeerConnectedCallback([this]() {this->OnPeerConnected(); });
		p_nosWebRTC->manager->SetPeerDisconnectedCallback([this]() {this->OnPeerDisconnected(); });
		p_nosWebRTC->manager->SetServerConnectionSuccesfulCallback([this]() {this->OnConnectedToServer(); });
		p_nosWebRTC->manager->SetServerConnectionFailedCallback([this]() {this->OnDisconnectedFromServer(); });
		p_nosWebRTC->manager->SetImageEncodeCompletedCallback([this]() {this->OnEncodeCompleted(); });

		if (!CallbackHandlerThread.joinable()) {
			CallbackHandlerThread = std::thread([this]() {this->HandleWebRTCCallbacks(); });
		}
	}

	void ClearNodeInternals() {
		if (FrameSenderThread.joinable()) {
			shouldSendFrame = false;
			SendFrameCV.notify_one();
			FrameSenderThread.join();
		}

		p_nosWebRTC.reset();

	}

	void GetScheduleInfo(nosScheduleInfo* out) override
	{
		*out = nosScheduleInfo{
			.Importance = 0,
			.DeltaSeconds = {10'000u, (uint32_t)std::floor(FPS * 10'000)},
			.Type = NOS_SCHEDULE_TYPE_ON_DEMAND,
		};
	}

	void OnPathStart() override {
		nosVec2u deltaSec{ 10'000u, (uint32_t)std::floor(FPS * 10'000) };
		nosScheduleNodeParams scheduleParams
		{
			NodeID, 1, false
		};
		nosEngine.ScheduleNode(&scheduleParams);
	}

	void OnPinValueChanged(nos::Name pinName, nosUUID pinId, nosBuffer value) override
	{
		if (pinName == NSN_In) {
			DummyInput = nos::vkss::DeserializeTextureInfo(value.Data);
		}
		if (pinName == NSN_MaxFPS) {
			FPS = *(static_cast<float*>(value.Data));
			auto time = std::chrono::duration<float>(1.0f / FPS);
			timeLimit = std::chrono::round<std::chrono::microseconds>(time);
		}
		if (pinName == NSN_TargetBitrate && p_nosWebRTC != nullptr) {
			int targetKbps = *(static_cast<int*>(value.Data));
			p_nosWebRTC->SetTargetBitrate(targetKbps);
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
		*count = 2;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;

		names[0] = NOS_NAME_STATIC("ConnectToServer");
		fns[0] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCNodeContext* streamerNode = static_cast<WebRTCNodeContext*>(ctx)) {
				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);

				streamerNode->InitializeNodeInternals();
				streamerNode->server = nos::GetPinValue<const char>(values, NSN_ServerIP);
				streamerNode->p_nosWebRTC->StartConnection(streamerNode->server);
			}
			return NOS_RESULT_SUCCESS;
		};

		names[1] = NOS_NAME_STATIC("DisconnectFromServer");
		fns[1] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCNodeContext* streamerNode = static_cast<WebRTCNodeContext*>(ctx)) {
				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);

				streamerNode->currentState = EWebRTCPlayerStates::eDISCONNECTED_FROM_SERVER;
				streamerNode->WebRTCCallbacksCV.notify_one();
			}
			return NOS_RESULT_SUCCESS;
		};

		return NOS_RESULT_SUCCESS;
	}

	nosResult ExecuteNode(nosNodeExecuteParams* params) 
	{
		copyToLogger.LogStats();
		int writeIndex = 0;
		{
			std::unique_lock lock(SendFrameMutex);
			if (!InputRing->IsWriteable()) {
				nosEngine.LogW("WebRTC Streamer frame drop!");
				return NOS_RESULT_SUCCESS;
			}
			writeIndex = InputRing->GetNextWritable();
		}
		nosCmd cmd;
		nosVulkan->Begin("WebRTC Out Copy", &cmd);
		auto& toCopy = InputBuffers[writeIndex];
		nosVulkan->Copy(cmd, &DummyInput, &toCopy.second, 0);
		assert(toCopy.first == 0);
		nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &toCopy.first };
		nosVulkan->End(cmd, &endParams);
		{
			std::unique_lock lock(SendFrameMutex);
			InputRing->SetWrote();
			SendFrameCV.notify_one();
		}

		return NOS_RESULT_SUCCESS; 
	}

	void OnEncodeCompleted() {
		
		encodeLogger.LogStats();

		FreeBuffers++;
		EncodeCompletedCV.notify_one();
	}

	void SendFrames()
	{
		std::chrono::steady_clock::time_point startTime = std::chrono::high_resolution_clock::now();
		std::chrono::microseconds passedTime;

		nosVec2u deltaSec{10'000u, (uint32_t)std::floor(FPS * 10'000)};
		nosScheduleNodeParams scheduleParams{NodeId, InputBuffers.size() - 1, true};
		nosEngine.ScheduleNode(&scheduleParams);

		while (shouldSendFrame && p_nosWebRTC)
		{
			while (true)
			{
				InputRing->LogRing();
				passedTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - startTime);
				if(passedTime.count() < timeLimit.count())
					continue;
				break;
			}
			startTime = std::chrono::high_resolution_clock::now();
			nosEngine.WatchLog("WebRTC Streamer interframe passed time:", std::to_string(passedTime.count()).c_str());
			if (PeerCount == 0)
				continue;

			auto t_start = std::chrono::high_resolution_clock::now();
			
			{

				std::unique_lock<std::mutex> lck(SendFrameMutex);
				if(!InputRing->IsReadable())
				{
					if (shouldSendFrame) {
						nosEngine.LogW("WebRTC Streamer has no frame on the ring!");
						SendFrameCV.wait(lck);
					}
					continue;
				}
			}

			int readIndex = InputRing->GetNextReadable();

			std::vector<nosShaderBinding> inputs;
			auto& buf = InputBuffers[readIndex];
			inputs.emplace_back(nos::vkss::ShaderBinding(NSN_Input, buf.second));
			inputs.emplace_back(nos::vkss::ShaderBinding(NSN_PlaneY, YUVPlanes[nextBufferToCopyIndex]));
			if (buf.first)
				nosVulkan->WaitGpuEvent(&buf.first, UINT64_MAX);

			nosCmd cmdRunPass; 
			nosVulkan->Begin("WebRTCStreamer.YUVConversion", &cmdRunPass);
			auto t0 = std::chrono::high_resolution_clock::now();

			{
				nosRunComputePassParams pass = {};
				pass.Key = NSN_RGBtoYUV420_Compute_Pass;
				pass.DispatchSize = nosVec2u(InputRGBA8.Info.Texture.Width/20, InputRGBA8.Info.Texture.Height/12);
				pass.Bindings = inputs.data();
				pass.BindingCount = inputs.size();
				pass.Benchmark = 0;
				nosVulkan->RunComputePass(cmdRunPass, &pass);
			}
			auto t1 = std::chrono::high_resolution_clock::now();

			t0 = std::chrono::high_resolution_clock::now();
			{
				nosResourceShareInfo tempBuf = {};

				nosVulkan->Copy(cmdRunPass, &YUVPlanes[nextBufferToCopyIndex], &YUVBuffers[nextBufferToCopyIndex], 0);
				nosGPUEvent tex2bufEvent{};
				nosCmdEndParams endParams{ .ForceSubmit = true, .OutGPUEventHandle = &tex2bufEvent };

				nosVulkan->End(cmdRunPass, &endParams);

				nosVulkan->WaitGpuEvent(&tex2bufEvent, UINT64_MAX);

				//nosVec2u deltaSec{ 10'000u, (uint32_t)std::floor(FPS * 10'000) };
				//nosEngine.SchedulePin(InputPinUUID, deltaSec);
				InputRing->SetRead();
				/*if (StopRequested && InputRing->IsWriteable()) {
					nosVec2u deltaSec{ 10'000u, (uint32_t)std::floor(FPS * 10'000) };
					nosEngine.SchedulePin(InputPinUUID, deltaSec);
				}*/


				auto dataY = nosVulkan->Map(&YUVBuffers[nextBufferToCopyIndex]);

				auto dataU = dataY + InputRGBA8.Info.Texture.Width * InputRGBA8.Info.Texture.Height;

				auto dataV = dataU + InputRGBA8.Info.Texture.Width / 2 * InputRGBA8.Info.Texture.Height / 2;

				if (dataY == nullptr) {
					nosEngine.LogE("YUV420 Frame can not be built!");
					continue;
				}

				auto yuvBuffer = buffers[nextBufferToCopyIndex++];
				//wait for encode
				yuvBuffer->SetDataY(dataY);
				nextBufferToCopyIndex %= buffers.size();
				FreeBuffers--;
				
				t1 = std::chrono::high_resolution_clock::now();
				//nosEngine.WatchLog("WebRTC Streamer-YUV420 Frame Build Time(us)", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()).c_str());


				webrtc::VideoFrame frame =
					webrtc::VideoFrame::Builder()
					.set_video_frame_buffer(yuvBuffer)
					.set_rotation(webrtc::kVideoRotation_0)
					.set_timestamp_us(rtc::TimeMicros())
					.build();

				p_nosWebRTC->nosVideoSource->PushFrame(frame);

				[[unlikely]]
				if (FreeBuffers == 0 && PeerCount > 0)
				{
					nosEngine.LogW("WebRTC Streamer waits for encoding");
					std::unique_lock lock(EncodeMutex);
					EncodeCompletedCV.wait(lock);
				}
			}

			if (InputRing->IsWriteable()) {
				nosVec2u deltaSec{10'000u, (uint32_t)std::floor(FPS * 10'000)};
				nosScheduleNodeParams scheduleParams
				{
					NodeID, 1, false
				};
				nosEngine.ScheduleNode(&scheduleParams);
			}


			auto t_end = std::chrono::high_resolution_clock::now();

			//nosEngine.WatchLog("WebRTC Streamer Run Time(us)", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count()).c_str());
			//nosEngine.WatchLog("WebRTC Streamer FPS", std::to_string(1.0/passedTime.count()*1'000'000.0).c_str());
		}
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
					nosEngine.LogI("WebRTC Streamer connected to server");

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
					if (!FrameSenderThread.joinable()) {
						nosEngine.LogI("WebRTC Streamer starts frame thread");
						shouldSendFrame = true;
						FrameSenderThread = std::thread([this]() {SendFrames(); });
						flatbuffers::FlatBufferBuilder fbb;
						HandleEvent(nos::CreateAppEvent(fbb, nos::app::CreateSetThreadNameDirect(fbb, (u64)FrameSenderThread.native_handle(), "WebRTC Frame Sender")));
					}
					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}
				case EWebRTCPlayerStates::eDISCONNECTED_FROM_SERVER:
				{
					nosEngine.LogI("WebRTC Streamer disconnected from server");

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
					currentState = EWebRTCPlayerStates::eNONE;
					break;
				}

			}
		}
	}
};
nosResult RegisterWebRTCStreamer(nosNodeFunctions* outFunctions)
{
	NOS_BIND_NODE_CLASS(NSN_WebRTCStreamer, WebRTCNodeContext, outFunctions);

	RGBtoYUV420Shader = { NSN_RGBtoYUV420_Compute_Shader, {std::begin(RGBtoYUV420_Linearized_comp_spv), std::end(RGBtoYUV420_Linearized_comp_spv)} };

	nosShaderInfo RGBtoYUV420ShaderInfo = {
		.Key = RGBtoYUV420Shader.first,
		.Source = {.SpirvBlob = {RGBtoYUV420Shader.second.data(), RGBtoYUV420Shader.second.size()}},
	};
	nosResult ret = nosVulkan->RegisterShaders(1, &RGBtoYUV420ShaderInfo);
	if (NOS_RESULT_SUCCESS != ret)
		return ret;

	nosPassInfo pass = {.Key = NSN_RGBtoYUV420_Compute_Pass, .Shader = NSN_RGBtoYUV420_Compute_Shader, .MultiSample = 1};
	return nosVulkan->RegisterPasses(1, &pass);
}
