// Portable fault-injection regression for Metal/D3D12 create-and-index logic.
// Tests the exact CPU helper used by the native renderer modules, without
// linking Vulkan, Metal, D3D12, GLFW or the engine's C++ module graph.
#include "../detail/NativeResourcePublication.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        using kairo::renderer::detail::CreateAndPublishNativeResource;
        using Handle = std::uint64_t;
        std::unordered_set<Handle> indexed;
        std::vector<Handle> nativeLive;
        std::vector<std::string> operations;

        const auto create = [&](Handle handle)
        {
            nativeLive.push_back(handle); // mock backend owns this allocation
            operations.push_back("create");
            return handle;
        };
        const auto destroy = [&](Handle handle)
        {
            operations.push_back("destroy");
            for (auto it = nativeLive.begin(); it != nativeLive.end(); ++it)
            {
                if (*it == handle) { nativeLive.erase(it); return; }
            }
            throw std::runtime_error("unknown native handle");
        };

        // A successful resource is owned by the native backend and indexed
        // by the higher renderer facade. No rollback is requested.
        const auto good = CreateAndPublishNativeResource<Handle>(
            [&] { return create(11u); },
            [&](Handle handle)
            {
                operations.push_back("publish");
                indexed.insert(handle);
            },
            destroy);
        Check(good == 11u && indexed.contains(11u) &&
            nativeLive == std::vector<Handle>{ 11u } &&
            operations == std::vector<std::string>{ "create", "publish" },
            "Successful native publication did not preserve ownership");

        // std::unordered_map/set may throw while allocating a new node.
        // A successful native create followed by a publication exception
        // must call native rollback before propagating the publication error.
        bool publishThrew = false;
        try
        {
            (void)CreateAndPublishNativeResource<Handle>(
                [&] { return create(22u); },
                [&](Handle) -> void
                {
                    operations.push_back("publish");
                    throw std::length_error("injected map allocation failure");
                },
                destroy);
        }
        catch (const std::length_error&) { publishThrew = true; }
        Check(publishThrew && nativeLive == std::vector<Handle>{ 11u } &&
            operations == std::vector<std::string>{
                "create", "publish", "create", "publish", "destroy" },
            "Failed map publication leaked native GPU ownership");

        // If native creation throws, there is no returned handle to index,
        // and neither publish nor rollback may run.
        bool createThrew = false;
        try
        {
            (void)CreateAndPublishNativeResource<Handle>(
                [&]() -> Handle
                {
                    throw std::runtime_error("injected native create failure");
                },
                [&](Handle) { throw std::runtime_error("unexpected publish"); },
                [&](Handle) { throw std::runtime_error("unexpected cleanup"); });
        }
        catch (const std::runtime_error& error)
        {
            createThrew = std::string(error.what()) == "injected native create failure";
        }
        Check(createThrew && nativeLive == std::vector<Handle>{ 11u },
            "Native constructor failure unexpectedly changed resource state");

        // If both map publication and native rollback fail, expose this
        // unrecoverable state explicitly rather than eating a GPU leak.
        bool doubleFailureReported = false;
        try
        {
            (void)CreateAndPublishNativeResource<Handle>(
                [&] { return create(33u); },
                [&](Handle) { throw std::runtime_error("injected publish failure"); },
                [&](Handle) { throw std::runtime_error("injected destroy failure"); });
        }
        catch (const std::runtime_error& error)
        {
            doubleFailureReported = std::string(error.what()).find(
                "publication and rollback both failed") != std::string::npos;
        }
        Check(doubleFailureReported && nativeLive.size() == 2u &&
              nativeLive.back() == 33u,
            "Double native cleanup failure was silently swallowed");

        // The mock records the intentionally unrecoverable 33 for evidence.
        // Native integration tests must assert real Metal/D3D12 device state.
        std::puts("KairoRenderer native resource publication tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Renderer resource publication regression: %s\n",
            error.what());
        return 1;
    }
}
