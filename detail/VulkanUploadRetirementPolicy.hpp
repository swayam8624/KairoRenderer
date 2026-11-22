#pragma once

// The queue-submit result and the queue-completion result answer different
// questions. Vulkan's device-loss rule explicitly treats VK_ERROR_DEVICE_LOST
// as completion for "resource in use" and "command buffer pending" decisions.
// Other errors do NOT establish that the GPU has finished using staging bytes.
//
// This header is the exact decision policy used by VulkanTexture2D::Upload.
// It knows no Vulkan headers: pass the VkResult constants from the caller,
// and fault-inject the three native operations in an ordinary C++23 test.

#include <functional>
#include <utility>

namespace kairo::renderer::detail
{
    enum class VulkanUploadRetirement
    {
        Completed,           // submitted + queue wait succeeded
        Rejected,            // out-of-memory submit did not alter resource use
        RetiredAfterError,   // native operation failed, device-idle succeeded
        DeviceLost,          // loss counts as no-longer-in-use by Vulkan spec
        Quarantine           // neither successful nor loss retirement proven
    };

    template<class Result>
    struct VulkanUploadOutcome final
    {
        VulkanUploadRetirement Retirement;
        Result Error; // failing result, or success if Completed
    };

    // Vulkan's vkQueueSubmit guarantee for host/device OOM makes these
    // submission errors a separate case: the failed submit cannot have
    // changed the referenced command buffers/resources.
    //
    // Any unrecognized submission failure may have uncertain queue effects.
    // Try a device-wide idle once. If neither VK_SUCCESS nor DEVICE_LOST is
    // returned, the caller MUST transfer staging, command pool and destination
    // image to the VulkanDevice's deferred retirement journal before throwing.
    template<class Result, class Submit, class WaitQueue, class WaitDevice>
    [[nodiscard]] VulkanUploadOutcome<Result> SubmitAndRetireVulkanUpload(
        Submit&& submit, WaitQueue&& waitQueue, WaitDevice&& waitDevice,
        Result success, Result deviceLost,
        Result outOfHostMemory, Result outOfDeviceMemory)
    {
        const Result submitted = std::invoke(std::forward<Submit>(submit));
        if (submitted == deviceLost)
            return { VulkanUploadRetirement::DeviceLost, submitted };
        if (submitted == outOfHostMemory || submitted == outOfDeviceMemory)
            return { VulkanUploadRetirement::Rejected, submitted };

        if (submitted == success)
        {
            const Result waited = std::invoke(std::forward<WaitQueue>(waitQueue));
            if (waited == success)
                return { VulkanUploadRetirement::Completed, success };
            if (waited == deviceLost)
                return { VulkanUploadRetirement::DeviceLost, waited };
            const Result fallback = std::invoke(std::forward<WaitDevice>(waitDevice));
            if (fallback == success)
                return { VulkanUploadRetirement::RetiredAfterError, waited };
            if (fallback == deviceLost)
                return { VulkanUploadRetirement::DeviceLost, fallback };
            return { VulkanUploadRetirement::Quarantine, waited };
        }

        // Unlike explicitly specified OOM failures, an unknown submit error
        // has no generic promise of untouched resource state. Determine
        // whether cleanup is safe using the device-wide retirement barrier.
        const Result fallback = std::invoke(std::forward<WaitDevice>(waitDevice));
        if (fallback == success)
            return { VulkanUploadRetirement::RetiredAfterError, submitted };
        if (fallback == deviceLost)
            return { VulkanUploadRetirement::DeviceLost, fallback };
        return { VulkanUploadRetirement::Quarantine, submitted };
    }
}
