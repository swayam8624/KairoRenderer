module;

#include <vulkan/vulkan.h>

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <exception>
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
                VkPhysicalDeviceFeatures supportedFeatures{};
                vkGetPhysicalDeviceFeatures(candidate, &supportedFeatures);
                if (queues && HasExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME) &&
                    supportedFeatures.independentBlend == VK_TRUE)
                {
                    m_PhysicalDevice = candidate;
                    m_GraphicsFamily = queues->Graphics;
                    m_PresentFamily = queues->Present;
                    break;
                }
            }
            if (m_PhysicalDevice == VK_NULL_HANDLE)
            {
                throw std::runtime_error(
                    "No Vulkan device supports graphics, presentation, VK_KHR_swapchain, "
                    "and independent color-attachment blending.");
            }

            const float priority = 1.0f;
            std::set<std::uint32_t> uniqueFamilies{ m_GraphicsFamily, m_PresentFamily };
            std::vector<VkDeviceQueueCreateInfo> queueInfos;
            for (const std::uint32_t family : uniqueFamilies)
            {
                VkDeviceQueueCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
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
            // The viewport writes shaded color and integer object IDs in one
            // subpass. Transparent color must blend while IDs remain exact.
            features.independentBlend = VK_TRUE;
            VkDeviceCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
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
        ~VulkanDevice()
        {
            // Deferred uploads own children of this VkDevice. Before
            // destroying the parent we must establish that queued users of
            // their staging/image objects have retired. VK_ERROR_DEVICE_LOST
            // counts as completion for the Vulkan resource-in-use rule.
            if (!m_DeferredTextureUploads.empty())
            {
                const VkResult idle = vkDeviceWaitIdle(m_Handle);
                if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST)
                {
                    // Never free a possibly still-in-use VkBuffer/VkImage
                    // merely to make shutdown appear successful. There is
                    // no valid Vulkan teardown without completion evidence;
                    // fail explicitly instead of violating native lifetime.
                    std::fputs("Kairo Vulkan: unresolved device retirement at shutdown.\n",
                        stderr);
                    std::terminate();
                }
                for (const auto& upload : m_DeferredTextureUploads)
                    DestroyDeferredTextureUpload(upload);
                m_DeferredTextureUploads.clear();
            }
            if (m_Handle != VK_NULL_HANDLE)
                vkDestroyDevice(m_Handle, nullptr);
        }
        VulkanDevice(const VulkanDevice&) = delete;
        VulkanDevice& operator=(const VulkanDevice&) = delete;
        [[nodiscard]] VkPhysicalDevice PhysicalHandle() const noexcept { return m_PhysicalDevice; }
        [[nodiscard]] VkDevice Handle() const noexcept { return m_Handle; }
        [[nodiscard]] VkQueue GraphicsQueue() const noexcept { return m_GraphicsQueue; }
        [[nodiscard]] VkQueue PresentQueue() const noexcept { return m_PresentQueue; }
        [[nodiscard]] std::uint32_t GraphicsFamily() const noexcept { return m_GraphicsFamily; }
        [[nodiscard]] std::uint32_t PresentFamily() const noexcept { return m_PresentFamily; }

        // No upload or frame submission may be issued after DEVICE_LOST or
        // a native wait failure whose outstanding resource use is unproven.
        // Host access to the Vulkan queue is otherwise externally serialized.
        [[nodiscard]] bool IsUsable() const noexcept
        {
            return !m_Unusable.load(std::memory_order_relaxed);
        }
        void CheckUsable() const
        {
            if (!IsUsable())
                throw std::runtime_error(
                    "Vulkan logical device is unusable after native upload failure.");
        }
        void MarkUnusable() const noexcept
        {
            m_Unusable.store(true, std::memory_order_relaxed);
        }

        struct DeferredTextureUpload final
        {
            VkCommandPool CommandPool = VK_NULL_HANDLE;
            VkBuffer StagingBuffer = VK_NULL_HANDLE;
            VkDeviceMemory StagingMemory = VK_NULL_HANDLE;
            void* StagingMapping = nullptr;
            VkImage Image = VK_NULL_HANDLE;
            VkDeviceMemory ImageMemory = VK_NULL_HANDLE;
            VkImageView ImageView = VK_NULL_HANDLE;
            VkSampler Sampler = VK_NULL_HANDLE;
        };

        // Reserve BEFORE vkQueueSubmit, since an allocation after successful
        // submission could itself throw and lose native resource identity.
        void ReserveDeferredTextureUploadSlot() const
        {
            m_DeferredTextureUploads.reserve(m_DeferredTextureUploads.size() + 1u);
        }

        // Each field is a raw native handle. The caller relinquishes all
        // eight handles and zeroes its own owners BEFORE it throws. Capacity
        // was reserved before submission, so this append cannot allocate.
        void DeferUncertainTextureUpload(DeferredTextureUpload upload) const noexcept
        {
            MarkUnusable();
            if (m_DeferredTextureUploads.size() == m_DeferredTextureUploads.capacity())
                std::terminate(); // programming error: pre-submit reserve omitted
            m_DeferredTextureUploads.push_back(upload);
        }
    private:
        struct QueueFamilies final { std::uint32_t Graphics = 0; std::uint32_t Present = 0; };
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Handle = VK_NULL_HANDLE;
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        VkQueue m_PresentQueue = VK_NULL_HANDLE;
        std::uint32_t m_GraphicsFamily = 0;
        std::uint32_t m_PresentFamily = 0;
        mutable std::atomic<bool> m_Unusable{ false };
        // Unsafe-to-retire uploaded resources are kept alive until a
        // device-idle success or VK_ERROR_DEVICE_LOST is established.
        mutable std::vector<DeferredTextureUpload> m_DeferredTextureUploads;

        void DestroyDeferredTextureUpload(const DeferredTextureUpload& upload) noexcept
        {
            if (upload.CommandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(m_Handle, upload.CommandPool, nullptr);
            if (upload.StagingMapping != nullptr)
                vkUnmapMemory(m_Handle, upload.StagingMemory);
            if (upload.StagingBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(m_Handle, upload.StagingBuffer, nullptr);
            if (upload.StagingMemory != VK_NULL_HANDLE)
                vkFreeMemory(m_Handle, upload.StagingMemory, nullptr);
            if (upload.Sampler != VK_NULL_HANDLE)
                vkDestroySampler(m_Handle, upload.Sampler, nullptr);
            if (upload.ImageView != VK_NULL_HANDLE)
                vkDestroyImageView(m_Handle, upload.ImageView, nullptr);
            if (upload.Image != VK_NULL_HANDLE)
                vkDestroyImage(m_Handle, upload.Image, nullptr);
            if (upload.ImageMemory != VK_NULL_HANDLE)
                vkFreeMemory(m_Handle, upload.ImageMemory, nullptr);
        }

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
