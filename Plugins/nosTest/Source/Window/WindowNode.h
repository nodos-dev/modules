/*
 * Copyright MediaZ Teknoloji A.S. All Rights Reserved.
 */

#pragma once

#include "Nodos/PluginHelpers.hpp"
#include "nosVulkanSubsystem/Helpers.hpp"

#include "GLFW/glfw3.h"

namespace nos::test
{
void RegisterWindowNode(nosNodeFunctions* out);
class WindowNode : public nos::NodeContext
{
public:
	WindowNode(const nos::fb::Node* node);
	~WindowNode();
	bool CreateSwapchain();
	void Clear();
	void DestroySwapchain();
	void DestroyWindowSurface();
	void DestroyWindow();

	nosResult ExecuteNode(nosNodeExecuteParams* params) override;

	void OnPathStop() override;
	void OnPathStart() override;

	void OnEnterRunnerThread(std::optional<nosUUID> runnerId) override;
	void OnExitRunnerThread(std::optional<nosUUID> runnerId) override;

private:
	void WindowThread();

	GLFWwindow* Window = nullptr;
	std::vector<nosSemaphore> WaitSemaphore{};
	std::vector<nosSemaphore> SignalSemaphore{};
	std::vector<nosResourceShareInfo> Images{};
	uint32_t FrameCount = 0;
	uint32_t CurrentFrame = 0;
	nosSurfaceHandle Surface{};
	nosSwapchainHandle Swapchain{};
};
}