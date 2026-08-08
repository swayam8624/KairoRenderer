module;

#include <algorithm>
#include <functional>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

export module Kairo.Renderer.AssetAdapters;

import Kairo.Assets.MaterialArtifact;
import Kairo.Assets.GltfSceneArtifact;
import Kairo.Assets.Metadata;
import Kairo.Assets.TextureArtifact;
import Kairo.Foundation.Math;
import Kairo.Renderer.Material;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.Texture;

export namespace kairo::renderer
{
    /// Resolves a persistent texture asset reference to a renderer-owned GPU
    /// handle. Editor and Player therefore share conversion rules without
    /// sharing GPU ownership or import-cache policy.
    using MaterialTextureResolver =
        std::function<TextureHandle(kairo::assets::TextureAssetHandle)>;

    /// Input: one validated KairoAssets material artifact and a resolver for
    /// its optional texture references.
    /// Output: the complete real-time metallic/roughness material descriptor.
    /// Task: preserve channel semantics and alpha policy at one shared boundary.
    /// Degeneracy: unresolved referenced textures fail instead of silently
    /// degrading to a fallback texture.
    [[nodiscard]] inline PBRMaterial MakePBRMaterial(
        const kairo::assets::MaterialArtifactData& source,
        const MaterialTextureResolver& resolveTexture = {})
    {
        kairo::assets::ValidateMaterialArtifactData(source);
        const auto resolve = [&](const std::optional<kairo::assets::TextureAssetHandle>& texture)
        {
            if (!texture.has_value()) return InvalidTextureHandle;
            if (!resolveTexture)
                throw std::invalid_argument("A textured material requires a texture resolver.");
            const TextureHandle handle = resolveTexture(*texture);
            if (handle == InvalidTextureHandle)
                throw std::invalid_argument("A material texture resolved to an invalid renderer handle.");
            return handle;
        };

        PBRMaterial result;
        result.BaseColor = { source.BaseColorFactor[0], source.BaseColorFactor[1],
            source.BaseColorFactor[2] };
        result.BaseColorAlpha = source.BaseColorFactor[3];
        result.Metallic = source.MetallicFactor;
        result.Roughness = std::max(source.RoughnessFactor, 0.04f);
        result.Emissive = { source.EmissiveFactor[0], source.EmissiveFactor[1],
            source.EmissiveFactor[2] };
        result.NormalScale = source.NormalScale;
        result.AmbientOcclusion = source.OcclusionStrength;
        result.AlphaCutoff = source.AlphaCutoff;
        result.DoubleSided = source.DoubleSided;
        switch (source.AlphaMode)
        {
            case kairo::assets::MaterialAlphaMode::Opaque:
                result.AlphaMode = MaterialAlphaMode::Opaque;
                break;
            case kairo::assets::MaterialAlphaMode::Mask:
                result.AlphaMode = MaterialAlphaMode::Mask;
                break;
            case kairo::assets::MaterialAlphaMode::Blend:
                result.AlphaMode = MaterialAlphaMode::Blend;
                break;
        }
        result.BaseColorTexture = resolve(source.Textures.BaseColor);
        result.NormalTexture = resolve(source.Textures.Normal);
        result.MetallicRoughnessTexture = resolve(source.Textures.MetallicRoughness);
        result.EmissiveTexture = resolve(source.Textures.Emissive);
        result.OcclusionTexture = resolve(source.Textures.Occlusion);
        result.Validate();
        return result;
    }

    /// Resolves one external glTF image URI using the semantic required by its
    /// material channel. Empty URIs represent an absent texture in the current
    /// artifact format. Hosts own import-cache and GPU lifetime policy.
    using GltfTextureResolver = std::function<TextureHandle(
        std::string_view, kairo::assets::TextureSemantic)>;

    struct GltfRenderPrimitive final
    {
        Mesh Geometry;
        PBRMaterial Material;
        kairo::foundation::math::Mat4f LocalToAsset =
            kairo::foundation::math::Mat4f::Identity();
        std::uint32_t NodeIndex = 0u;
        std::uint32_t PrimitiveIndex = 0u;
    };

    struct GltfRenderAsset final
    {
        std::vector<GltfRenderPrimitive> Primitives;
    };

    /// Input: glTF's column-major matrix payload.
    /// Output: KairoMath's row/column-indexed matrix with identical transform.
    [[nodiscard]] inline kairo::foundation::math::Mat4f MakeGltfMatrix(
        const std::array<float, 16u>& source)
    {
        kairo::foundation::math::Mat4f result;
        for (std::size_t row = 0u; row < 4u; ++row)
            for (std::size_t column = 0u; column < 4u; ++column)
                result(row, column) = source[column * 4u + row];
        return result;
    }

    /// Converts one glTF metallic-roughness material without inventing asset
    /// identities. External URI resolution is delegated to the host; material
    /// factors remain usable when a channel is absent.
    [[nodiscard]] inline PBRMaterial MakeGltfPBRMaterial(
        const kairo::assets::GltfMaterialData& source,
        const GltfTextureResolver& resolveTexture = {})
    {
        const auto resolve = [&](const kairo::assets::GltfTextureBinding& binding,
            kairo::assets::TextureSemantic semantic)
        {
            if (binding.Uri.empty()) return InvalidTextureHandle;
            if (!resolveTexture)
                throw std::invalid_argument(
                    "A textured glTF material requires a texture resolver.");
            const TextureHandle result = resolveTexture(binding.Uri, semantic);
            if (result == InvalidTextureHandle)
                throw std::invalid_argument(
                    "A glTF material texture resolved to an invalid renderer handle.");
            return result;
        };

        PBRMaterial result;
        result.BaseColor = { source.BaseColorFactor[0], source.BaseColorFactor[1],
            source.BaseColorFactor[2] };
        result.BaseColorAlpha = source.BaseColorFactor[3];
        result.Metallic = source.MetallicFactor;
        result.Roughness = std::max(source.RoughnessFactor, 0.04f);
        result.Emissive = { source.EmissiveFactor[0], source.EmissiveFactor[1],
            source.EmissiveFactor[2] };
        result.NormalScale = source.NormalTexture.Scale;
        result.AmbientOcclusion = std::clamp(source.OcclusionTexture.Scale, 0.0f, 1.0f);
        result.AlphaCutoff = source.AlphaCutoff;
        result.DoubleSided = source.DoubleSided;
        switch (source.AlphaMode)
        {
            case kairo::assets::GltfAlphaMode::Opaque:
                result.AlphaMode = MaterialAlphaMode::Opaque; break;
            case kairo::assets::GltfAlphaMode::Mask:
                result.AlphaMode = MaterialAlphaMode::Mask; break;
            case kairo::assets::GltfAlphaMode::Blend:
                result.AlphaMode = MaterialAlphaMode::Blend; break;
        }
        result.BaseColorTexture = resolve(source.BaseColorTexture,
            kairo::assets::TextureSemantic::Color);
        result.MetallicRoughnessTexture = resolve(source.MetallicRoughnessTexture,
            kairo::assets::TextureSemantic::Data);
        result.NormalTexture = resolve(source.NormalTexture,
            kairo::assets::TextureSemantic::Normal);
        result.OcclusionTexture = resolve(source.OcclusionTexture,
            kairo::assets::TextureSemantic::Data);
        result.EmissiveTexture = resolve(source.EmissiveTexture,
            kairo::assets::TextureSemantic::Color);
        result.Validate();
        return result;
    }

    /// Input: one validated hierarchy-preserving glTF scene artifact.
    /// Output: renderable primitives with parent-composed local-to-asset
    /// transforms and their exact per-primitive material assignment.
    /// Task: provide one deterministic conversion shared by Editor and Player.
    /// Degeneracy: missing normals and unresolved external textures fail with a
    /// diagnostic; a primitive without a material receives neutral PBR values.
    [[nodiscard]] inline GltfRenderAsset MakeGltfRenderAsset(
        const kairo::assets::GltfSceneArtifactData& source,
        const GltfTextureResolver& resolveTexture = {})
    {
        kairo::assets::ValidateGltfSceneArtifactData(source);
        std::vector<kairo::foundation::math::Mat4f> world(
            source.Nodes.size(), kairo::foundation::math::Mat4f::Identity());
        std::vector<std::uint8_t> state(source.Nodes.size(), 0u);
        std::function<void(std::size_t)> resolveNode = [&](std::size_t index)
        {
            if (state[index] == 2u) return;
            if (state[index] == 1u)
                throw std::invalid_argument("glTF node hierarchy contains a cycle.");
            state[index] = 1u;
            const auto local = MakeGltfMatrix(source.Nodes[index].LocalTransform);
            if (source.Nodes[index].Parent >= 0)
            {
                const auto parent = static_cast<std::size_t>(source.Nodes[index].Parent);
                resolveNode(parent);
                world[index] = world[parent] * local;
            }
            else world[index] = local;
            state[index] = 2u;
        };
        for (std::size_t index = 0u; index < source.Nodes.size(); ++index)
            resolveNode(index);

        GltfRenderAsset result;
        for (std::size_t nodeIndex = 0u; nodeIndex < source.Nodes.size(); ++nodeIndex)
        {
            for (const std::uint32_t primitiveIndex :
                source.Nodes[nodeIndex].PrimitiveIndices)
            {
                const auto& primitive = source.Primitives[primitiveIndex];
                PBRMaterial material;
                if (primitive.MaterialIndex != std::numeric_limits<std::uint32_t>::max())
                    material = MakeGltfPBRMaterial(
                        source.Materials[primitive.MaterialIndex], resolveTexture);
                result.Primitives.push_back({ Mesh::FromArtifact(primitive.Mesh),
                    material, world[nodeIndex], static_cast<std::uint32_t>(nodeIndex),
                    primitiveIndex });
            }
        }
        if (result.Primitives.empty())
            throw std::invalid_argument("glTF scene contains no instantiated primitives.");
        return result;
    }
}
