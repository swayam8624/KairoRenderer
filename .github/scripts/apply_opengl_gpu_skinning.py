from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if new in text:
        return text
    if old not in text:
        raise SystemExit(f"{label} marker not found")
    return text.replace(old, new, 1)


def replace_block(text: str, start: str, end: str, new: str, label: str) -> str:
    begin = text.find(start)
    if begin < 0:
        raise SystemExit(f"{label} start not found")
    finish = text.find(end, begin)
    if finish < 0:
        raise SystemExit(f"{label} end not found")
    return text[:begin] + new + text[finish:]

# ---------------------------------------------------------------------------
# GLSL 4.1 skinned forward + shadow vertex programs.
# ---------------------------------------------------------------------------
path = Path("OpenGLShaders.cppm")
text = path.read_text()
mesh_marker = "    /// Metallic-roughness forward shading mirrors the Vulkan renderer's\n"
skinned_mesh = r'''    /// Four-weight GPU skinning variant. The palette is supplied through a
    /// std140 uniform block rather than ordinary vertex uniforms so the full
    /// portable 255-joint ceiling fits OpenGL 4.1's guaranteed 16 KiB block.
    inline constexpr std::string_view SkinnedMeshVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inColor;
layout(location=2) in vec3 inNormal;
layout(location=3) in vec2 inTexCoord;
layout(location=4) in uvec4 inJoints;
layout(location=5) in vec4 inWeights;

const int MaximumSkinJoints = 255;
layout(std140) uniform SkinPaletteBlock { mat4 uJoints[MaximumSkinJoints]; };

uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform mat4 uLightViewProjection;

out vec3 vertexColor;
out vec3 worldNormal;
out vec3 worldPosition;
out vec4 lightClipPosition;
out vec2 texCoord;

mat4 SkinMatrix()
{
    return inWeights.x * uJoints[inJoints.x] +
           inWeights.y * uJoints[inJoints.y] +
           inWeights.z * uJoints[inJoints.z] +
           inWeights.w * uJoints[inJoints.w];
}

void main()
{
    mat4 skin = SkinMatrix();
    vec4 assetPosition = skin * vec4(inPosition, 1.0);
    vec4 world = uModel * assetPosition;
    gl_Position = uProjection * uView * world;
    vertexColor = inColor;
    // glTF joint transforms are normally rigid/TRS. Using inverse-transpose of
    // the blended linear transform keeps normals correct under authored scale.
    vec3 assetNormal = transpose(inverse(mat3(skin))) * inNormal;
    worldNormal = uNormalMatrix * assetNormal;
    worldPosition = world.xyz;
    lightClipPosition = uLightViewProjection * world;
    texCoord = inTexCoord;
}
)GLSL";

'''
if skinned_mesh not in text:
    if mesh_marker not in text:
        raise SystemExit("OpenGL skinned mesh shader insertion marker not found")
    text = text.replace(mesh_marker, skinned_mesh + mesh_marker, 1)

shadow_fragment_marker = "    inline constexpr std::string_view ShadowFragment = R\"GLSL(#version 410 core\n"
skinned_shadow = r'''    inline constexpr std::string_view SkinnedShadowVertex = R"GLSL(#version 410 core
layout(location=0) in vec3 inPosition;
layout(location=4) in uvec4 inJoints;
layout(location=5) in vec4 inWeights;
const int MaximumSkinJoints = 255;
layout(std140) uniform SkinPaletteBlock { mat4 uJoints[MaximumSkinJoints]; };
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
mat4 SkinMatrix()
{
    return inWeights.x * uJoints[inJoints.x] +
           inWeights.y * uJoints[inJoints.y] +
           inWeights.z * uJoints[inJoints.z] +
           inWeights.w * uJoints[inJoints.w];
}
void main()
{
    gl_Position = uLightViewProjection * uModel *
        SkinMatrix() * vec4(inPosition, 1.0);
}
)GLSL";

'''
if skinned_shadow not in text:
    if shadow_fragment_marker not in text:
        raise SystemExit("OpenGL skinned shadow shader insertion marker not found")
    text = text.replace(shadow_fragment_marker, skinned_shadow + shadow_fragment_marker, 1)
path.write_text(text)

# ---------------------------------------------------------------------------
# OpenGL runtime native skin stream + UBO palette path.
# ---------------------------------------------------------------------------
path = Path("OpenGLRenderer.cppm")
text = path.read_text()
text = replace_once(text,
    "import Kairo.Renderer.Mesh;\n",
    "import Kairo.Renderer.Mesh;\nimport Kairo.Renderer.Skinning;\n",
    "OpenGL Skinning import")

text = replace_once(text,
'''        struct GpuMesh final
        {
            GLuint VertexArray = 0u;
            GLuint VertexBuffer = 0u;
            GLuint IndexBuffer = 0u;
            GLsizei IndexCount = 0;
        };
''',
'''        struct GpuMesh final
        {
            GLuint VertexArray = 0u;
            GLuint VertexBuffer = 0u;
            GLuint SkinBuffer = 0u;
            GLuint IndexBuffer = 0u;
            GLsizei IndexCount = 0;
            std::size_t RequiredJoints = 0u;
        };
''', "OpenGL GpuMesh")

text = replace_once(text,
'''        GLuint m_MeshProgram = 0u;
        GLuint m_ShadowProgram = 0u;
        GLuint m_DebugProgram = 0u;
''',
'''        GLuint m_MeshProgram = 0u;
        GLuint m_SkinnedMeshProgram = 0u;
        GLuint m_ShadowProgram = 0u;
        GLuint m_SkinnedShadowProgram = 0u;
        GLuint m_DebugProgram = 0u;
        GLuint m_SkinPaletteBuffer = 0u;
''', "OpenGL program members")
text = replace_once(text,
    "        static constexpr std::uint32_t ShadowResolution = 2048u;\n",
    "        static constexpr std::uint32_t ShadowResolution = 2048u;\n"
    "        static constexpr GLuint SkinPaletteBindingPoint = 7u;\n",
    "OpenGL skin binding point")

text = replace_once(text,
'''                m_MeshProgram = CreateProgram(
                    opengl_shaders::MeshVertex, opengl_shaders::MeshFragment,
                    "OpenGL mesh program");
                m_ShadowProgram = CreateProgram(
                    opengl_shaders::ShadowVertex, opengl_shaders::ShadowFragment,
                    "OpenGL shadow program");
                m_DebugProgram = CreateProgram(
''',
'''                m_MeshProgram = CreateProgram(
                    opengl_shaders::MeshVertex, opengl_shaders::MeshFragment,
                    "OpenGL mesh program");
                m_SkinnedMeshProgram = CreateProgram(
                    opengl_shaders::SkinnedMeshVertex,
                    opengl_shaders::MeshFragment,
                    "OpenGL skinned mesh program");
                m_ShadowProgram = CreateProgram(
                    opengl_shaders::ShadowVertex, opengl_shaders::ShadowFragment,
                    "OpenGL shadow program");
                m_SkinnedShadowProgram = CreateProgram(
                    opengl_shaders::SkinnedShadowVertex,
                    opengl_shaders::ShadowFragment,
                    "OpenGL skinned shadow program");
                m_DebugProgram = CreateProgram(
''', "OpenGL skinned program creation")
text = replace_once(text,
'''                m_FallbackBlack = CreateSolidTexture(0u, 0u, 0u, 255u);
                ConfigureSamplerUniforms();
''',
'''                m_FallbackBlack = CreateSolidTexture(0u, 0u, 0u, 255u);
                CreateSkinningResources();
                ConfigureSamplerUniforms();
''', "OpenGL skin resource initialization")

create_mesh = '''        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            if (mesh.Indices().size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
                throw std::length_error("OpenGL mesh index count exceeds GLsizei capacity.");
            if (mesh.RequiredJointCount() > MaximumSkinJoints)
                throw std::length_error("OpenGL skinned mesh exceeds the portable joint limit.");
            glfwMakeContextCurrent(m_Window.NativeHandle());
            GpuMesh gpu;
            glGenVertexArrays(1, &gpu.VertexArray);
            glGenBuffers(1, &gpu.VertexBuffer);
            glGenBuffers(1, &gpu.IndexBuffer);
            if (mesh.IsSkinned()) glGenBuffers(1, &gpu.SkinBuffer);
            try
            {
                if (gpu.VertexArray == 0u || gpu.VertexBuffer == 0u ||
                    gpu.IndexBuffer == 0u || (mesh.IsSkinned() && gpu.SkinBuffer == 0u))
                    throw std::runtime_error("OpenGL failed to allocate mesh objects.");
                glBindVertexArray(gpu.VertexArray);
                glBindBuffer(GL_ARRAY_BUFFER, gpu.VertexBuffer);
                glBufferData(GL_ARRAY_BUFFER, CheckedGLSize(mesh.VertexBytes()),
                    mesh.Vertices().data(), GL_STATIC_DRAW);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.IndexBuffer);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, CheckedGLSize(mesh.IndexBytes()),
                    mesh.Indices().data(), GL_STATIC_DRAW);
                constexpr GLsizei stride = static_cast<GLsizei>(sizeof(MeshVertex));
                ConfigureVertexAttribute(0u, 3, stride, offsetof(MeshVertex, Position));
                ConfigureVertexAttribute(1u, 3, stride, offsetof(MeshVertex, Color));
                ConfigureVertexAttribute(2u, 3, stride, offsetof(MeshVertex, Normal));
                ConfigureVertexAttribute(3u, 2, stride, offsetof(MeshVertex, TexCoord));
                if (mesh.IsSkinned())
                {
                    glBindBuffer(GL_ARRAY_BUFFER, gpu.SkinBuffer);
                    glBufferData(GL_ARRAY_BUFFER, CheckedGLSize(mesh.SkinBytes()),
                        mesh.Skinning().data(), GL_STATIC_DRAW);
                    constexpr GLsizei skinStride =
                        static_cast<GLsizei>(sizeof(SkinVertexInfluence));
                    ConfigureIntegerVertexAttribute(4u, 4, GL_UNSIGNED_INT,
                        skinStride, offsetof(SkinVertexInfluence, Joints));
                    ConfigureVertexAttribute(5u, 4, skinStride,
                        offsetof(SkinVertexInfluence, Weights));
                    gpu.RequiredJoints = mesh.RequiredJointCount();
                }
                glBindVertexArray(0u);
                gpu.IndexCount = static_cast<GLsizei>(mesh.Indices().size());
                ThrowIfGLError("uploading a mesh");
            }
            catch (...)
            {
                DeleteMesh(gpu);
                throw;
            }
            const MeshHandle handle = NextHandle(m_NextMesh, m_Meshes);
            m_Meshes.emplace(handle, gpu);
            return handle;
        }

'''
text = replace_block(text,
    "        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)\n        {\n",
    "        void DestroyMesh(MeshHandle mesh)\n",
    create_mesh, "OpenGL CreateMesh")

text = replace_once(text,
'''                if (!m_Meshes.contains(draw.Mesh))
                    throw std::invalid_argument("RenderScene references an OpenGL mesh owned by another runtime.");
                ValidateTextureHandle(draw.Material.BaseColorTexture);
''',
'''                if (!m_Meshes.contains(draw.Mesh))
                    throw std::invalid_argument("RenderScene references an OpenGL mesh owned by another runtime.");
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                if (mesh.RequiredJoints > 0u)
                {
                    if (draw.Skinning.Size() < mesh.RequiredJoints)
                        throw std::invalid_argument(
                            "OpenGL skinned draw palette does not cover every referenced joint.");
                }
                else if (!draw.Skinning.Empty())
                    throw std::invalid_argument(
                        "OpenGL static mesh draw cannot carry a skin palette.");
                ValidateTextureHandle(draw.Material.BaseColorTexture);
''', "OpenGL scene skin validation")

configure_samplers = '''        void ConfigureSamplerUniforms() const
        {
            ConfigureProgramSamplers(m_MeshProgram);
            ConfigureProgramSamplers(m_SkinnedMeshProgram);
            glUseProgram(0u);
        }

        static void ConfigureProgramSamplers(GLuint program)
        {
            glUseProgram(program);
            SetUniform(program, "uShadowMap", 0);
            SetUniform(program, "uBaseColorMap", 1);
            SetUniform(program, "uNormalMap", 2);
            SetUniform(program, "uMetallicRoughnessMap", 3);
            SetUniform(program, "uEmissiveMap", 4);
            SetUniform(program, "uOcclusionMap", 5);
            SetUniform(program, "uEnvironmentMap", 6);
        }

        void CreateSkinningResources()
        {
            GLint maximumBlockBytes = 0;
            glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maximumBlockBytes);
            constexpr std::size_t requiredBytes = MaximumSkinJoints * 16u * sizeof(float);
            if (maximumBlockBytes < static_cast<GLint>(requiredBytes))
                throw PresentationUnavailableError(
                    "OpenGL implementation does not provide the required 16 KiB skin uniform block.");
            glGenBuffers(1, &m_SkinPaletteBuffer);
            if (m_SkinPaletteBuffer == 0u)
                throw std::runtime_error("OpenGL failed to allocate the skin palette buffer.");
            glBindBuffer(GL_UNIFORM_BUFFER, m_SkinPaletteBuffer);
            glBufferData(GL_UNIFORM_BUFFER, CheckedGLSize(requiredBytes), nullptr,
                GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, SkinPaletteBindingPoint,
                m_SkinPaletteBuffer);
            const auto bindBlock = [](GLuint program)
            {
                const GLuint block = glGetUniformBlockIndex(program, "SkinPaletteBlock");
                if (block == GL_INVALID_INDEX)
                    throw std::runtime_error(
                        "OpenGL skinned shader does not expose SkinPaletteBlock.");
                glUniformBlockBinding(program, block, SkinPaletteBindingPoint);
            };
            bindBlock(m_SkinnedMeshProgram);
            bindBlock(m_SkinnedShadowProgram);
            glBindBuffer(GL_UNIFORM_BUFFER, 0u);
        }

'''
text = replace_block(text,
    "        void ConfigureSamplerUniforms() const\n        {\n",
    "        void CreateViewportTarget(std::uint32_t width, std::uint32_t height)\n",
    configure_samplers, "OpenGL sampler/skin resources")

shadow = '''        void DrawShadowPass(const kairo::foundation::math::Mat4f& lightViewProjection)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowFramebuffer);
            glViewport(0, 0, ShadowResolution, ShadowResolution);
            glClear(GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(m_ShadowSettings.SlopeDepthBias,
                m_ShadowSettings.ConstantDepthBias);
            for (const MeshDraw& draw : m_Draws)
            {
                if (!draw.CastShadows ||
                    draw.Material.AlphaMode == MaterialAlphaMode::Blend) continue;
                const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
                const GLuint program = mesh.RequiredJoints > 0u
                    ? m_SkinnedShadowProgram : m_ShadowProgram;
                glUseProgram(program);
                SetMatrix(program, "uLightViewProjection", lightViewProjection);
                SetMatrix(program, "uModel", draw.Model);
                if (mesh.RequiredJoints > 0u) UploadSkinPalette(draw.Skinning);
                DrawMesh(draw.Mesh);
            }
            glDisable(GL_POLYGON_OFFSET_FILL);
            glBindFramebuffer(GL_FRAMEBUFFER, 0u);
        }

'''
text = replace_block(text,
    "        void DrawShadowPass(const kairo::foundation::math::Mat4f& lightViewProjection)\n        {\n",
    "        void BeginViewportMeshPass(\n",
    shadow, "OpenGL shadow pass")

begin_pass = '''        void BeginViewportMeshPass(
            const kairo::foundation::math::Mat4f& lightViewProjection,
            std::optional<std::size_t> shadowIndex) const
        {
            static_cast<void>(lightViewProjection);
            static_cast<void>(shadowIndex);
            glBindFramebuffer(GL_FRAMEBUFFER, m_ViewportFramebuffer);
            glViewport(0, 0, CheckedGLDimension(m_ViewportWidth),
                CheckedGLDimension(m_ViewportHeight));
            constexpr GLenum attachments[]{ GL_COLOR_ATTACHMENT0,
                GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, attachments);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_ShadowDepth);
        }

'''
text = replace_block(text,
    "        void BeginViewportMeshPass(\n",
    "        void DrawOpaquePass(\n",
    begin_pass, "OpenGL begin viewport pass")

text = text.replace(
    "                    DrawOne(m_Draws[index]);\n",
    "                    DrawOne(m_Draws[index], lightViewProjection, shadowIndex);\n")
text = text.replace(
    "                for (const std::size_t index : transparent) DrawOne(m_Draws[index]);\n",
    "                for (const std::size_t index : transparent)\n"
    "                    DrawOne(m_Draws[index], lightViewProjection, shadowIndex);\n")

upload_frame_start = "        void UploadFrameUniforms(\n"
draw_one_marker = "        void DrawOne(const MeshDraw& draw) const\n"
start = text.find(upload_frame_start)
end = text.find(draw_one_marker, start)
if start < 0 or end < 0:
    raise SystemExit("OpenGL frame uniform block markers not found")
frame_block = text[start:end]
frame_block = frame_block.replace(
'''        void UploadFrameUniforms(
            const kairo::foundation::math::Mat4f& lightViewProjection,
            std::optional<std::size_t> shadowIndex) const
''',
'''        void UploadFrameUniforms(GLuint program,
            const kairo::foundation::math::Mat4f& lightViewProjection,
            std::optional<std::size_t> shadowIndex) const
''', 1)
frame_block = frame_block.replace("m_MeshProgram", "program")
text = text[:start] + frame_block + text[end:]

draw_one = '''        void DrawOne(const MeshDraw& draw,
            const kairo::foundation::math::Mat4f& lightViewProjection,
            std::optional<std::size_t> shadowIndex) const
        {
            const GpuMesh& mesh = m_Meshes.at(draw.Mesh);
            const GLuint program = mesh.RequiredJoints > 0u
                ? m_SkinnedMeshProgram : m_MeshProgram;
            glUseProgram(program);
            UploadFrameUniforms(program, lightViewProjection, shadowIndex);
            if (mesh.RequiredJoints > 0u) UploadSkinPalette(draw.Skinning);
            const auto normal = ComputeNormalMatrix(draw.Model);
            SetMatrix(program, "uModel", draw.Model);
            SetMatrix(program, "uNormalMatrix", normal);
            SetUniform4(program, "uBaseColorFactor",
                draw.Material.BaseColor.x, draw.Material.BaseColor.y,
                draw.Material.BaseColor.z, draw.Material.BaseColorAlpha);
            SetUniform(program, "uEmissiveFactor", draw.Material.Emissive);
            SetUniform4(program, "uMaterialFactors",
                draw.Material.Metallic, draw.Material.Roughness,
                draw.Material.AmbientOcclusion, draw.Material.AlphaCutoff);
            SetUniform(program, "uNormalScale", draw.Material.NormalScale);
            SetUniform(program, "uAlphaMode",
                static_cast<int>(draw.Material.AlphaMode));
            SetUniformUnsigned(program, "uObjectID", draw.ObjectID);
            SetUniform(program, "uReceiveShadows", draw.ReceiveShadows ? 1 : 0);
            BindTextureUnit(1u, TextureOr(draw.Material.BaseColorTexture,
                m_FallbackWhite));
            BindTextureUnit(2u, TextureOr(draw.Material.NormalTexture,
                m_FallbackNormal));
            BindTextureUnit(3u, TextureOr(
                draw.Material.MetallicRoughnessTexture, m_FallbackWhite));
            BindTextureUnit(4u, TextureOr(draw.Material.EmissiveTexture,
                m_FallbackWhite));
            BindTextureUnit(5u, TextureOr(draw.Material.OcclusionTexture,
                m_FallbackWhite));
            const GLuint environment = TextureOr(
                m_Environment.EnvironmentTexture, m_FallbackBlack);
            BindTextureUnit(6u, environment);
            const float maxLod = m_Environment.EnvironmentTexture == InvalidTextureHandle
                ? 0.0f
                : static_cast<float>(m_Textures.at(
                    m_Environment.EnvironmentTexture).MipLevels - 1u);
            SetUniform(program, "uEnvironmentMaxLod", maxLod);
            if (draw.Material.DoubleSided) glDisable(GL_CULL_FACE);
            else glEnable(GL_CULL_FACE);
            DrawMesh(draw.Mesh);
        }

        void UploadSkinPalette(const SkinPalette& palette) const
        {
            palette.Validate();
            if (palette.Empty())
                throw std::invalid_argument(
                    "OpenGL skinned draw requires a non-empty skin palette.");
            std::array<float, MaximumSkinJoints * 16u> packed{};
            for (std::size_t joint = 0u; joint < palette.Size(); ++joint)
                for (std::size_t row = 0u; row < 4u; ++row)
                    for (std::size_t column = 0u; column < 4u; ++column)
                        packed[joint * 16u + column * 4u + row] =
                            palette.JointMatrices[joint](row, column);
            const std::size_t bytes = palette.Size() * 16u * sizeof(float);
            glBindBuffer(GL_UNIFORM_BUFFER, m_SkinPaletteBuffer);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, CheckedGLSize(bytes), packed.data());
            glBindBufferBase(GL_UNIFORM_BUFFER, SkinPaletteBindingPoint,
                m_SkinPaletteBuffer);
        }

'''
text = replace_block(text,
    draw_one_marker,
    "        void DrawDebugLines()\n",
    draw_one, "OpenGL DrawOne")

text = replace_once(text,
'''        static void ConfigureVertexAttribute(GLuint index, GLint components,
            GLsizei stride, std::size_t offset)
        {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(index, components, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offset));
        }
''',
'''        static void ConfigureVertexAttribute(GLuint index, GLint components,
            GLsizei stride, std::size_t offset)
        {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(index, components, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offset));
        }

        static void ConfigureIntegerVertexAttribute(GLuint index, GLint components,
            GLenum type, GLsizei stride, std::size_t offset)
        {
            glEnableVertexAttribArray(index);
            glVertexAttribIPointer(index, components, type, stride,
                reinterpret_cast<const void*>(offset));
        }
''', "OpenGL integer skin attribute helper")

text = replace_once(text,
'''        static void DeleteMesh(const GpuMesh& mesh) noexcept
        {
            if (mesh.IndexBuffer != 0u) glDeleteBuffers(1, &mesh.IndexBuffer);
            if (mesh.VertexBuffer != 0u) glDeleteBuffers(1, &mesh.VertexBuffer);
            if (mesh.VertexArray != 0u) glDeleteVertexArrays(1, &mesh.VertexArray);
        }
''',
'''        static void DeleteMesh(const GpuMesh& mesh) noexcept
        {
            if (mesh.IndexBuffer != 0u) glDeleteBuffers(1, &mesh.IndexBuffer);
            if (mesh.SkinBuffer != 0u) glDeleteBuffers(1, &mesh.SkinBuffer);
            if (mesh.VertexBuffer != 0u) glDeleteBuffers(1, &mesh.VertexBuffer);
            if (mesh.VertexArray != 0u) glDeleteVertexArrays(1, &mesh.VertexArray);
        }
''', "OpenGL skin buffer destruction")

text = replace_once(text,
'''            if (m_DebugProgram != 0u) glDeleteProgram(m_DebugProgram);
            if (m_ShadowProgram != 0u) glDeleteProgram(m_ShadowProgram);
            if (m_MeshProgram != 0u) glDeleteProgram(m_MeshProgram);
''',
'''            if (m_SkinPaletteBuffer != 0u) glDeleteBuffers(1, &m_SkinPaletteBuffer);
            if (m_DebugProgram != 0u) glDeleteProgram(m_DebugProgram);
            if (m_SkinnedShadowProgram != 0u) glDeleteProgram(m_SkinnedShadowProgram);
            if (m_ShadowProgram != 0u) glDeleteProgram(m_ShadowProgram);
            if (m_SkinnedMeshProgram != 0u) glDeleteProgram(m_SkinnedMeshProgram);
            if (m_MeshProgram != 0u) glDeleteProgram(m_MeshProgram);
''', "OpenGL skin program destruction")
text = replace_once(text,
'''            m_DebugProgram = m_ShadowProgram = m_MeshProgram = 0u;
''',
'''            m_SkinPaletteBuffer = 0u;
            m_DebugProgram = m_SkinnedShadowProgram = m_ShadowProgram =
                m_SkinnedMeshProgram = m_MeshProgram = 0u;
''', "OpenGL skin zeroing")
path.write_text(text)

# ---------------------------------------------------------------------------
# Contract regression for the portable 255-joint ceiling.
# ---------------------------------------------------------------------------
path = Path("tests/SkinningContractTests.cpp")
text = path.read_text()
append = r'''
TEST_CASE("portable GPU skin palette rejects the 256th joint")
{
    SkinPalette palette;
    palette.JointMatrices.resize(MaximumSkinJoints,
        kairo::foundation::math::Mat4f::Identity());
    REQUIRE_NOTHROW(palette.Validate());
    palette.JointMatrices.push_back(kairo::foundation::math::Mat4f::Identity());
    REQUIRE_THROWS_AS(palette.Validate(), std::length_error);

    SkinVertexInfluence invalid{ { static_cast<std::uint32_t>(MaximumSkinJoints),
        0u, 0u, 0u }, { 1.0f, 0.0f, 0.0f, 0.0f } };
    REQUIRE_THROWS_AS(ValidateSkinVertexInfluence(invalid), std::out_of_range);
}
'''
if append not in text:
    text += append
path.write_text(text)

# ---------------------------------------------------------------------------
# Linux/Xvfb native smoke: shader compile, second stream, palette UBO, shadow.
# ---------------------------------------------------------------------------
Path("tests/OpenGLSkinningSmoke.cpp").write_text(r'''#include <exception>
#include <iostream>

import Kairo.Renderer;
import Kairo.Foundation.Math;
import Kairo.Assets;

int main()
{
    try
    {
        using namespace kairo::renderer;
        kairo::assets::GltfPrimitiveData primitive;
        primitive.Mesh.HasNormals = true;
        primitive.Mesh.Vertices = {
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} },
            { {  0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} },
            { {  0.0f,  0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {} }
        };
        primitive.Mesh.Indices = { 0u, 1u, 2u };
        primitive.Skinning.resize(3u);
        for (auto& influence : primitive.Skinning)
        {
            influence.Joints = { 0u, 0u, 0u, 0u };
            influence.Weights = { 1.0f, 0.0f, 0.0f, 0.0f };
        }

        OpenGLRendererRuntime runtime({ "OpenGL skinning smoke", 96u, 96u, false });
        const MeshHandle mesh = runtime.CreateMesh(Mesh::FromGltfPrimitive(primitive));
        RenderScene scene;
        MeshDraw draw;
        draw.Mesh = mesh;
        draw.Skinning.JointMatrices.push_back(
            kairo::foundation::math::MakeTranslation(
                kairo::foundation::math::Vec3f{ 0.1f, 0.0f, 0.0f }));
        scene.Add(draw);
        RenderLight light;
        light.Type = RenderLightType::Directional;
        light.Direction = { 0.25f, 1.0f, 0.35f };
        light.CastShadows = true;
        scene.AddLight(light);
        runtime.SetViewportShadingMode(ViewportShadingMode::Unlit);
        runtime.SubmitRenderScene(scene);
        runtime.DrawFrame();
        return 0;
    }
    catch (const PresentationUnavailableError& error)
    {
        std::cerr << "OpenGL presentation unavailable: " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error)
    {
        std::cerr << "OpenGL skinning smoke failed: " << error.what() << '\n';
        return 1;
    }
}
''')

path = Path("CMakeLists.txt")
text = path.read_text()
marker = '''    if(TARGET KairoRendererPhysicsDebug)
        add_executable(KairoRendererPhysicsDebugTests tests/PhysicsDebugBridgeTests.cpp)
'''
insert = '''    if(UNIX AND NOT APPLE)
        add_executable(KairoRendererOpenGLSkinningSmoke tests/OpenGLSkinningSmoke.cpp)
        target_link_libraries(KairoRendererOpenGLSkinningSmoke PRIVATE KairoRenderer)
        add_test(NAME KairoRendererOpenGLSkinningSmoke
            COMMAND KairoRendererOpenGLSkinningSmoke)
        set_tests_properties(KairoRendererOpenGLSkinningSmoke PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
'''
if insert not in text:
    if marker not in text:
        raise SystemExit("OpenGL smoke CMake insertion marker not found")
    text = text.replace(marker, insert + marker, 1)
path.write_text(text)

Path(".github/workflows/apply-opengl-gpu-skinning.yml").unlink(missing_ok=True)
Path(".github/scripts/apply_opengl_gpu_skinning.py").unlink(missing_ok=True)
