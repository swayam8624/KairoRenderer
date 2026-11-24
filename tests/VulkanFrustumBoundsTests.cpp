#include "../detail/VulkanFrustumBounds.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace
{
    using kairo::renderer::detail::LocalBounds;
    using kairo::renderer::detail::OutsideVulkanFrustum;
    struct Point { float x, y, z; };
    struct Matrix
    {
        std::array<float, 16> Data{};
        float operator()(std::size_t r, std::size_t c) const { return Data[r * 4 + c]; }
        static Matrix Identity()
        {
            Matrix m;
            for (std::size_t i = 0; i < 4; ++i) m.Data[i * 5] = 1;
            return m;
        }
    };
    void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
    std::array<float, 4> Multiply(const Matrix& m, const std::array<float,4>& p)
    {
        std::array<float, 4> result{};
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t j = 0; j < 4; ++j) result[i] += m(i,j) * p[j];
        return result;
    }
    bool Inside(const std::array<float,4>& p)
    {
        return p[0] >= -p[3] && p[0] <= p[3] && p[1] >= -p[3] &&
            p[1] <= p[3] && p[2] >= 0 && p[2] <= p[3];
    }
}
int main()
{
    const auto identity = Matrix::Identity();
    LocalBounds box;
    box.Include(Point{-.25f,-.25f,.25f});
    box.Include(Point{.25f,.25f,.75f});
    Check(!OutsideVulkanFrustum(box,identity,identity,identity), "Visible box culled");
    for (unsigned axis = 0; axis < 3; ++axis)
        for (float sign : {-1.0f,1.0f})
        {
            auto model = identity;
            model.Data[axis * 4 + 3] = sign * 3.0f;
            Check(OutsideVulkanFrustum(box,model,identity,identity), "Outside plane accepted");
        }
    auto tangent = identity;
    tangent.Data[3] = 1.25f;
    Check(!OutsideVulkanFrustum(box,tangent,identity,identity), "Touching clip boundary culled");
    auto mirror = identity;
    mirror.Data[0] = -3;
    mirror.Data[1] = 2;
    Check(!OutsideVulkanFrustum(box,mirror,identity,identity), "Shear/reflection culled");
    auto perspective = identity;
    perspective.Data[14] = 1;
    perspective.Data[15] = 0;
    perspective.Data[10] = .5f;
    LocalBounds crossing;
    crossing.Include(Point{-.25f,-.25f,-1});
    crossing.Include(Point{.25f,.25f,1});
    Check(!OutsideVulkanFrustum(crossing,identity,identity,perspective), "Eye-crossing box culled");
    auto invalid = identity;
    invalid.Data[0] = std::numeric_limits<float>::infinity();
    Check(!OutsideVulkanFrustum(box,invalid,identity,identity), "Nonfinite case did not fail open");

    std::mt19937 rng(82701u);
    std::uniform_real_distribution<float> random(-4.f,4.f), unit(0.f,1.f);
    unsigned rejected = 0, preparedRejected = 0;
    std::size_t samples = 0;
    for (unsigned trial = 0; trial < 25000; ++trial)
    {
        Matrix m = identity, v = identity, p = identity;
        for (unsigned i = 0; i < 12; ++i) m.Data[i] = random(rng);
        v.Data[3] = random(rng);
        v.Data[7] = random(rng);
        v.Data[11] = random(rng);
        if (trial % 2) p = perspective;
        const bool referenceCulled = OutsideVulkanFrustum(box,m,v,p);
        const kairo::renderer::detail::PreparedVulkanFrustum prepared(v,p);
        const bool culled = prepared.Outside(box,m);
        Check(!culled || referenceCulled, "Prepared support rejected a reference-visible box");
        if (referenceCulled) ++rejected;
        if (culled) ++preparedRejected;
        for (unsigned j = 0; j < 32; ++j)
        {
            std::array<float,4> point{};
            for (unsigned axis = 0; axis < 3; ++axis)
                point[axis] = static_cast<float>(box.Min[axis] +
                    unit(rng) * (box.Max[axis] - box.Min[axis]));
            point[3] = 1;
            const auto clip = Multiply(p,Multiply(v,Multiply(m,point)));
            Check(!culled || !Inside(clip), "Culled a box containing a visible sample");
            ++samples;
        }
    }
    Check(rejected > 1000 && preparedRejected > 1000, "Culling test is vacuous");
    const kairo::renderer::detail::PreparedVulkanFrustum prepared(identity, identity);
    Check(!prepared.Outside(box,tangent), "Prepared support culled tangent box");
    Check(!prepared.Outside(box,invalid), "Prepared support failed open on invalid model");
    auto extreme = identity;
    extreme.Data[0] = 1.0e20f;
    Check(!prepared.Outside(box,extreme), "Prepared support rejected extreme model");
    std::cout << samples << " clip-space samples; " << rejected <<
        " reference-rejected boxes; " << preparedRejected <<
        " prepared rejections; conservative support and fail-open passed\n";
}
