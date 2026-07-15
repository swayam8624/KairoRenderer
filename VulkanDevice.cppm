module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

export module Kairo.Renderer.VulkanDevice;

import Kairo.Renderer.VulkanInstance;
import Kairo.Renderer.VulkanSurface;

export namespace kairo::renderer
{
    inline constexpr const char* VulkanPortabilitySubsetExtension = "VK_KHR_portability_subset";
    /// Input: selected physical device, presentation surface, and queue needs.
    /// Output: owning logical device with graphics and presentation queues.
    /// Task: ensure swapchain construction receives a device that can both
    /// render commands and present to the exact native surface, rather than
    /// assuming the first enumerated adapter is usable.
    class VulkanDevice final
    {
    public:
        VulkanDevice(const VulkanInstance& instance, const VulkanSurface& surface)
        {
            std::uint32_t count = 0;
            if (vkEnumeratePhysicalDevices(instance.Handle(), &count, nullptr) != VK_SUCCESS || count == 0u)
            {
                throw std::runtime_error("No Vulkan physical device is available.");
            }
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(instance.Handle(), &count, devices.data());
            for (const VkPhysicalDevice candidate : devices)
            {
                const auto queues = FindQueues(candidate, surface.Handle());
                if (queues && HasExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                {
                    m_PhysicalDevice = candidate;
                    m_GraphicsFamily = queues->Graphics;
                    m_PresentFamily = queues->Present;
                    break;
                }
            }
            if (m_PhysicalDevice == VK_NULL_HANDLE)
            {
                throw std::runtime_error("No Vulkan device supports graphics, presentation, and VK_KHR_swapchain.");
            }

            const float priority = 1.0f;
            std::set<std::uint32_t> uniqueFamilies{ m_GraphicsFamily, m_PresentFamily };
            std::vector<VkDeviceQueueCreateInfo> queueInfos;
            for (const std::uint32_t family : uniqueFamilies)
            {
                VkDeviceQueueCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
                info.queueFamilyIndex = family;
                info.queueCount = 1;
                info.pQueuePriorities = &priority;
                queueInfos.push_back(info);
            }

            std::vector<const char*> extensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
            if (HasExtension(m_PhysicalDevice, VulkanPortabilitySubsetExtension))
            {
                extensions.push_back(VulkanPortabilitySubsetExtension);
            }
            VkPhysicalDeviceFeatures features{};
            VkDeviceCreateInfo create{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
            create.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
            create.pQueueCreateInfos = queueInfos.data();
            create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            create.pEnabledFeatures = &features;
            if (vkCreateDevice(m_PhysicalDevice, &create, nullptr, &m_Handle) != VK_SUCCESS)
            {
                throw std::runtime_error("vkCreateDevice failed.");
            }
            vkGetDeviceQueue(m_Handle, m_GraphicsFamily, 0, &m_GraphicsQueue);
            vkGetDeviceQueue(m_Handle, m_PresentFamily, 0, &m_PresentQueue);
        }
        ~VulkanDevice() { if (m_Handle != VK_NULL_HANDLE) vkDestroyDevice(m_Handle, nullptr); }
        VulkanDevice(const VulkanDevice&) = delete;
        VulkanDevice& operator=(const VulkanDevice&) = delete;
        [[nodiscard]] VkPhysicalDevice PhysicalHandle() const noexcept { return m_PhysicalDevice; }
        [[nodiscard]] VkDevice Handle() const noexcept { return m_Handle; }
        [[nodiscard]] VkQueue GraphicsQueue() const noexcept { return m_GraphicsQueue; }
        [[nodiscard]] VkQueue PresentQueue() const noexcept { return m_PresentQueue; }
        [[nodiscard]] std::uint32_t GraphicsFamily() const noexcept { return m_GraphicsFamily; }
        [[nodiscard]] std::uint32_t PresentFamily() const noexcept { return m_PresentFamily; }
    private:
        struct QueueFamilies final { std::uint32_t Graphics = 0; std::uint32_t Present = 0; };
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Handle = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        VkQueue m_PresentQueue = VK_NULL_HANDLE;
        std::uint32_t m_GraphicsFamily = 0;
        std::uint32_t m_PresentFamily = 0;
        [[nodiscard]] static bool HasExtension(VkPhysicalDevice device, const char* requested)
        {
            std::uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
            for (const auto& extension : extensions)
            {
                if (std::string(extension.extensionName) == requested) return true;
            }
            return false;
        }
        [[nodiscard]] static std::optional<QueueFamilies> FindQueues(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            std::uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
            std::optional<std::uint32_t> graphics;
            std::optional<std::uint32_t> present;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) graphics = i;
                VkBool32 supported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supported);
                if (supported == VK_TRUE) present = i;
            }
            return graphics && present ? std::optional<QueueFamilies>{ { *graphics, *present } } : std::nullopt;
        }
    };
}
