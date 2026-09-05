module;

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module Kairo.Renderer.RenderGraph;

export namespace kairo::renderer
{
    enum class RenderResourceKind { Texture, Buffer, External };
    enum class RenderAccessMode { Read, Write, ReadWrite };
    enum class RenderResourceState
    {
        Undefined,
        ColorAttachment,
        DepthAttachment,
        ShaderRead,
        CopySource,
        CopyDestination,
        Present
    };

    struct RenderResourceHandle final
    {
        std::uint32_t Value = std::numeric_limits<std::uint32_t>::max();
        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Value != std::numeric_limits<std::uint32_t>::max();
        }
        auto operator<=>(const RenderResourceHandle&) const = default;
    };

    struct RenderPassHandle final
    {
        std::uint32_t Value = std::numeric_limits<std::uint32_t>::max();
        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Value != std::numeric_limits<std::uint32_t>::max();
        }
        auto operator<=>(const RenderPassHandle&) const = default;
    };

    struct RenderResourceDesc final
    {
        std::string Name;
        RenderResourceKind Kind = RenderResourceKind::Texture;
        std::uint64_t EstimatedBytes = 0u;
        bool Transient = true;
        RenderResourceState InitialState = RenderResourceState::Undefined;
    };

    struct RenderResourceAccess final
    {
        RenderResourceHandle Resource;
        RenderAccessMode Mode = RenderAccessMode::Read;
        RenderResourceState State = RenderResourceState::ShaderRead;
    };

    struct RenderResourceTransition final
    {
        RenderResourceHandle Resource;
        RenderResourceState Before = RenderResourceState::Undefined;
        RenderResourceState After = RenderResourceState::Undefined;
    };

    struct RenderResourceLifetime final
    {
        RenderResourceHandle Resource;
        std::uint32_t FirstPass = 0u;
        std::uint32_t LastPass = 0u;
        std::uint32_t AliasSlot = std::numeric_limits<std::uint32_t>::max();
    };

    /// Physical reservation required for one class-compatible transient alias
    /// slot. Backends may bind every non-overlapping resource lifetime assigned
    /// to this slot to the same native allocation when their API permits it.
    struct RenderAliasSlotDesc final
    {
        std::uint32_t Slot = std::numeric_limits<std::uint32_t>::max();
        RenderResourceKind Kind = RenderResourceKind::Texture;
        std::uint64_t CapacityBytes = 0u;
    };

    struct RenderPassProfile final
    {
        std::string Name;
        double Milliseconds = 0.0;
    };

    struct RenderGraphExecutionProfile final
    {
        std::vector<RenderPassProfile> Passes;
        double TotalMilliseconds = 0.0;
    };

    /// Optional backend hook executed immediately before a compiled pass callback.
    /// It receives the exact deterministic state transitions produced by the
    /// compiler. Vulkan/D3D12/Metal translators can therefore record native
    /// barriers without re-deriving hazards or mutating the compiled graph.
    struct RenderGraphExecutionHooks final
    {
        std::function<void(std::size_t, std::string_view,
            const std::vector<RenderResourceTransition>&)> ApplyTransitions;
    };

    class CompiledRenderGraph final
    {
        struct Pass final
        {
            std::string Name;
            std::vector<RenderResourceAccess> Accesses;
            std::vector<RenderResourceTransition> Transitions;
            std::function<void()> Execute;
        };

        std::vector<RenderResourceDesc> m_Resources;
        std::vector<Pass> m_Passes;
        std::vector<RenderResourceLifetime> m_Lifetimes;
        std::vector<RenderAliasSlotDesc> m_AliasSlots;
        std::uint64_t m_TransientAllocationBytes = 0u;

        friend class RenderGraph;

        void ValidateResource(RenderResourceHandle handle) const
        {
            if (!handle.IsValid() || handle.Value >= m_Resources.size())
                throw std::out_of_range(
                    "Compiled render graph resource handle is invalid.");
        }

    public:
        [[nodiscard]] std::size_t ResourceCount() const noexcept
        {
            return m_Resources.size();
        }

        [[nodiscard]] const RenderResourceDesc& Resource(
            RenderResourceHandle handle) const
        {
            ValidateResource(handle);
            return m_Resources[handle.Value];
        }

        [[nodiscard]] std::size_t PassCount() const noexcept
        {
            return m_Passes.size();
        }

        [[nodiscard]] std::string_view PassName(std::size_t index) const
        {
            if (index >= m_Passes.size())
                throw std::out_of_range("Render graph pass index is out of range.");
            return m_Passes[index].Name;
        }

        [[nodiscard]] const std::vector<RenderResourceAccess>& Accesses(
            std::size_t passIndex) const
        {
            if (passIndex >= m_Passes.size())
                throw std::out_of_range("Render graph pass index is out of range.");
            return m_Passes[passIndex].Accesses;
        }

        [[nodiscard]] const std::vector<RenderResourceTransition>& Transitions(
            std::size_t passIndex) const
        {
            if (passIndex >= m_Passes.size())
                throw std::out_of_range("Render graph pass index is out of range.");
            return m_Passes[passIndex].Transitions;
        }

        [[nodiscard]] const std::vector<RenderResourceLifetime>& Lifetimes()
            const noexcept
        {
            return m_Lifetimes;
        }

        [[nodiscard]] const std::vector<RenderAliasSlotDesc>& AliasSlots()
            const noexcept
        {
            return m_AliasSlots;
        }

        /// Total native memory capacity required when every transient alias slot
        /// receives one allocation. This is not the sum of logical resources;
        /// non-overlapping lifetimes share the capacity of their selected slot.
        [[nodiscard]] std::uint64_t TransientAllocationBytes() const noexcept
        {
            return m_TransientAllocationBytes;
        }

        /// Execute the immutable compiled schedule in dependency order and
        /// measure CPU recording time per pass. Transition hooks execute before
        /// each pass callback and are included in that pass timing because native
        /// barrier recording is part of frame command recording. Any exception
        /// stops execution immediately and propagates unchanged.
        [[nodiscard]] RenderGraphExecutionProfile Execute(
            const RenderGraphExecutionHooks& hooks = {}) const
        {
            RenderGraphExecutionProfile profile;
            profile.Passes.reserve(m_Passes.size());
            const auto graphStart = std::chrono::steady_clock::now();
            for (std::size_t index = 0u; index < m_Passes.size(); ++index)
            {
                const Pass& pass = m_Passes[index];
                const auto start = std::chrono::steady_clock::now();
                if (hooks.ApplyTransitions)
                    hooks.ApplyTransitions(index, pass.Name, pass.Transitions);
                if (pass.Execute) pass.Execute();
                const auto end = std::chrono::steady_clock::now();
                profile.Passes.push_back({ pass.Name,
                    std::chrono::duration<double, std::milli>(end - start).count() });
            }
            profile.TotalMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - graphStart).count();
            return profile;
        }
    };

    /// Backend-neutral frame dependency compiler.
    ///
    /// Input: logical resources, pass accesses, optional explicit dependencies,
    /// and CPU recording callbacks. Output: one deterministic topological pass
    /// order, resource transitions, transient lifetimes/alias slots, a physical
    /// transient reservation plan, and an executable profiling schedule.
    /// Backends consume this immutable plan rather than re-deriving hazards.
    class RenderGraph final
    {
        struct Resource final { RenderResourceDesc Desc; };
        struct Pass final
        {
            std::string Name;
            std::vector<RenderResourceAccess> Accesses;
            std::vector<RenderPassHandle> Dependencies;
            std::function<void()> Execute;
        };

        std::vector<Resource> m_Resources;
        std::vector<Pass> m_Passes;
        std::unordered_set<std::string> m_ResourceNames;
        std::unordered_set<std::string> m_PassNames;

        void ValidateResource(RenderResourceHandle handle) const
        {
            if (!handle.IsValid() || handle.Value >= m_Resources.size())
                throw std::out_of_range("Render graph resource handle is invalid.");
        }

        void ValidatePass(RenderPassHandle handle) const
        {
            if (!handle.IsValid() || handle.Value >= m_Passes.size())
                throw std::out_of_range("Render graph pass handle is invalid.");
        }

    public:
        [[nodiscard]] RenderResourceHandle AddResource(RenderResourceDesc desc)
        {
            if (desc.Name.empty())
                throw std::invalid_argument("Render graph resource name is empty.");
            if (!m_ResourceNames.insert(desc.Name).second)
                throw std::invalid_argument(
                    "Render graph resource names must be unique: " + desc.Name);
            if (desc.Kind == RenderResourceKind::External && desc.Transient)
                throw std::invalid_argument(
                    "External render graph resources cannot be transient.");
            if (desc.Kind != RenderResourceKind::External &&
                desc.EstimatedBytes == 0u)
                throw std::invalid_argument(
                    "Render graph texture/buffer resources require a byte estimate.");
            if (desc.Transient && desc.InitialState != RenderResourceState::Undefined)
                throw std::invalid_argument(
                    "Transient render resources must begin undefined.");
            const auto handle = RenderResourceHandle{
                static_cast<std::uint32_t>(m_Resources.size()) };
            m_Resources.push_back({ std::move(desc) });
            return handle;
        }

        [[nodiscard]] RenderPassHandle AddPass(std::string name,
            std::vector<RenderResourceAccess> accesses,
            std::function<void()> execute = {})
        {
            if (name.empty())
                throw std::invalid_argument("Render graph pass name is empty.");
            if (!m_PassNames.insert(name).second)
                throw std::invalid_argument(
                    "Render graph pass names must be unique: " + name);
            if (accesses.empty())
                throw std::invalid_argument(
                    "Render graph passes require at least one resource access.");
            std::unordered_set<std::uint32_t> seen;
            for (const auto& access : accesses)
            {
                ValidateResource(access.Resource);
                if (access.State == RenderResourceState::Undefined)
                    throw std::invalid_argument(
                        "Render graph pass access cannot request undefined state.");
                if (!seen.insert(access.Resource.Value).second)
                    throw std::invalid_argument(
                        "A render graph pass may access each resource once; use ReadWrite.");
            }
            const auto handle = RenderPassHandle{
                static_cast<std::uint32_t>(m_Passes.size()) };
            m_Passes.push_back({ std::move(name), std::move(accesses), {},
                std::move(execute) });
            return handle;
        }

        void DependsOn(RenderPassHandle pass, RenderPassHandle dependency)
        {
            ValidatePass(pass);
            ValidatePass(dependency);
            if (pass == dependency)
                throw std::invalid_argument("A render graph pass cannot depend on itself.");
            auto& dependencies = m_Passes[pass.Value].Dependencies;
            if (std::ranges::find(dependencies, dependency) != dependencies.end())
                throw std::invalid_argument("Render graph dependency is duplicated.");
            dependencies.push_back(dependency);
        }

        [[nodiscard]] CompiledRenderGraph Compile() const
        {
            if (m_Passes.empty())
                throw std::logic_error("Cannot compile an empty render graph.");
            const std::size_t passCount = m_Passes.size();
            std::vector<std::unordered_set<std::uint32_t>> edges(passCount);
            std::vector<std::uint32_t> indegree(passCount, 0u);
            const auto addEdge = [&](std::uint32_t from, std::uint32_t to)
            {
                if (from == to) return;
                if (edges[from].insert(to).second) ++indegree[to];
            };
            for (std::uint32_t pass = 0u; pass < passCount; ++pass)
                for (const auto dependency : m_Passes[pass].Dependencies)
                    addEdge(dependency.Value, pass);

            struct Hazard final
            {
                std::optional<std::uint32_t> Writer;
                std::vector<std::uint32_t> Readers;
                bool Initialized = false;
            };
            std::vector<Hazard> hazards(m_Resources.size());
            for (std::size_t resource = 0u; resource < m_Resources.size(); ++resource)
                hazards[resource].Initialized =
                    m_Resources[resource].Desc.InitialState != RenderResourceState::Undefined;
            for (std::uint32_t pass = 0u; pass < passCount; ++pass)
            {
                for (const auto& access : m_Passes[pass].Accesses)
                {
                    auto& hazard = hazards[access.Resource.Value];
                    if (access.Mode == RenderAccessMode::Read)
                    {
                        if (!hazard.Initialized)
                            throw std::logic_error("Render graph resource '" +
                                m_Resources[access.Resource.Value].Desc.Name +
                                "' is read before its first write.");
                        if (hazard.Writer) addEdge(*hazard.Writer, pass);
                        hazard.Readers.push_back(pass);
                    }
                    else
                    {
                        if (access.Mode == RenderAccessMode::ReadWrite &&
                            !hazard.Initialized)
                            throw std::logic_error("Render graph resource '" +
                                m_Resources[access.Resource.Value].Desc.Name +
                                "' is read-written before initialization.");
                        if (hazard.Writer) addEdge(*hazard.Writer, pass);
                        for (const auto reader : hazard.Readers) addEdge(reader, pass);
                        hazard.Readers.clear();
                        hazard.Writer = pass;
                        hazard.Initialized = true;
                    }
                }
            }

            std::priority_queue<std::uint32_t, std::vector<std::uint32_t>,
                std::greater<>> ready;
            for (std::uint32_t pass = 0u; pass < passCount; ++pass)
                if (indegree[pass] == 0u) ready.push(pass);
            std::vector<std::uint32_t> order;
            while (!ready.empty())
            {
                const auto pass = ready.top();
                ready.pop();
                order.push_back(pass);
                std::vector<std::uint32_t> sorted(edges[pass].begin(),
                    edges[pass].end());
                std::ranges::sort(sorted);
                for (const auto next : sorted)
                    if (--indegree[next] == 0u) ready.push(next);
            }
            if (order.size() != passCount)
                throw std::logic_error("Render graph dependencies contain a cycle.");

            CompiledRenderGraph compiled;
            compiled.m_Resources.reserve(m_Resources.size());
            for (const auto& resource : m_Resources)
                compiled.m_Resources.push_back(resource.Desc);

            std::vector<RenderResourceState> states;
            states.reserve(m_Resources.size());
            for (const auto& resource : m_Resources)
                states.push_back(resource.Desc.InitialState);
            std::vector<std::optional<RenderResourceLifetime>> lifetimes(
                m_Resources.size());
            for (std::uint32_t position = 0u; position < order.size(); ++position)
            {
                const Pass& source = m_Passes[order[position]];
                CompiledRenderGraph::Pass target{
                    source.Name, source.Accesses, {}, source.Execute };
                for (const auto& access : source.Accesses)
                {
                    auto& lifetime = lifetimes[access.Resource.Value];
                    if (!lifetime)
                        lifetime = RenderResourceLifetime{ access.Resource,
                            position, position };
                    else lifetime->LastPass = position;
                    if (states[access.Resource.Value] != access.State)
                    {
                        target.Transitions.push_back({ access.Resource,
                            states[access.Resource.Value], access.State });
                        states[access.Resource.Value] = access.State;
                    }
                }
                compiled.m_Passes.push_back(std::move(target));
            }

            struct AliasSlot final
            {
                RenderResourceKind Kind;
                std::uint64_t Capacity;
                std::uint32_t LastPass;
            };
            std::vector<AliasSlot> slots;
            std::vector<RenderResourceLifetime> transient;
            for (auto& lifetime : lifetimes)
                if (lifetime &&
                    m_Resources[lifetime->Resource.Value].Desc.Transient)
                    transient.push_back(*lifetime);
            std::ranges::sort(transient, [](const auto& left, const auto& right)
            {
                if (left.FirstPass != right.FirstPass)
                    return left.FirstPass < right.FirstPass;
                return left.Resource.Value < right.Resource.Value;
            });
            for (auto& lifetime : transient)
            {
                const auto& desc = m_Resources[lifetime.Resource.Value].Desc;
                std::optional<std::uint32_t> selected;
                for (std::uint32_t slot = 0u; slot < slots.size(); ++slot)
                    if (slots[slot].Kind == desc.Kind &&
                        slots[slot].Capacity >= desc.EstimatedBytes &&
                        slots[slot].LastPass < lifetime.FirstPass)
                    {
                        selected = slot;
                        break;
                    }
                if (!selected)
                {
                    selected = static_cast<std::uint32_t>(slots.size());
                    slots.push_back({ desc.Kind, desc.EstimatedBytes,
                        lifetime.LastPass });
                }
                else slots[*selected].LastPass = lifetime.LastPass;
                lifetime.AliasSlot = *selected;
            }

            compiled.m_AliasSlots.reserve(slots.size());
            for (std::uint32_t slot = 0u; slot < slots.size(); ++slot)
            {
                const AliasSlot& source = slots[slot];
                if (std::numeric_limits<std::uint64_t>::max() -
                    compiled.m_TransientAllocationBytes < source.Capacity)
                    throw std::overflow_error(
                        "Render graph transient allocation byte count overflowed.");
                compiled.m_TransientAllocationBytes += source.Capacity;
                compiled.m_AliasSlots.push_back({ slot, source.Kind,
                    source.Capacity });
            }

            for (const auto& lifetime : lifetimes)
                if (lifetime &&
                    !m_Resources[lifetime->Resource.Value].Desc.Transient)
                    compiled.m_Lifetimes.push_back(*lifetime);
            compiled.m_Lifetimes.insert(compiled.m_Lifetimes.end(),
                transient.begin(), transient.end());
            std::ranges::sort(compiled.m_Lifetimes,
                [](const auto& left, const auto& right)
                { return left.Resource.Value < right.Resource.Value; });
            return compiled;
        }
    };
}
