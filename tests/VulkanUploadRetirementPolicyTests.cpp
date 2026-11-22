// GPU-free failure injection of the production synchronous Vulkan texture
// upload completion policy. Models all VkResult distinctions, including the
// Vulkan-spec device-loss lifetime rule; no VkDevice, window or driver needed.
#include "../detail/VulkanUploadRetirementPolicy.hpp"

#include <cstdio>
#include <stdexcept>

namespace
{
    constexpr int Success = 0;
    constexpr int HostOOM = -1;
    constexpr int DeviceOOM = -2;
    constexpr int DeviceLost = -4;
    constexpr int Unknown = -13;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    struct FakeQueue final
    {
        int Submitted = Success;
        int Waited = Success;
        int DeviceIdle = Success;
        int SubmitCalls = 0;
        int WaitCalls = 0;
        int IdleCalls = 0;

        [[nodiscard]] auto Run()
        {
            using kairo::renderer::detail::SubmitAndRetireVulkanUpload;
            return SubmitAndRetireVulkanUpload(
                [this] { ++SubmitCalls; return Submitted; },
                [this] { ++WaitCalls; return Waited; },
                [this] { ++IdleCalls; return DeviceIdle; },
                Success, DeviceLost, HostOOM, DeviceOOM);
        }
    };
}

int main()
{
    try
    {
        using kairo::renderer::detail::VulkanUploadRetirement;
        {
            FakeQueue q;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::Completed &&
                x.Error == Success && q.SubmitCalls == 1 &&
                q.WaitCalls == 1 && q.IdleCalls == 0,
                "Successful upload did not retire via queue wait");
        }
        for (const int error : {HostOOM, DeviceOOM})
        {
            FakeQueue q;
            q.Submitted = error;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::Rejected &&
                x.Error == error && q.WaitCalls == 0 && q.IdleCalls == 0,
                "Rejected OOM submit incorrectly waited or leaked state");
        }
        {
            FakeQueue q;
            q.Submitted = DeviceLost;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::DeviceLost &&
                q.WaitCalls == 0 && q.IdleCalls == 0,
                "Lost device submission was not classified as retired");
        }
        {
            FakeQueue q;
            q.Waited = DeviceLost;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::DeviceLost &&
                q.WaitCalls == 1 && q.IdleCalls == 0,
                "Lost queue wait required an invalid second wait");
        }
        {
            FakeQueue q;
            q.Waited = Unknown;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::RetiredAfterError &&
                x.Error == Unknown && q.WaitCalls == 1 && q.IdleCalls == 1,
                "Successful device-idle fallback failed to retire upload");
        }
        {
            FakeQueue q;
            q.Waited = Unknown;
            q.DeviceIdle = DeviceLost;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::DeviceLost,
                "Device loss during fallback did not allow resource cleanup");
        }
        {
            FakeQueue q;
            q.Waited = HostOOM;
            q.DeviceIdle = DeviceOOM;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::Quarantine &&
                x.Error == HostOOM && q.IdleCalls == 1,
                "Unproven queue completion was incorrectly released");
        }
        {
            FakeQueue q;
            q.Submitted = Unknown;
            q.DeviceIdle = Unknown;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::Quarantine &&
                x.Error == Unknown && q.WaitCalls == 0 && q.IdleCalls == 1,
                "Unknown submit failure without proven idle was released");
        }
        {
            FakeQueue q;
            q.Submitted = Unknown;
            const auto x = q.Run();
            Require(x.Retirement == VulkanUploadRetirement::RetiredAfterError &&
                x.Error == Unknown && q.WaitCalls == 0 && q.IdleCalls == 1,
                "Unknown submit after proven device idle was not retired");
        }
        std::puts("Vulkan texture upload retirement policy tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan upload retirement test failed: %s\n", error.what());
        return 1;
    }
}
