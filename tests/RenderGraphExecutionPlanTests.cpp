#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import Kairo.Renderer;

using namespace kairo::renderer;

TEST_CASE("Compiled render graph exposes backend allocation and access plans",
    "[KairoRenderer][RenderGraph][ExecutionPlan]")
{
    RenderGraph graph;
    const auto shadow = graph.AddResource({
        "Shadow", RenderResourceKind::Texture, 4096u, true });
    const auto gbuffer = graph.AddResource({
        "GBuffer", RenderResourceKind::Texture, 8192u, true });
    const auto post = graph.AddResource({
        "Post", RenderResourceKind::Texture, 4096u, true });
    const auto upload = graph.AddResource({
        "Upload", RenderResourceKind::Buffer, 2048u, true });
    const auto swapchain = graph.AddResource({
        "Swapchain", RenderResourceKind::External, 0u, false,
        RenderResourceState::Present });

    graph.AddPass("Upload", {
        { upload, RenderAccessMode::Write,
            RenderResourceState::CopyDestination }
    });
    graph.AddPass("Shadow", {
        { shadow, RenderAccessMode::Write,
            RenderResourceState::DepthAttachment }
    });
    graph.AddPass("Geometry", {
        { upload, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { shadow, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { gbuffer, RenderAccessMode::Write,
            RenderResourceState::ColorAttachment }
    });
    graph.AddPass("Post", {
        { gbuffer, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { post, RenderAccessMode::Write,
            RenderResourceState::ColorAttachment }
    });
    graph.AddPass("Present", {
        { post, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { swapchain, RenderAccessMode::Write, RenderResourceState::Present }
    });

    const auto compiled = graph.Compile();

    REQUIRE(compiled.ResourceCount() == 5u);
    CHECK(compiled.Resource(shadow).Name == "Shadow");
    CHECK(compiled.Resource(swapchain).Kind == RenderResourceKind::External);

    REQUIRE(compiled.PassCount() == 5u);
    REQUIRE(compiled.Accesses(2u).size() == 3u);
    CHECK(compiled.Accesses(2u)[0u].Resource == upload);
    CHECK(compiled.Accesses(2u)[2u].Mode == RenderAccessMode::Write);

    REQUIRE(compiled.AliasSlots().size() == 3u);
    CHECK(compiled.AliasSlots()[0u].Slot == 0u);
    CHECK(compiled.AliasSlots()[0u].Kind == RenderResourceKind::Buffer);
    CHECK(compiled.AliasSlots()[0u].CapacityBytes == 2048u);
    CHECK(compiled.TransientAllocationBytes() == 14336u);

    const auto& lifetimes = compiled.Lifetimes();
    REQUIRE(lifetimes.size() == 5u);
    CHECK(lifetimes[shadow.Value].AliasSlot == lifetimes[post.Value].AliasSlot);
    CHECK(lifetimes[gbuffer.Value].AliasSlot != lifetimes[post.Value].AliasSlot);
}

TEST_CASE("Render graph transition hook runs before pass recording",
    "[KairoRenderer][RenderGraph][ExecutionPlan]")
{
    RenderGraph graph;
    const auto color = graph.AddResource({
        "Color", RenderResourceKind::Texture, 1024u, true });
    const auto present = graph.AddResource({
        "Present", RenderResourceKind::External, 0u, false,
        RenderResourceState::Present });

    std::vector<std::string> order;
    graph.AddPass("Draw", {
        { color, RenderAccessMode::Write,
            RenderResourceState::ColorAttachment }
    }, [&] { order.push_back("pass:Draw"); });
    graph.AddPass("Present", {
        { color, RenderAccessMode::Read, RenderResourceState::ShaderRead },
        { present, RenderAccessMode::Write, RenderResourceState::Present }
    }, [&] { order.push_back("pass:Present"); });

    const auto compiled = graph.Compile();
    RenderGraphExecutionHooks hooks;
    hooks.ApplyTransitions = [&](std::size_t index, std::string_view name,
        const std::vector<RenderResourceTransition>& transitions)
    {
        order.push_back("barrier:" + std::string(name));
        if (index == 0u)
        {
            REQUIRE(transitions.size() == 1u);
            CHECK(transitions[0u].Before == RenderResourceState::Undefined);
            CHECK(transitions[0u].After == RenderResourceState::ColorAttachment);
        }
    };

    const auto profile = compiled.Execute(hooks);

    CHECK(order == std::vector<std::string>{
        "barrier:Draw", "pass:Draw", "barrier:Present", "pass:Present" });
    REQUIRE(profile.Passes.size() == 2u);
    CHECK(profile.Passes[0u].Name == "Draw");
}

TEST_CASE("Render graph rejects ambiguous external and uninitialized resources",
    "[KairoRenderer][RenderGraph][Validation]")
{
    RenderGraph invalidExternal;
    REQUIRE_THROWS_AS(invalidExternal.AddResource({
        "External", RenderResourceKind::External, 0u, true }),
        std::invalid_argument);

    RenderGraph uninitializedPersistent;
    const auto history = uninitializedPersistent.AddResource({
        "History", RenderResourceKind::Texture, 4096u, false,
        RenderResourceState::Undefined });
    uninitializedPersistent.AddPass("ReadHistory", {
        { history, RenderAccessMode::Read, RenderResourceState::ShaderRead }
    });
    REQUIRE_THROWS_AS(uninitializedPersistent.Compile(), std::logic_error);

    RenderGraph initializedPersistent;
    const auto initialized = initializedPersistent.AddResource({
        "History", RenderResourceKind::Texture, 4096u, false,
        RenderResourceState::ShaderRead });
    initializedPersistent.AddPass("ReadHistory", {
        { initialized, RenderAccessMode::Read, RenderResourceState::ShaderRead }
    });
    REQUIRE_NOTHROW(initializedPersistent.Compile());
}

TEST_CASE("Transition hook failures stop later render passes",
    "[KairoRenderer][RenderGraph][ExecutionPlan]")
{
    RenderGraph graph;
    const auto resource = graph.AddResource({
        "Resource", RenderResourceKind::Buffer, 64u, true });
    std::uint32_t executed = 0u;
    graph.AddPass("Write", {
        { resource, RenderAccessMode::Write,
            RenderResourceState::CopyDestination }
    }, [&] { ++executed; });
    graph.AddPass("Read", {
        { resource, RenderAccessMode::Read, RenderResourceState::ShaderRead }
    }, [&] { ++executed; });

    const auto compiled = graph.Compile();
    RenderGraphExecutionHooks hooks;
    hooks.ApplyTransitions = [](std::size_t index, std::string_view,
        const std::vector<RenderResourceTransition>&)
    {
        if (index == 1u)
            throw std::runtime_error("backend barrier failure");
    };

    REQUIRE_THROWS_WITH(compiled.Execute(hooks), "backend barrier failure");
    CHECK(executed == 1u);
}
