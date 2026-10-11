module;

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
            VkInstanceCreateFlags flags = 0u;
            if (HasExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
            {
                extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            }
            // Validation is optional for editor/game startup. Homebrew's Vulkan
            // loader + MoltenVK does not install Khronos validation layers, and
            // partially removed Vulkan SDKs can advertise a stale layer manifest.
            const char* validationOverride = std::getenv("KAIRO_VULKAN_VALIDATION");
            const bool validationDisabled = validationOverride != nullptr &&
                (std::strcmp(validationOverride, "0") == 0 ||
                 std::strcmp(validationOverride, "false") == 0 ||
                 std::strcmp(validationOverride, "off") == 0);
            if (desc.EnableValidation && !validationDisabled &&
                HasLayer("VK_LAYER_KHRONOS_validation") &&
                HasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            {
                m_ValidationEnabled = true;
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }

            VkApplicationInfo app{};
            app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            app.pApplicationName = desc.ApplicationName.c_str();
            app.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
            app.pEngineName = "Kairo";
            app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
            app.apiVersion = VK_API_VERSION_1_1;

            VkInstanceCreateInfo create{};
            create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            create.flags = flags;
            create.pApplicationInfo = &app;
            create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
            create.ppEnabledExtensionNames = extensions.data();
            const char* validationLayer = "VK_LAYER_KHRONOS_validation";
            if (m_ValidationEnabled)
            {
                create.enabledLayerCount = 1;
                create.ppEnabledLayerNames = &validationLayer;
            }

            VkResult result = vkCreateInstance(&create, nullptr, &m_Handle);
            // An advertised validation layer may still fail to load, e.g. its
            // JSON manifest references a deleted dylib after a Homebrew upgrade.
            // Prefer a running Vulkan renderer without validation to exiting.
            if (result == VK_ERROR_LAYER_NOT_PRESENT && m_ValidationEnabled)
            {
                std::cerr << "Kairo Vulkan: optional validation layer is advertised but "
                             "could not be loaded. Retrying without validation.\n";
                m_ValidationEnabled = false;
                create.enabledLayerCount = 0u;
                create.ppEnabledLayerNames = nullptr;
                result = vkCreateInstance(&create, nullptr, &m_Handle);
            }
            if (result != VK_SUCCESS)
            {
                std::string explanation = std::string("vkCreateInstance failed: ") +
                    VulkanResultText(result);
                if (result == VK_ERROR_LAYER_NOT_PRESENT)
                {
                    explanation +=
                        ". Vulkan could not load an instance layer even after "
                        "skipping optional Kairo validation. Check stale layer "
                        "manifests or VK_INSTANCE_LAYERS / VK_LAYER_PATH / "
                        "VK_ADD_LAYER_PATH / VK_LOADER_LAYERS_ENABLE overrides. "
                        "Try: env -u VK_INSTANCE_LAYERS -u VK_LAYER_PATH "
                        "-u VK_ADD_LAYER_PATH -u VK_LOADER_LAYERS_ENABLE "
                        "KAIRO_VULKAN_VALIDATION=0 <KairoEditorApp> "
                        "--project <Project.kproject> --renderer vulkan. "
                        "Set VK_LOADER_DEBUG=layer for loader diagnostics.";
                }
                throw std::runtime_error(explanation);
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
        [[nodiscard]] static bool HasExtension(const char* name)
        {
            std::uint32_t count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
            return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension)
            {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        }
    };
}
