from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f"{label} marker not found")
    return text.replace(old, new, 1)


# ----- Mesh: optional parallel four-weight skin stream -----
path = Path("Mesh.cppm")
text = path.read_text()
text = replace_once(
    text,
    "import Kairo.Assets.MeshArtifact;\n",
    "import Kairo.Assets.MeshArtifact;\n"
    "import Kairo.Assets.GltfSceneArtifact;\n"
    "import Kairo.Renderer.Skinning;\n",
    "mesh imports",
)
text = replace_once(
    text,
    "        [[nodiscard]] const std::vector<std::uint32_t>& Indices() const noexcept { return m_Indices; }\n"
    "        [[nodiscard]] std::size_t VertexBytes() const noexcept { return m_Vertices.size() * sizeof(MeshVertex); }\n",
    "        [[nodiscard]] const std::vector<std::uint32_t>& Indices() const noexcept { return m_Indices; }\n"
    "        [[nodiscard]] const std::vector<SkinVertexInfluence>& Skinning() const noexcept { return m_Skinning; }\n"
    "        [[nodiscard]] bool IsSkinned() const noexcept { return !m_Skinning.empty(); }\n"
    "        [[nodiscard]] std::size_t VertexBytes() const noexcept { return m_Vertices.size() * sizeof(MeshVertex); }\n"
    "        [[nodiscard]] std::size_t SkinBytes() const noexcept { return m_Skinning.size() * sizeof(SkinVertexInfluence); }\n"
    "        [[nodiscard]] std::size_t RequiredJointCount() const noexcept\n"
    "        {\n"
    "            std::size_t required = 0u;\n"
    "            for (const auto& influence : m_Skinning)\n"
    "                for (std::size_t slot = 0u; slot < influence.Weights.size(); ++slot)\n"
    "                    if (influence.Weights[slot] > 0.0f)\n"
    "                        required = std::max(required,\n"
    "                            static_cast<std::size_t>(influence.Joints[slot]) + 1u);\n"
    "            return required;\n"
    "        }\n"
    "        [[nodiscard]] std::size_t IndexBytes() const noexcept { return m_Indices.size() * sizeof(std::uint32_t); }\n",
    "mesh skin accessors",
)
# std::max is used by RequiredJointCount.
text = replace_once(text, "#include <cmath>\n", "#include <algorithm>\n#include <cmath>\n", "mesh algorithm include")

marker = "        /// Output: a 24-vertex/36-index cube with face colors. Duplicating\n"
insert = '''        /// Input: one validated glTF primitive. Output: the existing static
        /// vertex stream plus an optional parallel four-weight skin stream.
        /// Task: preserve the backend's established vertex ABI while allowing
        /// native APIs to bind skinning as a second stream/buffer.
        [[nodiscard]] static Mesh FromGltfPrimitive(
            const kairo::assets::GltfPrimitiveData& primitive,
            const kairo::foundation::math::Vec3f& color = { 1.0f, 1.0f, 1.0f })
        {
            Mesh result = FromArtifact(primitive.Mesh, color);
            if (!primitive.Skinning.empty())
            {
                if (primitive.Skinning.size() != result.m_Vertices.size())
                    throw std::invalid_argument(
                        "Renderer glTF skin stream must match the vertex count.");
                result.m_Skinning.reserve(primitive.Skinning.size());
                for (const auto& source : primitive.Skinning)
                {
                    SkinVertexInfluence influence{ source.Joints, source.Weights };
                    ValidateSkinVertexInfluence(influence);
                    result.m_Skinning.push_back(influence);
                }
                result.Validate();
            }
            return result;
        }

'''
if insert not in text:
    if marker not in text:
        raise SystemExit("gltf primitive insertion marker not found")
    text = text.replace(marker, insert + marker, 1)

text = replace_once(
    text,
    "        std::vector<MeshVertex> m_Vertices;\n        std::vector<std::uint32_t> m_Indices;\n",
    "        std::vector<MeshVertex> m_Vertices;\n"
    "        std::vector<std::uint32_t> m_Indices;\n"
    "        std::vector<SkinVertexInfluence> m_Skinning;\n",
    "mesh skin storage",
)
text = replace_once(
    text,
    "            for (const std::uint32_t index : m_Indices)\n"
    "                if (index >= m_Vertices.size()) throw std::out_of_range(\"Mesh index exceeds vertex count.\");\n",
    "            for (const std::uint32_t index : m_Indices)\n"
    "                if (index >= m_Vertices.size()) throw std::out_of_range(\"Mesh index exceeds vertex count.\");\n"
    "            if (!m_Skinning.empty())\n"
    "            {\n"
    "                if (m_Skinning.size() != m_Vertices.size())\n"
    "                    throw std::invalid_argument(\"Mesh skin stream must match vertex count.\");\n"
    "                for (const auto& influence : m_Skinning)\n"
    "                    ValidateSkinVertexInfluence(influence);\n"
    "            }\n",
    "mesh validation",
)
path.write_text(text)

# ----- Asset adapter: skinned node palettes are already asset-space -----
path = Path("AssetAdapters.cppm")
text = path.read_text()
text = replace_once(
    text,
    "        std::uint32_t PrimitiveIndex = 0u;\n",
    "        std::uint32_t PrimitiveIndex = 0u;\n"
    "        std::uint32_t SkinIndex = kairo::assets::GltfMissingIndex;\n",
    "gltf render primitive skin index",
)
old = '''                result.Primitives.push_back({ Mesh::FromArtifact(primitive.Mesh),
                    material, world[nodeIndex], static_cast<std::uint32_t>(nodeIndex),
                    primitiveIndex });
'''
new = '''                const bool skinned = source.Nodes[nodeIndex].SkinIndex !=
                    kairo::assets::GltfMissingIndex;
                Mesh geometry = skinned
                    ? Mesh::FromGltfPrimitive(primitive)
                    : Mesh::FromArtifact(primitive.Mesh);
                // EngineCore skin palettes are jointWorld * inverseBind in
                // imported-asset space. Applying the mesh node world again
                // would double-transform skinned vertices, so only static
                // primitives retain their hierarchy-composed LocalToAsset.
                const auto localToAsset = skinned
                    ? kairo::foundation::math::Mat4f::Identity()
                    : world[nodeIndex];
                result.Primitives.push_back({ std::move(geometry), material,
                    localToAsset, static_cast<std::uint32_t>(nodeIndex),
                    primitiveIndex, source.Nodes[nodeIndex].SkinIndex });
'''
text = replace_once(text, old, new, "gltf render primitive construction")
path.write_text(text)

# ----- RenderScene: carry palette, keep hierarchy logic outside renderer -----
path = Path("RenderScene.cppm")
text = path.read_text()
text = replace_once(
    text,
    "import Kairo.Renderer.Texture;\n",
    "import Kairo.Renderer.Texture;\nimport Kairo.Renderer.Skinning;\n",
    "render scene skin import",
)
text = replace_once(
    text,
    "        PBRMaterial Material{};\n",
    "        PBRMaterial Material{};\n"
    "        /// Empty for a static draw. For a skinned draw these matrices are\n"
    "        /// already in imported-asset space (jointWorld * inverseBind).\n"
    "        SkinPalette Skinning{};\n",
    "mesh draw skin palette",
)
text = replace_once(
    text,
    "            draw.Material.Validate();\n",
    "            draw.Material.Validate();\n            draw.Skinning.Validate();\n",
    "render scene palette validation",
)
path.write_text(text)

# ----- Umbrella / CMake / dependency pin -----
path = Path("KairoRenderer.cppm")
text = path.read_text()
text = replace_once(
    text,
    "export import Kairo.Renderer.Mesh;\n",
    "export import Kairo.Renderer.Mesh;\nexport import Kairo.Renderer.Skinning;\n",
    "renderer umbrella skin export",
)
path.write_text(text)

path = Path("CMakeLists.txt")
text = path.read_text()
text = replace_once(
    text,
    "set(KAIRO_RENDERER_ASSETS_REVISION ad9715df00fffaa8a09f22afc990bde557689f53)\n",
    "set(KAIRO_RENDERER_ASSETS_REVISION 9e63b89520a053a1144a0350415a9f107b66eafa)\n",
    "renderer assets v2 pin",
)
text = replace_once(
    text,
    "    Mesh.cppm\n    Texture.cppm\n",
    "    Mesh.cppm\n    Skinning.cppm\n    Texture.cppm\n",
    "renderer skin module registration",
)
text = replace_once(
    text,
    "        tests/RendererTests.cpp\n        tests/RenderGraphExecutionPlanTests.cpp)\n",
    "        tests/RendererTests.cpp\n        tests/RenderGraphExecutionPlanTests.cpp\n        tests/SkinningContractTests.cpp)\n",
    "renderer skin test registration",
)
path.write_text(text)

# Remove one-shot patch machinery after the workflow commits the real source.
Path(".github/workflows/apply-gpu-skinning-contract.yml").unlink(missing_ok=True)
Path(".github/scripts/apply_gpu_skinning_contract.py").unlink(missing_ok=True)
