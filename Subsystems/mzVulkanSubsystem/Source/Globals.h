﻿#pragma once

// For now, must be included before MediaZ/Modules.h
#include <mzVulkanSubsystem/mzVulkanSubsystem.h>

// MediaZ SDK
#include <MediaZ/Modules.h>

// Vulkan base
#include <mzVulkan/Device.h>

// std
#include <memory>

extern "C" mzEngineServices mzEngine;

namespace mz::vkss
{
template <typename T>
using rc = vk::rc<T>;

// These are initialized at vkss::Initialize, and destroyed at vkss::Deinitialize
extern vk::Device* GVkDevice;
extern vk::rc<vk::Context> GVkCtx;
extern std::unique_ptr<struct ResourceManager> GResources;
}