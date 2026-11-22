#pragma once

// CPU-only exception-safety adapter for native Metal and D3D12 uploads.
// The platform-specific backend creates the real GPU allocation and owns it.
// The C++ renderer facade then publishes the returned handle into a map/set.
// If that map/set allocation fails, C++ destruction of a mere numeric handle
// does NOT release the native MTLBuffer, MTLTexture, or D3D12 resource.
//
// Keeping this template out of the GPU API-specific implementations makes
// allocation-failure behavior testable on every host without a GPU device.
#include <functional>
#include <stdexcept>
#include <utility>

namespace kairo::renderer::detail
{
    // Acquire native handle -> publish in renderer's CPU index -> return handle.
    // Any exception while publishing triggers native rollback exactly once,
    // before rethrowing the ORIGINAL publication failure.
    //
    // A failure in native rollback itself is reported explicitly instead
    // of silently swallowing an unfreed GPU allocation. This helper is only
    // used for fresh, not-yet-submitted resources: the native backend must
    // ensure Rollback(handle) is legal without a queue-retirement barrier.
    template<class Handle, class Create, class Publish, class Rollback>
    [[nodiscard]] Handle CreateAndPublishNativeResource(
        Create&& create, Publish&& publish, Rollback&& rollback)
    {
        const Handle handle = std::invoke(std::forward<Create>(create));
        try
        {
            std::invoke(std::forward<Publish>(publish), handle);
        }
        catch (...)
        {
            try { std::invoke(std::forward<Rollback>(rollback), handle); }
            catch (...)
            {
                throw std::runtime_error(
                    "Native GPU resource publication and rollback both failed.");
            }
            throw;
        }
        return handle;
    }
}
