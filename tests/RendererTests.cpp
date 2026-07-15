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
