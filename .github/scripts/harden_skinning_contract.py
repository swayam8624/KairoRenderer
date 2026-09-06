from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f"{label} marker not found")
    return text.replace(old, new, 1)

# Preserve the aggregate-initialization ABI of MeshDraw by appending skinning.
path = Path("RenderScene.cppm")
text = path.read_text()
old = '''        PBRMaterial Material{};
        /// Empty for a static draw. For a skinned draw these matrices are
        /// already in imported-asset space (jointWorld * inverseBind).
        SkinPalette Skinning{};
        /// Zero means non-pickable. Editor scene extraction supplies stable
        /// scene entity IDs; runtime-only draws may deliberately leave it zero.
        std::uint32_t ObjectID = 0u;
        bool CastShadows = true;
        bool ReceiveShadows = true;
'''
new = '''        PBRMaterial Material{};
        /// Zero means non-pickable. Editor scene extraction supplies stable
        /// scene entity IDs; runtime-only draws may deliberately leave it zero.
        std::uint32_t ObjectID = 0u;
        bool CastShadows = true;
        bool ReceiveShadows = true;
        /// Empty for a static draw. For a skinned draw these matrices are
        /// already in imported-asset space (jointWorld * inverseBind).
        SkinPalette Skinning{};
'''
text = replace_once(text, old, new, "MeshDraw skinning placement")
path.write_text(text)

# Until the native GPU implementations land, every public backend must fail
# explicitly for a skinned mesh instead of silently throwing away influences.
for filename in [
    "VulkanTriangle.cppm",
    "OpenGLRenderer.cppm",
    "MetalRenderer.cppm",
    "Direct3D12Renderer.cppm",
]:
    path = Path(filename)
    if not path.exists():
        continue
    text = path.read_text()
    marker = '''        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
'''
    insertion = '''        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            if (mesh.IsSkinned())
                throw std::logic_error(
                    "Skinned mesh upload requires the native GPU skinning path.");
'''
    text = replace_once(text, marker, insertion, f"{filename} skinned upload guard")
    path.write_text(text)

Path(".github/workflows/harden-skinning-contract.yml").unlink(missing_ok=True)
Path(".github/scripts/harden_skinning_contract.py").unlink(missing_ok=True)
