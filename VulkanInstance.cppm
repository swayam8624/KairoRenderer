module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

export module Kairo.Renderer.VulkanInstance;

import Kairo.Renderer.Types;

export namespace kairo::renderer
{
    [[nodiscard]] inline const char* VulkanResultText(VkResult result) noexcept
    {
        switch (result)
        {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        default: return "Vulkan error";
        }
    }

    class VulkanInstance final
    {
    public:
        VulkanInstance(const VulkanInstanceDesc& desc, const std::vector<const char*>& surfaceExtensions)
        {
            std::vector<const char*> extensions = surfaceExtensions;
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            if (desc.EnableValidation && HasLayer("VK_LAYER_KHRONOS_validation"))
            {
                m_ValidationEnabled = true;
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }

            VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            app.pApplicationName = desc.ApplicationName.c_str();
            app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
            app.pEngineName = "Kairo";
            app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
            app.apiVersion = VK_API_VERSION_1_1;

            VkInstanceCreateInfo create{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            create.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            create.pApplicationInfo = &app;
            create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            const char* validationLayer = "VK_LAYER_KHRONOS_validation";
            if (m_ValidationEnabled)
            {
                create.enabledLayerCount = 1;
                create.ppEnabledLayerNames = &validationLayer;
            }

            const VkResult result = vkCreateInstance(&create, nullptr, &m_Handle);
            if (result != VK_SUCCESS)
            {
                throw std::runtime_error(std::string("vkCreateInstance failed: ") + VulkanResultText(result));
            }
        }
        ~VulkanInstance() { if (m_Handle != VK_NULL_HANDLE) vkDestroyInstance(m_Handle, nullptr); }
        VulkanInstance(const VulkanInstance&) = delete;
        VulkanInstance& operator=(const VulkanInstance&) = delete;
        [[nodiscard]] VkInstance Handle() const noexcept { return m_Handle; }
        [[nodiscard]] bool ValidationEnabled() const noexcept { return m_ValidationEnabled; }
    private:
        VkInstance m_Handle = VK_NULL_HANDLE;
        bool m_ValidationEnabled = false;
        [[nodiscard]] static bool HasLayer(const char* name)
        {
            std::uint32_t count = 0;
            vkEnumerateInstanceLayerProperties(&count, nullptr);
            std::vector<VkLayerProperties> layers(count);
            vkEnumerateInstanceLayerProperties(&count, layers.data());
            return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& layer)
            {
                return std::strcmp(layer.layerName, name) == 0;
            });
        }
    };
}
