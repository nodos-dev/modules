// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include <Nodos/PluginAPI.h>
#include <Builtins_generated.h>
#include <Nodos/PluginHelpers.hpp>
#include <AppService_generated.h>
#include <AppEvents_generated.h>
#include "SignalingServer.h"

NOS_REGISTER_NAME(WebRTCSignalingServer);
NOS_REGISTER_NAME(StreamerPort)
NOS_REGISTER_NAME(PlayerPort)

struct WebRTCSignalingServerNodeContext : nos::NodeContext {
	nosUUID StartServerUUID;
	nosUUID StopServerUUID;

	std::thread ServerThread;
	std::mutex UpdateMutex;
	std::condition_variable ServerUpdateCV;
	std::atomic_bool ShouldUpdateServer = false;
	std::atomic_bool ShouldSuspendUpdate = false;
	std::unique_ptr<nosSignalingServer> p_server;
	int StreamerCount = 0;
	int PlayerCount = 0;
	WebRTCSignalingServerNodeContext(nos::fb::Node const* node) {
		//lws_set_log_level(LLL_ERR | LLL_WARN | LLL_INFO | LLL_DEBUG, nullptr);

		for (auto func : *node->functions()) {
			if (strcmp(func->class_name()->c_str(), "StartServer") == 0) {
				StartServerUUID = *func->id();
			}
			else if (strcmp(func->class_name()->c_str(), "StopServer") == 0) {
				StopServerUUID = *func->id();
			}
		}

		flatbuffers::FlatBufferBuilder fbb;

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &StopServerUUID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));
	}

	~WebRTCSignalingServerNodeContext() {
		if(p_server)
			p_server->StopServer();
		ShouldUpdateServer = false;
		ShouldSuspendUpdate = false;
		if (ServerThread.joinable()) {
			ServerUpdateCV.notify_one();
			ServerThread.join();
		}
	}

	void RegisterCallbacks() {
		if (p_server) {
			p_server->SetStreamerConnectedCallback([this](int id, std::string path) {this->OnStreamerConnected(id, path); });
			p_server->SetStreamerDisconnectedCallback([this](int id, std::string path) {this->OnStreamerDisconnected(id, path); });
			p_server->SetPlayerConnectedCallback([this](int id, std::string path) {this->OnPlayerConnected(id, path); });
			p_server->SetPlayerDisconnectedCallback([this](int id, std::string path) {this->OnPlayerDisconnected(id, path); });
			p_server->SetServerCreatedCallback([this]() {this->OnServerCreated(); });
			p_server->SetServerDestroyedCallback([this]() {this->OnServerDestroyed(); });
		}
	}

	void OnServerCreated() {
		flatbuffers::FlatBufferBuilder fbb;

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &StartServerUUID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &StopServerUUID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, false))));
	}

	void OnServerDestroyed() {
		flatbuffers::FlatBufferBuilder fbb;

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &StartServerUUID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, false))));

		HandleEvent(
			nos::CreateAppEvent(fbb, nos::CreatePartialNodeUpdateDirect(fbb, &StopServerUUID,
				nos::ClearFlags::NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0, nos::fb::CreateOrphanStateDirect(fbb, true))));
	}

	void OnStreamerConnected(int id, std::string path) {
		StreamerCount++;
		nosEngine.LogI("Streamer %d connected to %s!", id, path);
		nosEngine.WatchLog("Connected Streamers: ", std::to_string(StreamerCount).c_str());
	}

	void OnStreamerDisconnected(int id, std::string path) {
		StreamerCount--;
		nosEngine.LogI("Streamer %d disconnected from %s!", id, path);
		nosEngine.WatchLog("Connected Streamers: ", std::to_string(StreamerCount).c_str());
	}

	void OnPlayerConnected(int id, std::string path) {
		PlayerCount++;
		nosEngine.LogI("Player %d connected to %s!", id, path);
		nosEngine.WatchLog("Connected Players: ", std::to_string(PlayerCount).c_str());
	}

	void OnPlayerDisconnected(int id, std::string path) {
		PlayerCount--;
		nosEngine.LogI("Player %d disconnected from %s!", id, path);
		nosEngine.WatchLog("Connected Players: ", std::to_string(PlayerCount).c_str());
	}

	void ServerUpdate() {
		while (ShouldUpdateServer) {
			std::unique_lock<std::mutex> serverLock(UpdateMutex);
			if (ShouldSuspendUpdate) {
				ServerUpdateCV.wait(serverLock);
			}
			if (p_server) {
				p_server->Update();
			}
		}
	}

	static nosResult GetFunctions(size_t* count, nosName* names, nosPfnNodeFunctionExecute* fns) {
		*count = 2;
		if (!names || !fns)
			return NOS_RESULT_SUCCESS;

		names[0] = NOS_NAME_STATIC("StartServer");
		fns[0] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCSignalingServerNodeContext* serverNode = static_cast<WebRTCSignalingServerNodeContext*>(ctx)) {
				
				std::unique_lock<std::mutex> serverLock(serverNode->UpdateMutex);

				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);
				int streamerPort = *nos::GetPinValue<int>(values, NSN_StreamerPort);
				int playerPort = *nos::GetPinValue<int>(values, NSN_PlayerPort);
				if (!serverNode->p_server) {
					serverNode->p_server.reset(new nosSignalingServer());
					serverNode->RegisterCallbacks();
					serverNode->ShouldUpdateServer = true;
					serverNode->ServerThread = std::thread([serverNode]() {serverNode->ServerUpdate(); });
				}
				
				serverNode->p_server->StartServer(streamerPort, playerPort);
				serverNode->ShouldSuspendUpdate = false;
				serverNode->ServerUpdateCV.notify_one();

			}
			return NOS_RESULT_SUCCESS;
		};

		names[1] = NOS_NAME_STATIC("StopServer");
		fns[1] = [](void* ctx, nosFunctionExecuteParams* params) {
			if (WebRTCSignalingServerNodeContext* serverNode = static_cast<WebRTCSignalingServerNodeContext*>(ctx)) {
				serverNode->ShouldSuspendUpdate = true;
				auto values = nos::GetPinValues(params->ParentNodeExecuteParams);
				serverNode->p_server->StopServer();
			}
			return NOS_RESULT_SUCCESS;
		};

		return NOS_RESULT_SUCCESS;
	}
};

void RegisterWebRTCSignalingServer(nosNodeFunctions* outFunctions) {
	NOS_BIND_NODE_CLASS(NSN_WebRTCSignalingServer, WebRTCSignalingServerNodeContext, outFunctions);
}
