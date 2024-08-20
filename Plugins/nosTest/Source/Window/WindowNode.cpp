// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#include "WindowNode.h"
#include "GLFW/glfw3.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3native.h"

#include "nosUtil/Stopwatch.hpp"
namespace nos::test
{

void RegisterWindowNode(nosNodeFunctions* out)
{	
	NOS_BIND_NODE_CLASS(NOS_NAME_STATIC("nos.test.Window"), WindowNode, out);
}

WindowNode::WindowNode(const nos::fb::Node* node) : nos::NodeContext(node) 
{ 
}

WindowNode::~WindowNode() 
{ 
	assert(Window == nullptr);
}

bool WindowNode::CreateSwapchain()
{
	nosSwapchainCreateInfo createInfo = {};
	createInfo.SurfaceHandle = Surface;
	createInfo.Extent = { 800, 600 };
	createInfo.PresentMode = NOS_PRESENT_MODE_FIFO;
	nosResult res = nosVulkan->CreateSwapchain(&createInfo, &Swapchain, &FrameCount);
	if (res != NOS_RESULT_SUCCESS)
		return false;
	nosSemaphoreCreateInfo semaphoreCreateInfo = {};
	semaphoreCreateInfo.Type = NOS_SEMAPHORE_TYPE_BINARY;
	Images.resize(FrameCount);
	nosVulkan->GetSwapchainImages(Swapchain, Images.data());
	WaitSemaphore.resize(FrameCount);
	SignalSemaphore.resize(FrameCount);
	for (int i = 0; i < FrameCount; i++)
	{
#ifdef CreateSemaphore
#undef CreateSemaphore
#endif
		nosVulkan->CreateSemaphore(&semaphoreCreateInfo, &WaitSemaphore[i]);
		nosVulkan->CreateSemaphore(&semaphoreCreateInfo, &SignalSemaphore[i]);
	}
	return true;
}

void WindowNode::Clear() 
{
	DestroySwapchain();
	DestroyWindowSurface();
	DestroyWindow();
}

void WindowNode::DestroySwapchain() 
{
	if (!Swapchain)
		return;
	nosCmd cmd;
	nosCmdBeginParams beginParams = {.Name = NOS_NAME("Window node flush cmd"), .AssociatedNodeId = NodeId, .OutCmdHandle = &cmd};
	nosVulkan->Begin2(&beginParams);
	nosGPUEvent wait;
	nosCmdEndParams endParams = {.ForceSubmit = true, .OutGPUEventHandle = &wait};
	nosVulkan->End(cmd, &endParams);
	nosVulkan->WaitGpuEvent(&wait, UINT64_MAX);
	for (int i = 0; i < FrameCount; i++)
	{
		nosVulkan->DestroySemaphore(&WaitSemaphore[i]);
		nosVulkan->DestroySemaphore(&SignalSemaphore[i]);
	}
	WaitSemaphore.clear();
	SignalSemaphore.clear();
	Images.clear();
	nosVulkan->DestroySwapchain(&Swapchain);
}

void WindowNode::DestroyWindowSurface() 
{
	if (!Surface)
		return;
	nosVulkan->DestroyWindowSurface(&Surface);
}

void WindowNode::DestroyWindow() 
{
	if (!Window)
		return;
	glfwDestroyWindow(Window);
	glfwTerminate();
	Window = nullptr;
}

nosResult WindowNode::ExecuteNode(nosNodeExecuteParams* params)
{
	if (!Window)
		return NOS_RESULT_FAILED;
	nosScheduleNodeParams scheduleParams = {};
	scheduleParams.NodeId = NodeId;
	scheduleParams.Reset = false;
	scheduleParams.AddScheduleCount = 1;

	nos::NodeExecuteParams execParams = params;

	auto input = vkss::DeserializeTextureInfo(execParams[NOS_NAME("Input")].Data->Data);
	if (!input.Memory.Handle)
		return NOS_RESULT_FAILED;

	if(!glfwWindowShouldClose(Window))
	{
		glfwPollEvents();

		uint32_t imageIndex;
		nosVulkan->SwapchainAcquireNextImage(Swapchain, -1, &imageIndex, WaitSemaphore[CurrentFrame]);
		nosCmd cmd;
		nosVulkan->Begin("Window", &cmd);
		nosVulkan->Copy(cmd, &input, &Images[imageIndex], 0);

		nosVulkan->ImageStateToPresent(cmd, &Images[imageIndex]);
		nosVulkan->AddWaitSemaphoreToCmd(cmd, WaitSemaphore[CurrentFrame], 1);
		nosVulkan->AddSignalSemaphoreToCmd(cmd, SignalSemaphore[CurrentFrame], 1);

		nosCmdEndParams endParams{.ForceSubmit = true};
		nosVulkan->End(cmd, &endParams);
		nosVulkan->SwapchainPresent(Swapchain, imageIndex, SignalSemaphore[CurrentFrame]);
		nosEngine.ScheduleNode(&scheduleParams);
		CurrentFrame = (CurrentFrame + 1) % FrameCount;
	}
	else
	{
		Clear();
		return NOS_RESULT_FAILED;
	}

	return NOS_RESULT_SUCCESS;
}

void WindowNode::OnExitRunnerThread(std::optional<nosUUID> runnerId) 
{ 
	if (!runnerId)
		return;
	Clear(); 
}

void WindowNode::OnEnterRunnerThread(std::optional<nosUUID> runnerId)
{
	if (!runnerId)
		return;
	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	Window = glfwCreateWindow(800, 600, "NOSWindow", nullptr, nullptr);
	if (nosVulkan->CreateWindowSurface(glfwGetWin32Window(Window), &Surface) != NOS_RESULT_SUCCESS)
	{
		DestroyWindow();
		return;
	}
	if (!CreateSwapchain())
	{
		DestroyWindowSurface();
		DestroyWindow();
		return;
	}

}

void WindowNode::OnPathStop() 
{
	nosCmd cmd;
	nosCmdBeginParams beginParams = { .Name = NOS_NAME("Window node flush cmd"), .AssociatedNodeId = NodeId, .OutCmdHandle = &cmd };
	nosVulkan->Begin2(&beginParams);
	nosGPUEvent wait;
	nosCmdEndParams endParams = { .ForceSubmit = true, .OutGPUEventHandle = &wait };
	nosVulkan->End(cmd, &endParams);
	nosVulkan->WaitGpuEvent(&wait, UINT64_MAX);
}

void WindowNode::OnPathStart()
{
	nosScheduleNodeParams params = {};
	params.NodeId = NodeId;
	params.Reset = false;
	params.AddScheduleCount = 1;

	nosEngine.ScheduleNode(&params);
}

}