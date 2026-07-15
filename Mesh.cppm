module;

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

export module Kairo.Renderer.Mesh;

import Kairo.Foundation.Math;

export namespace kairo::renderer
{
    /// CPU-side vertex contract shared by mesh upload and the basic forward
    /// shader. Positions and colors are object-local; transforms are supplied
    /// by the renderer camera uniform rather than baked into asset data.
    struct MeshVertex final
    {
        kairo::foundation::math::Vec3f Position{};
        kairo::foundation::math::Vec3f Color{ 1.0f, 1.0f, 1.0f };
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

        /// Output: a 24-vertex/36-index cube with face colors. Duplicating
        /// corners preserves a clean path for future per-face normals/UVs.
        [[nodiscard]] static Mesh MakeCube()
        {
            const std::vector<MeshVertex> vertices{
                {{-1,-1, 1},{0.95f,0.25f,0.25f}}, {{ 1,-1, 1},{0.95f,0.25f,0.25f}}, {{ 1, 1, 1},{0.95f,0.25f,0.25f}}, {{-1, 1, 1},{0.95f,0.25f,0.25f}},
                {{ 1,-1,-1},{0.25f,0.5f,0.95f}}, {{-1,-1,-1},{0.25f,0.5f,0.95f}}, {{-1, 1,-1},{0.25f,0.5f,0.95f}}, {{ 1, 1,-1},{0.25f,0.5f,0.95f}},
                {{-1,-1,-1},{0.25f,0.9f,0.45f}}, {{-1,-1, 1},{0.25f,0.9f,0.45f}}, {{-1, 1, 1},{0.25f,0.9f,0.45f}}, {{-1, 1,-1},{0.25f,0.9f,0.45f}},
                {{ 1,-1, 1},{0.95f,0.75f,0.2f}}, {{ 1,-1,-1},{0.95f,0.75f,0.2f}}, {{ 1, 1,-1},{0.95f,0.75f,0.2f}}, {{ 1, 1, 1},{0.95f,0.75f,0.2f}},
                {{-1, 1, 1},{0.65f,0.35f,0.95f}}, {{ 1, 1, 1},{0.65f,0.35f,0.95f}}, {{ 1, 1,-1},{0.65f,0.35f,0.95f}}, {{-1, 1,-1},{0.65f,0.35f,0.95f}},
                {{-1,-1,-1},{0.2f,0.85f,0.85f}}, {{ 1,-1,-1},{0.2f,0.85f,0.85f}}, {{ 1,-1, 1},{0.2f,0.85f,0.85f}}, {{-1,-1, 1},{0.2f,0.85f,0.85f}}
            };
            const std::vector<std::uint32_t> indices{
                0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
                12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23
            };
            return Mesh(vertices, indices);
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
