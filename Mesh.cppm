module;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

export module Kairo.Renderer.Mesh;

import Kairo.Foundation.Math;
import Kairo.Assets.MeshArtifact;

export namespace kairo::renderer
{
    /// CPU-side vertex contract shared by mesh upload and the basic forward
    /// shader. Positions and colors are object-local; transforms are supplied
    /// by the renderer camera uniform rather than baked into asset data.
    struct MeshVertex final
    {
        kairo::foundation::math::Vec3f Position{};
        kairo::foundation::math::Vec3f Color{ 1.0f, 1.0f, 1.0f };
        kairo::foundation::math::Vec3f Normal{ 0.0f, 1.0f, 0.0f };
    };

    /// Indexed triangle mesh with explicit ownership of CPU geometry.
    /// Input: non-empty vertices and indices divisible by three.
    /// Output: validated immutable-by-convention data for a renderer upload.
    /// Task: separate authored geometry from Vulkan resource lifetime so the
    /// same mesh description can later come from an asset/import pipeline.
    class Mesh final
    {
    public:
        Mesh(std::vector<MeshVertex> vertices, std::vector<std::uint32_t> indices)
            : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
        {
            Validate();
        }

        [[nodiscard]] const std::vector<MeshVertex>& Vertices() const noexcept { return m_Vertices; }
        [[nodiscard]] const std::vector<std::uint32_t>& Indices() const noexcept { return m_Indices; }
        [[nodiscard]] std::size_t VertexBytes() const noexcept { return m_Vertices.size() * sizeof(MeshVertex); }
        [[nodiscard]] std::size_t IndexBytes() const noexcept { return m_Indices.size() * sizeof(std::uint32_t); }

        /// Input: a validated, backend-neutral KairoAssets triangle mesh and
        /// a finite non-negative linear-RGB vertex color.
        /// Output: renderer-owned vertices and indices ready for GPU upload.
        /// Task: keep source parsing and derived-data layout in KairoAssets
        /// while adapting that canonical representation exactly once at the
        /// renderer boundary. The current forward shader requires normals;
        /// UVs remain in the artifact until the renderer texture path consumes
        /// them instead of being duplicated in an ad-hoc renderer asset type.
        [[nodiscard]] static Mesh FromArtifact(
            const kairo::assets::MeshArtifactData& artifact,
            const kairo::foundation::math::Vec3f& color = { 1.0f, 1.0f, 1.0f })
        {
            kairo::assets::ValidateMeshArtifactData(artifact);
            if (!artifact.HasNormals)
                throw std::invalid_argument("Renderer mesh artifacts require vertex normals.");
            if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) ||
                color.x < 0.0f || color.y < 0.0f || color.z < 0.0f)
                throw std::invalid_argument("Renderer mesh color must be finite and non-negative.");

            std::vector<MeshVertex> vertices;
            vertices.reserve(artifact.Vertices.size());
            for (const kairo::assets::MeshArtifactVertex& source : artifact.Vertices)
            {
                vertices.push_back({
                    { source.Position[0], source.Position[1], source.Position[2] },
                    color,
                    { source.Normal[0], source.Normal[1], source.Normal[2] }
                });
            }
            return Mesh(std::move(vertices), artifact.Indices);
        }

        /// Output: a 24-vertex/36-index cube with face colors. Duplicating
        /// corners preserves a clean path for future per-face normals/UVs.
        [[nodiscard]] static Mesh MakeCube()
        {
            std::vector<MeshVertex> vertices{
                {{-1,-1, 1},{0.95f,0.25f,0.25f}}, {{ 1,-1, 1},{0.95f,0.25f,0.25f}}, {{ 1, 1, 1},{0.95f,0.25f,0.25f}}, {{-1, 1, 1},{0.95f,0.25f,0.25f}},
                {{ 1,-1,-1},{0.25f,0.5f,0.95f}}, {{-1,-1,-1},{0.25f,0.5f,0.95f}}, {{-1, 1,-1},{0.25f,0.5f,0.95f}}, {{ 1, 1,-1},{0.25f,0.5f,0.95f}},
                {{-1,-1,-1},{0.25f,0.9f,0.45f}}, {{-1,-1, 1},{0.25f,0.9f,0.45f}}, {{-1, 1, 1},{0.25f,0.9f,0.45f}}, {{-1, 1,-1},{0.25f,0.9f,0.45f}},
                {{ 1,-1, 1},{0.95f,0.75f,0.2f}}, {{ 1,-1,-1},{0.95f,0.75f,0.2f}}, {{ 1, 1,-1},{0.95f,0.75f,0.2f}}, {{ 1, 1, 1},{0.95f,0.75f,0.2f}},
                {{-1, 1, 1},{0.65f,0.35f,0.95f}}, {{ 1, 1, 1},{0.65f,0.35f,0.95f}}, {{ 1, 1,-1},{0.65f,0.35f,0.95f}}, {{-1, 1,-1},{0.65f,0.35f,0.95f}},
                {{-1,-1,-1},{0.2f,0.85f,0.85f}}, {{ 1,-1,-1},{0.2f,0.85f,0.85f}}, {{ 1,-1, 1},{0.2f,0.85f,0.85f}}, {{-1,-1, 1},{0.2f,0.85f,0.85f}}
            };
            const kairo::foundation::math::Vec3f faceNormals[]{
                { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }, { -1.0f, 0.0f, 0.0f },
                { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }
            };
            for (std::size_t face = 0u; face < 6u; ++face)
                for (std::size_t corner = 0u; corner < 4u; ++corner)
                    vertices[face * 4u + corner].Normal = faceNormals[face];
            const std::vector<std::uint32_t> indices{
                0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
                12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23
            };
            return Mesh(vertices, indices);
        }

        /// Output: a two-triangle XZ ground plane with upward-facing normals.
        /// Task: provide a real authoring primitive for floors and blockouts;
        /// scale belongs to the scene transform rather than baked vertices.
        [[nodiscard]] static Mesh MakePlane()
        {
            return Mesh({
                {{-1.0f, 0.0f, -1.0f}, {0.38f, 0.52f, 0.42f}, {0.0f, 1.0f, 0.0f}},
                {{ 1.0f, 0.0f, -1.0f}, {0.38f, 0.52f, 0.42f}, {0.0f, 1.0f, 0.0f}},
                {{ 1.0f, 0.0f,  1.0f}, {0.38f, 0.52f, 0.42f}, {0.0f, 1.0f, 0.0f}},
                {{-1.0f, 0.0f,  1.0f}, {0.38f, 0.52f, 0.42f}, {0.0f, 1.0f, 0.0f}}
            }, { 0u, 2u, 1u, 0u, 3u, 2u });
        }

        /// Input: latitude/longitude tessellation counts, both at least three.
        /// Output: a closed unit UV sphere with smooth normals.
        /// Task: expose a robust blockout primitive; this is renderer geometry,
        /// not a substitute for an editable mesh asset or source importer.
        [[nodiscard]] static Mesh MakeUVSphere(std::uint32_t rings = 16u, std::uint32_t segments = 24u)
        {
            if (rings < 3u || segments < 3u || rings > 512u || segments > 512u)
                throw std::invalid_argument("UV sphere tessellation must be in [3, 512].");
            constexpr float tau = 6.28318530718f;
            constexpr float pi = 3.14159265359f;
            std::vector<MeshVertex> vertices;
            std::vector<std::uint32_t> indices;
            vertices.reserve(static_cast<std::size_t>(rings + 1u) * (segments + 1u));
            indices.reserve(static_cast<std::size_t>(rings) * segments * 6u);
            for (std::uint32_t ring = 0u; ring <= rings; ++ring)
            {
                const float theta = pi * static_cast<float>(ring) / static_cast<float>(rings);
                const float y = std::cos(theta);
                const float radius = std::sin(theta);
                for (std::uint32_t segment = 0u; segment <= segments; ++segment)
                {
                    const float phi = tau * static_cast<float>(segment) / static_cast<float>(segments);
                    const kairo::foundation::math::Vec3f normal{
                        radius * std::cos(phi), y, radius * std::sin(phi) };
                    vertices.push_back({ normal, { 0.34f, 0.62f, 0.95f }, normal });
                }
            }
            const std::uint32_t stride = segments + 1u;
            for (std::uint32_t ring = 0u; ring < rings; ++ring)
                for (std::uint32_t segment = 0u; segment < segments; ++segment)
                {
                    const std::uint32_t a = ring * stride + segment;
                    const std::uint32_t b = a + stride;
                    indices.insert(indices.end(), { a, b, a + 1u, a + 1u, b, b + 1u });
                }
            return Mesh(std::move(vertices), std::move(indices));
        }

        /// Input: positive radial/height segment counts, each at least three.
        /// Output: a closed unit-radius cylinder centered at the origin.
        /// Task: provide a practical primitive with cap and side normals for
        /// blockout and physics visualisation without external mesh tooling.
        [[nodiscard]] static Mesh MakeCylinder(std::uint32_t segments = 24u)
        {
            if (segments < 3u || segments > 512u)
                throw std::invalid_argument("Cylinder tessellation must be in [3, 512].");
            constexpr float tau = 6.28318530718f;
            std::vector<MeshVertex> vertices;
            std::vector<std::uint32_t> indices;
            vertices.reserve(static_cast<std::size_t>(segments + 1u) * 4u + 2u);
            indices.reserve(static_cast<std::size_t>(segments) * 12u);
            for (std::uint32_t index = 0u; index <= segments; ++index)
            {
                const float angle = tau * static_cast<float>(index) / static_cast<float>(segments);
                const float x = std::cos(angle);
                const float z = std::sin(angle);
                vertices.push_back({ { x, -1.0f, z }, { 0.96f, 0.58f, 0.23f }, { x, 0.0f, z } });
                vertices.push_back({ { x,  1.0f, z }, { 0.96f, 0.58f, 0.23f }, { x, 0.0f, z } });
            }
            const std::uint32_t topCenter = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back({ { 0.0f, 1.0f, 0.0f }, { 0.96f, 0.58f, 0.23f }, { 0.0f, 1.0f, 0.0f } });
            const std::uint32_t bottomCenter = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back({ { 0.0f,-1.0f, 0.0f }, { 0.96f, 0.58f, 0.23f }, { 0.0f,-1.0f, 0.0f } });
            for (std::uint32_t index = 0u; index < segments; ++index)
            {
                const std::uint32_t bottom = index * 2u;
                const std::uint32_t top = bottom + 1u;
                const std::uint32_t nextBottom = bottom + 2u;
                const std::uint32_t nextTop = bottom + 3u;
                indices.insert(indices.end(), { bottom, top, nextBottom, nextBottom, top, nextTop,
                    topCenter, nextTop, top, bottomCenter, bottom, nextBottom });
            }
            return Mesh(std::move(vertices), std::move(indices));
        }

    private:
        std::vector<MeshVertex> m_Vertices;
        std::vector<std::uint32_t> m_Indices;
        void Validate() const
        {
            if (m_Vertices.empty() || m_Indices.empty() || m_Indices.size() % 3u != 0u) throw std::invalid_argument("Mesh requires vertices and triangle indices.");
            for (const std::uint32_t index : m_Indices)
                if (index >= m_Vertices.size()) throw std::out_of_range("Mesh index exceeds vertex count.");
        }
    };
}
