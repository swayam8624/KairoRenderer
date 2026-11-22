// GPU-independent fault-injection test for the exact transaction helper
// invoked by VulkanMaterialDescriptors::Rebuild. It proves the prior resource
// generation survives validation errors, partial allocations and successful
// staged publication without using Vulkan or other graphics libraries.
#include "../detail/TransactionalResourceGeneration.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    void Require(bool ok, const char* message)
    {
        if (!ok) throw std::runtime_error(message);
    }

    struct FakeGeneration final
    {
        int Id = 0;
        int* Destructions = nullptr;
        std::vector<int> Handles;

        FakeGeneration(int id, int& destructCount) noexcept
            : Id(id), Destructions(&destructCount) {}

        ~FakeGeneration() noexcept
        {
            // Represents a VkDescriptorPool plus its owned per-draw UBOs.
            // A partially staged allocation must be released on unwind.
            *Destructions += static_cast<int>(Handles.size());
        }

        FakeGeneration(const FakeGeneration&) = delete;
        FakeGeneration& operator=(const FakeGeneration&) = delete;

        void Swap(FakeGeneration& other) noexcept
        {
            std::swap(Id, other.Id);
            Handles.swap(other.Handles);
        }
    };
}

int main()
{
    try
    {
        using kairo::renderer::detail::BuildAndCommitResourceGeneration;
        int destroyed = 0;
        FakeGeneration active(7, destroyed);
        active.Handles = { 10, 20 }; // live descriptor generation

        bool invalidRejected = false;
        {
            FakeGeneration candidate(8, destroyed);
            try
            {
                BuildAndCommitResourceGeneration(active, candidate,
                    [](FakeGeneration&) {
                        throw std::invalid_argument(
                            "invalid sampled texture view before allocation");
                    });
            }
            catch (const std::invalid_argument&) { invalidRejected = true; }

            Require(invalidRejected && active.Id == 7 &&
                active.Handles == std::vector<int>{ 10, 20 } &&
                candidate.Handles.empty(),
                "Preflight exception changed previously valid generation");
        }
        Require(destroyed == 0,
            "A rejected input erroneously freed old active descriptor resources");

        bool uploadFailed = false;
        {
            FakeGeneration candidate(9, destroyed);
            try
            {
                BuildAndCommitResourceGeneration(active, candidate,
                    [](FakeGeneration& staged) {
                        staged.Handles.push_back(30);
                        staged.Handles.push_back(40);
                        throw std::runtime_error(
                            "injected fourth uniform buffer allocation failure");
                    });
            }
            catch (const std::runtime_error&) { uploadFailed = true; }

            Require(uploadFailed && active.Id == 7 &&
                active.Handles == std::vector<int>{ 10, 20 } &&
                candidate.Handles == std::vector<int>{ 30, 40 },
                "Partial allocation error mutated active descriptor generation");
        }
        Require(destroyed == 2,
            "Failed candidate's own destructor did not release staged buffers");

        {
            FakeGeneration candidate(10, destroyed);
            BuildAndCommitResourceGeneration(active, candidate,
                [](FakeGeneration& staged) {
                    staged.Handles = { 70, 80, 90 };
                });
            Require(active.Id == 10 &&
                active.Handles == std::vector<int>{ 70, 80, 90 } &&
                candidate.Id == 7 &&
                candidate.Handles == std::vector<int>{ 10, 20 },
                "Commit did not swap in complete generation");
            // The old generation retires at the already-fence-safe call site
            // when candidate leaves scope.
        }
        Require(destroyed == 4 &&
            active.Handles == std::vector<int>{ 70, 80, 90 },
            "Committed old generation did not retire after publication");

        {
            FakeGeneration empty(0, destroyed);
            BuildAndCommitResourceGeneration(active, empty,
                [](FakeGeneration&) {});
            Require(active.Handles.empty() && active.Id == 0 &&
                empty.Handles == std::vector<int>{ 70, 80, 90 },
                "Empty draw list failed to publish empty descriptor generation");
        }
        Require(destroyed == 7,
            "Retired replaced generation's last buffers were not freed");

        bool selfSwapRejected = false;
        try
        {
            BuildAndCommitResourceGeneration(active, active,
                [](FakeGeneration&) {});
        }
        catch (const std::invalid_argument&) { selfSwapRejected = true; }
        Require(selfSwapRejected,
            "An active resource owner was accepted as its own candidate");

        std::puts("Transactional Vulkan resource generation tests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan generation transaction regression: %s\n",
            error.what());
        return 1;
    }
}
