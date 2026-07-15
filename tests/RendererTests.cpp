#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

import Kairo.Renderer;

using namespace kairo::renderer;

TEST_CASE("Renderer window descriptions validate required dimensions", "[KairoRenderer][Types]")
{
    REQUIRE_NOTHROW(ValidateWindowDesc({ "Test", 1, 1, false }));
    REQUIRE_THROWS(ValidateWindowDesc({ "", 1, 1, false }));
    REQUIRE_THROWS(ValidateWindowDesc({ "Test", 0, 1, false }));
}

TEST_CASE("Showcase camera produces Vulkan-depth projection and advances its model", "[KairoRenderer][Camera]")
{
    ShowcaseCamera camera;
    const auto initialModel = camera.Model();
    const auto projection = camera.Projection(1600u, 900u);

    camera.Advance(0.5f);

    CHECK(camera.Model() != initialModel);
    CHECK(projection(0, 0) > 0.0f);
    CHECK(projection(1, 1) > 0.0f);
    CHECK(projection(3, 2) == -1.0f);
}

TEST_CASE("Debug draw emits deterministic AABB edges and axes", "[KairoRenderer][Debug]")
{
    DebugDrawList draw;
    draw.AddAABB({ -1.0f, -2.0f, -3.0f }, { 1.0f, 2.0f, 3.0f });
    REQUIRE(draw.Lines().size() == 12u);
    draw.AddAxes({ 0.0f, 0.0f, 0.0f }, 2.0f);
    CHECK(draw.Lines().size() == 15u);
    REQUIRE_THROWS(draw.AddAABB({ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }));
}

TEST_CASE("Debug draw emits sphere, capsule, and contact geometry", "[KairoRenderer][Debug]")
{
    DebugDrawList draw;
    draw.AddWireSphere({ 0.0f, 0.0f, 0.0f }, 1.0f, 8u);
    REQUIRE(draw.Lines().size() == 24u);
    draw.AddWireCapsule({ 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, 0.5f, 4u);
    CHECK(draw.Lines().size() == 52u);
    draw.AddContactNormal({ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    CHECK(draw.Lines().size() == 53u);
}
