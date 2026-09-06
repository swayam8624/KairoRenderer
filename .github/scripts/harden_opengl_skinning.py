from pathlib import Path

shader = Path('OpenGLShaders.cppm')
text = shader.read_text()
old = '''mat4 SkinMatrix()\n{\n    return inWeights.x * uJoints[inJoints.x] +\n           inWeights.y * uJoints[inJoints.y] +\n           inWeights.z * uJoints[inJoints.z] +\n           inWeights.w * uJoints[inJoints.w];\n}'''
new = '''mat4 SkinMatrix()\n{\n    mat4 skin = mat4(0.0);\n    // Zero-weight slots are semantically inactive and may contain arbitrary\n    // JOINTS_0 values. Never dereference them: out-of-range uniform-array\n    // indexing is undefined even when the mathematical multiplier is zero.\n    if (inWeights.x > 0.0) skin += inWeights.x * uJoints[inJoints.x];\n    if (inWeights.y > 0.0) skin += inWeights.y * uJoints[inJoints.y];\n    if (inWeights.z > 0.0) skin += inWeights.z * uJoints[inJoints.z];\n    if (inWeights.w > 0.0) skin += inWeights.w * uJoints[inJoints.w];\n    return skin;\n}'''
count = text.count(old)
if count != 2:
    raise SystemExit(f'expected two SkinMatrix bodies, found {count}')
shader.write_text(text.replace(old, new))

smoke = Path('tests/OpenGLSkinningSmoke.cpp')
text = smoke.read_text()
old = '''            influence.Joints = { 0u, 0u, 0u, 0u };\n            influence.Weights = { 1.0f, 0.0f, 0.0f, 0.0f };'''
new = '''            // glTF permits irrelevant joint values in zero-weight slots. This\n            // deliberately uses indices beyond the palette limit so the native\n            // shader smoke test catches accidental unconditional dereferences.\n            influence.Joints = { 0u, 999999u, 70000u, 255u };\n            influence.Weights = { 1.0f, 0.0f, 0.0f, 0.0f };'''
if old not in text:
    raise SystemExit('smoke influence marker not found')
smoke.write_text(text.replace(old, new, 1))

Path('.github/workflows/harden-opengl-skinning.yml').unlink(missing_ok=True)
Path('.github/scripts/harden_opengl_skinning.py').unlink(missing_ok=True)
