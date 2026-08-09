module;

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module Kairo.Renderer.OpenGLRuntime;

import Kairo.Foundation.Math;
import Kairo.Renderer.Types;
import Kairo.Renderer.GraphicsBackend;
import Kairo.Renderer.Camera;
import Kairo.Renderer.Window;
import Kairo.Renderer.Mesh;
import Kairo.Renderer.Texture;
import Kairo.Renderer.Material;
import Kairo.Renderer.RenderScene;
import Kairo.Renderer.ShadowSettings;
import Kairo.Renderer.DebugDraw;
import Kairo.Renderer.OpenGLShaders;
import Kairo.Assets.TextureArtifact;

export namespace kairo::renderer
{
    /// Owning OpenGL 4.1 implementation of the renderer-neutral runtime API.
    ///
    /// Input: a validated OpenGL WindowDesc.
    /// Output: an offscreen HDR viewport with object-ID/depth attachments,
    /// directional shadows, PBR material textures, debug lines, readback, and
    /// presentation to a GLFW window.
    /// Task: provide a real compatibility backend without exposing OpenGL
    /// names to EngineCore, Editor scene extraction, or gameplay code. A pinned
    /// GLAD 4.1-core loader gives macOS, Linux, and Windows the same entry-point
    /// and validation contract.
    class OpenGLRendererRuntime final
    {
        struct GpuMesh final
        {
            GLuint VertexArray = 0u;
            GLuint VertexBuffer = 0u;
            GLuint IndexBuffer = 0u;
            GLsizei IndexCount = 0;
        };

        struct GpuTexture final
        {
            GLuint Name = 0u;
            std::uint32_t MipLevels = 1u;
        };

        struct DebugVertex final
        {
            float Position[3]{};
            float Color[4]{};
        };

        GlfwRuntime m_Glfw;
        Window m_Window;
        GLuint m_MeshProgram = 0u;
        GLuint m_ShadowProgram = 0u;
        GLuint m_DebugProgram = 0u;
        GLuint m_ViewportFramebuffer = 0u;
        GLuint m_ViewportColor = 0u;
        GLuint m_ViewportObjectID = 0u;
        GLuint m_ViewportDepth = 0u;
        GLuint m_ShadowFramebuffer = 0u;
        GLuint m_ShadowDepth = 0u;
        GLuint m_DebugVertexArray = 0u;
        GLuint m_DebugVertexBuffer = 0u;
        GLuint m_FallbackWhite = 0u;
        GLuint m_FallbackNormal = 0u;
        GLuint m_FallbackBlack = 0u;
        std::uint32_t m_ViewportWidth = 1u;
        std::uint32_t m_ViewportHeight = 1u;
        static constexpr std::uint32_t ShadowResolution = 2048u;
        ShowcaseCamera m_Camera;
        DirectionalShadowSettings m_ShadowSettings;
        ViewportShadingMode m_ShadingMode = ViewportShadingMode::Lit;
        std::unordered_map<MeshHandle, GpuMesh> m_Meshes;
        std::unordered_map<TextureHandle, GpuTexture> m_Textures;
        std::vector<MeshDraw> m_Draws;
        std::vector<RenderLight> m_Lights;
        RenderEnvironment m_Environment;
        std::vector<DebugVertex> m_DebugVertices;
        MeshHandle m_NextMesh = 1u;
        TextureHandle m_NextTexture = 1u;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> m_PickRequest;
        std::optional<std::uint32_t> m_PickResult;
        bool m_CaptureRequested = false;
        std::optional<ViewportCapture> m_CaptureResult;
        OpenGLOverlayRecorder m_OverlayRecorder;
        std::uint64_t m_ViewportGeneration = 1u;

    public:
        explicit OpenGLRendererRuntime(const WindowDesc& windowDesc)
            : m_Glfw(), m_Window(windowDesc, GraphicsBackend::OpenGL),
              m_ViewportWidth(windowDesc.Width), m_ViewportHeight(windowDesc.Height)
        {
            glfwMakeContextCurrent(m_Window.NativeHandle());
            if (gladLoadGLLoader(
                reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0)
                throw PresentationUnavailableError(
                    "GLAD could not load the OpenGL 4.1 core entry points.");
            glfwSwapInterval(1);
            try
            {
                ValidateContext();
                m_MeshProgram = CreateProgram(
                    opengl_shaders::MeshVertex, opengl_shaders::MeshFragment,
                    "OpenGL mesh program");
                m_ShadowProgram = CreateProgram(
                    opengl_shaders::ShadowVertex, opengl_shaders::ShadowFragment,
                    "OpenGL shadow program");
                m_DebugProgram = CreateProgram(
                    opengl_shaders::DebugVertex, opengl_shaders::DebugFragment,
                    "OpenGL debug program");
                CreateViewportTarget(m_ViewportWidth, m_ViewportHeight);
                CreateShadowTarget();
                CreateDebugResources();
                m_FallbackWhite = CreateSolidTexture(255u, 255u, 255u, 255u);
                m_FallbackNormal = CreateSolidTexture(128u, 128u, 255u, 255u);
                m_FallbackBlack = CreateSolidTexture(0u, 0u, 0u, 255u);
                ConfigureSamplerUniforms();
            }
            catch (...)
            {
                Destroy();
                throw;
            }
        }

        ~OpenGLRendererRuntime()
        {
            glfwMakeContextCurrent(m_Window.NativeHandle());
            Destroy();
        }

        OpenGLRendererRuntime(const OpenGLRendererRuntime&) = delete;
        OpenGLRendererRuntime& operator=(const OpenGLRendererRuntime&) = delete;

        [[nodiscard]] GraphicsBackend Backend() const noexcept
        {
            return GraphicsBackend::OpenGL;
        }

        [[nodiscard]] Window& NativeWindow() noexcept { return m_Window; }

        /// Task: render shadows and the complete offscreen scene, resolve any
        /// synchronous OpenGL readback requests, and scale the target into the
        /// current native framebuffer. A minimized window performs no work.
        void DrawFrame()
        {
            glfwMakeContextCurrent(m_Window.NativeHandle());
            const auto [windowWidth, windowHeight] = m_Window.FramebufferExtent();
            if (windowWidth == 0u || windowHeight == 0u) return;

            const auto shadowIndex = ShadowLightIndex();
            const auto lightViewProjection = BuildLightViewProjection(shadowIndex);
            if (shadowIndex.has_value() && m_ShadowSettings.Enabled)
                DrawShadowPass(lightViewProjection);
            DrawViewportPass(lightViewProjection, shadowIndex);
            CompleteReadbacks();
            Present(windowWidth, windowHeight);
            if (m_OverlayRecorder) m_OverlayRecorder();
            glfwSwapBuffers(m_Window.NativeHandle());
        }

        void SubmitDebugDraw(const DebugDrawList& debug)
        {
            m_DebugVertices.clear();
            if (debug.Lines().size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()) / 2u)
                throw std::length_error("OpenGL debug draw exceeds GLsizei capacity.");
            m_DebugVertices.reserve(debug.Lines().size() * 2u);
            for (const DebugLine& line : debug.Lines())
            {
                m_DebugVertices.push_back(MakeDebugVertex(line.A, line.Color));
                m_DebugVertices.push_back(MakeDebugVertex(line.B, line.Color));
            }
        }

        [[nodiscard]] MeshHandle CreateMesh(const Mesh& mesh)
        {
            if (mesh.Indices().size() >
                static_cast<std::size_t>(std::numeric_limits<GLsizei>::max()))
                throw std::length_error("OpenGL mesh index count exceeds GLsizei capacity.");
            glfwMakeContextCurrent(m_Window.NativeHandle());
            GpuMesh gpu;
            glGenVertexArrays(1, &gpu.VertexArray);
            glGenBuffers(1, &gpu.VertexBuffer);
            glGenBuffers(1, &gpu.IndexBuffer);
            try
            {
                if (gpu.VertexArray == 0u || gpu.VertexBuffer == 0u ||
                    gpu.IndexBuffer == 0u)
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

        void DestroyMesh(MeshHandle mesh)
        {
            const auto found = m_Meshes.find(mesh);
            if (found == m_Meshes.end())
                throw std::invalid_argument("OpenGL mesh handle is not owned by this runtime.");
            if (std::ranges::any_of(m_Draws,
                [&](const MeshDraw& draw) { return draw.Mesh == mesh; }))
                throw std::logic_error("Cannot destroy an OpenGL mesh used by the submitted scene.");
            glfwMakeContextCurrent(m_Window.NativeHandle());
            DeleteMesh(found->second);
            m_Meshes.erase(found);
        }

        [[nodiscard]] TextureHandle CreateTexture(
            const kairo::assets::TextureArtifactData& texture,
            TextureSampler sampling = {})
        {
            kairo::assets::ValidateTextureArtifactData(texture);
            glfwMakeContextCurrent(m_Window.NativeHandle());
            GpuTexture gpu;
            glGenTextures(1, &gpu.Name);
            try
            {
                if (gpu.Name == 0u)
                    throw std::runtime_error("OpenGL failed to allocate a texture object.");
                glBindTexture(GL_TEXTURE_2D, gpu.Name);
                const auto format = TextureFormat(texture);
                for (std::size_t level = 0u; level < texture.Mips.size(); ++level)
                {
                    const auto& mip = texture.Mips[level];
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                        format.Internal, CheckedGLDimension(mip.Width),
                        CheckedGLDimension(mip.Height), 0, format.External,
                        format.Type, mip.Pixels.data());
                }
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                    static_cast<GLint>(texture.Mips.size() - 1u));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    texture.Mips.size() > 1u ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                    ToOpenGL(sampling.AddressU));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                    ToOpenGL(sampling.AddressV));
                glBindTexture(GL_TEXTURE_2D, 0u);
                ThrowIfGLError("uploading a texture");
                gpu.MipLevels = static_cast<std::uint32_t>(texture.Mips.size());
            }
            catch (...)
            {
                if (gpu.Name != 0u) glDeleteTextures(1, &gpu.Name);
                throw;
            }
            const TextureHandle handle = NextHandle(m_NextTexture, m_Textures);
            m_Textures.emplace(handle, gpu);
            return handle;
        }

        void DestroyTexture(TextureHandle texture)
        {
            const auto found = m_Textures.find(texture);
            if (found == m_Textures.end())
                throw std::invalid_argument("OpenGL texture handle is not owned by this runtime.");
            if (TextureIsSubmitted(texture))
                throw std::logic_error("Cannot destroy an OpenGL texture used by the submitted scene.");
            glfwMakeContextCurrent(m_Window.NativeHandle());
            glDeleteTextures(1, &found->second.Name);
            m_Textures.erase(found);
        }

        void SubmitRenderScene(const RenderScene& scene)
        {
            for (const MeshDraw& draw : scene.Draws())
            {
                RenderScene::Validate(draw);
                if (!m_Meshes.contains(draw.Mesh))
                    throw std::invalid_argument("RenderScene references an OpenGL mesh owned by another runtime.");
                ValidateTextureHandle(draw.Material.BaseColorTexture);
                ValidateTextureHandle(draw.Material.NormalTexture);
                ValidateTextureHandle(draw.Material.MetallicRoughnessTexture);
                ValidateTextureHandle(draw.Material.EmissiveTexture);
                ValidateTextureHandle(draw.Material.OcclusionTexture);
            }
            ValidateTextureHandle(scene.Environment().EnvironmentTexture);
            m_Draws = scene.Draws();
            m_Lights = scene.Lights();
            m_Environment = scene.Environment();
        }

        void SetCameraPose(const CameraPose& pose) { m_Camera.SetPose(pose); }

        void SetDirectionalShadowSettings(const DirectionalShadowSettings& settings)
        {
            settings.Validate();
            m_ShadowSettings = settings;
        }

        [[nodiscard]] const DirectionalShadowSettings& DirectionalShadows() const noexcept
        {
            return m_ShadowSettings;
        }

        void SetViewportShadingMode(ViewportShadingMode mode) noexcept
        {
            m_ShadingMode = mode;
        }

        [[nodiscard]] ViewportShadingMode ViewportShading() const noexcept
        {
            return m_ShadingMode;
        }

        void ResizeViewport(std::uint32_t width, std::uint32_t height)
        {
            if (width == 0u || height == 0u) return;
            if (width > 32'768u || height > 32'768u)
                throw std::invalid_argument("OpenGL viewport dimensions exceed 32768.");
            if (width == m_ViewportWidth && height == m_ViewportHeight) return;
            glfwMakeContextCurrent(m_Window.NativeHandle());
            CreateViewportTarget(width, height);
            m_ViewportWidth = width;
            m_ViewportHeight = height;
            if (++m_ViewportGeneration == 0u) ++m_ViewportGeneration;
        }

        void RequestViewportPick(std::uint32_t x, std::uint32_t y)
        {
            if (x >= m_ViewportWidth || y >= m_ViewportHeight)
                throw std::out_of_range("Viewport pick lies outside the render target.");
            m_PickRequest = std::pair{x, y};
        }

        [[nodiscard]] std::optional<std::uint32_t> TakeViewportPickResult() noexcept
        {
            return std::exchange(m_PickResult, std::nullopt);
        }

        void RequestViewportCapture()
        {
            if (m_CaptureRequested)
                throw std::logic_error("A viewport capture is already pending.");
            m_CaptureRequested = true;
        }

        [[nodiscard]] std::optional<ViewportCapture> TakeViewportCapture() noexcept
        {
            return std::exchange(m_CaptureResult, std::nullopt);
        }

        [[nodiscard]] OpenGLViewportTexture ViewportTexture() const noexcept
        {
            return { m_ViewportColor, m_ViewportWidth, m_ViewportHeight,
                m_ViewportGeneration };
        }

        void SetOverlayRecorder(OpenGLOverlayRecorder recorder)
        {
            m_OverlayRecorder = std::move(recorder);
        }

    private:
        struct PixelFormat final
        {
            GLint Internal = GL_RGBA8;
            GLenum External = GL_RGBA;
            GLenum Type = GL_UNSIGNED_BYTE;
        };

        static void ValidateContext()
        {
            const GLubyte* version = glGetString(GL_VERSION);
            if (version == nullptr)
                throw PresentationUnavailableError(
                    "OpenGL context exists but did not report a version.");
            GLint major = 0;
            GLint minor = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            if (major < 4 || (major == 4 && minor < 1))
                throw PresentationUnavailableError(
                    "KairoRenderer requires an OpenGL 4.1 core context; host reported " +
                    std::to_string(major) + "." + std::to_string(minor) + ".");
        }

        [[nodiscard]] static GLuint CompileShader(
            GLenum stage, std::string_view source, std::string_view label)
        {
            const GLuint shader = glCreateShader(stage);
            if (shader == 0u) throw std::runtime_error("glCreateShader failed for " + std::string(label) + ".");
            const GLchar* data = source.data();
            const GLint length = static_cast<GLint>(source.size());
            glShaderSource(shader, 1, &data, &length);
            glCompileShader(shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (compiled == GL_TRUE) return shader;
            GLint logLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
            GLsizei written = 0;
            glGetShaderInfoLog(shader, logLength, &written, log.data());
            glDeleteShader(shader);
            log.resize(static_cast<std::size_t>(std::max(written, GLsizei{0})));
            throw std::runtime_error(std::string(label) + " compilation failed: " + log);
        }

        [[nodiscard]] static GLuint CreateProgram(
            std::string_view vertexSource, std::string_view fragmentSource,
            std::string_view label)
        {
            const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource,
                std::string(label) + " vertex shader");
            GLuint fragment = 0u;
            GLuint program = 0u;
            try
            {
                fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource,
                    std::string(label) + " fragment shader");
                program = glCreateProgram();
                if (program == 0u) throw std::runtime_error("glCreateProgram failed for " + std::string(label) + ".");
                glAttachShader(program, vertex);
                glAttachShader(program, fragment);
                glLinkProgram(program);
                GLint linked = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linked);
                if (linked != GL_TRUE)
                {
                    GLint logLength = 0;
                    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
                    std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
                    GLsizei written = 0;
                    glGetProgramInfoLog(program, logLength, &written, log.data());
                    log.resize(static_cast<std::size_t>(std::max(written, GLsizei{0})));
                    throw std::runtime_error(std::string(label) + " link failed: " + log);
                }
            }
            catch (...)
            {
                if (program != 0u) glDeleteProgram(program);
                if (fragment != 0u) glDeleteShader(fragment);
                glDeleteShader(vertex);
                throw;
            }
            glDetachShader(program, vertex);
            glDetachShader(program, fragment);
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            return program;
        }

        void ConfigureSamplerUniforms() const
        {
            glUseProgram(m_MeshProgram);
            SetUniform(m_MeshProgram, "uShadowMap", 0);
            SetUniform(m_MeshProgram, "uBaseColorMap", 1);
            SetUniform(m_MeshProgram, "uNormalMap", 2);
            SetUniform(m_MeshProgram, "uMetallicRoughnessMap", 3);
            SetUniform(m_MeshProgram, "uEmissiveMap", 4);
            SetUniform(m_MeshProgram, "uOcclusionMap", 5);
            SetUniform(m_MeshProgram, "uEnvironmentMap", 6);
            glUseProgram(0u);
        }

        void CreateViewportTarget(std::uint32_t width, std::uint32_t height)
        {
            GLuint framebuffer = 0u;
            GLuint color = 0u;
            GLuint objectID = 0u;
            GLuint depth = 0u;
            glGenFramebuffers(1, &framebuffer);
            glGenTextures(1, &color);
            glGenTextures(1, &objectID);
            glGenRenderbuffers(1, &depth);
            try
            {
                if (framebuffer == 0u || color == 0u || objectID == 0u || depth == 0u)
                    throw std::runtime_error("OpenGL failed to allocate viewport attachments.");
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glBindTexture(GL_TEXTURE_2D, color);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                    CheckedGLDimension(width), CheckedGLDimension(height), 0,
                    GL_RGBA, GL_HALF_FLOAT, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_2D, color, 0);

                glBindTexture(GL_TEXTURE_2D, objectID);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
                    CheckedGLDimension(width), CheckedGLDimension(height), 0,
                    GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                    GL_TEXTURE_2D, objectID, 0);

                glBindRenderbuffer(GL_RENDERBUFFER, depth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F,
                    CheckedGLDimension(width), CheckedGLDimension(height));
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                    GL_RENDERBUFFER, depth);
                constexpr GLenum attachments[]{ GL_COLOR_ATTACHMENT0,
                    GL_COLOR_ATTACHMENT1 };
                glDrawBuffers(2, attachments);
                RequireCompleteFramebuffer("OpenGL viewport framebuffer");
                glBindFramebuffer(GL_FRAMEBUFFER, 0u);
            }
            catch (...)
            {
                if (depth != 0u) glDeleteRenderbuffers(1, &depth);
                if (objectID != 0u) glDeleteTextures(1, &objectID);
                if (color != 0u) glDeleteTextures(1, &color);
                if (framebuffer != 0u) glDeleteFramebuffers(1, &framebuffer);
                throw;
            }
            DeleteViewportTarget();
            m_ViewportFramebuffer = framebuffer;
            m_ViewportColor = color;
            m_ViewportObjectID = objectID;
            m_ViewportDepth = depth;
        }

        void CreateShadowTarget()
        {
            glGenFramebuffers(1, &m_ShadowFramebuffer);
            glGenTextures(1, &m_ShadowDepth);
            if (m_ShadowFramebuffer == 0u || m_ShadowDepth == 0u)
                throw std::runtime_error("OpenGL failed to allocate shadow resources.");
            glBindTexture(GL_TEXTURE_2D, m_ShadowDepth);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F,
                ShadowResolution, ShadowResolution, 0, GL_DEPTH_COMPONENT,
                GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
            const GLfloat border[]{ 1.0f, 1.0f, 1.0f, 1.0f };
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
            glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowFramebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                GL_TEXTURE_2D, m_ShadowDepth, 0);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
            RequireCompleteFramebuffer("OpenGL shadow framebuffer");
            glBindFramebuffer(GL_FRAMEBUFFER, 0u);
        }

        void CreateDebugResources()
        {
            glGenVertexArrays(1, &m_DebugVertexArray);
            glGenBuffers(1, &m_DebugVertexBuffer);
            if (m_DebugVertexArray == 0u || m_DebugVertexBuffer == 0u)
                throw std::runtime_error("OpenGL failed to allocate debug-line resources.");
            glBindVertexArray(m_DebugVertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, m_DebugVertexBuffer);
            constexpr GLsizei stride = static_cast<GLsizei>(sizeof(DebugVertex));
            ConfigureVertexAttribute(0u, 3, stride, offsetof(DebugVertex, Position));
            ConfigureVertexAttribute(1u, 4, stride, offsetof(DebugVertex, Color));
            glBindVertexArray(0u);
        }

        [[nodiscard]] static GLuint CreateSolidTexture(
            std::uint8_t red, std::uint8_t green, std::uint8_t blue,
            std::uint8_t alpha)
        {
            const std::array<std::uint8_t, 4u> pixel{ red, green, blue, alpha };
            GLuint texture = 0u;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, pixel.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            return texture;
        }

        void DrawShadowPass(const kairo::foundation::math::Mat4f& lightViewProjection)
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
            glUseProgram(m_ShadowProgram);
            SetMatrix(m_ShadowProgram, "uLightViewProjection", lightViewProjection);
            for (const MeshDraw& draw : m_Draws)
            {
                if (!draw.CastShadows ||
                    draw.Material.AlphaMode == MaterialAlphaMode::Blend) continue;
                SetMatrix(m_ShadowProgram, "uModel", draw.Model);
                DrawMesh(draw.Mesh);
            }
            glDisable(GL_POLYGON_OFFSET_FILL);
            glBindFramebuffer(GL_FRAMEBUFFER, 0u);
        }

        void DrawViewportPass(
            const kairo::foundation::math::Mat4f& lightViewProjection,
            std::optional<std::size_t> shadowIndex)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_ViewportFramebuffer);
            glViewport(0, 0, CheckedGLDimension(m_ViewportWidth),
                CheckedGLDimension(m_ViewportHeight));
            constexpr GLenum attachments[]{ GL_COLOR_ATTACHMENT0,
                GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, attachments);
            const GLfloat clearColor[]{ m_Environment.BackgroundColor.x,
                m_Environment.BackgroundColor.y,
                m_Environment.BackgroundColor.z, 1.0f };
            constexpr GLuint clearID[]{ 0u };
            constexpr GLfloat clearDepth[]{ 1.0f };
            glClearBufferfv(GL_COLOR, 0, clearColor);
            glClearBufferuiv(GL_COLOR, 1, clearID);
            glClearBufferfv(GL_DEPTH, 0, clearDepth);
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glDisable(GL_BLEND);

            glUseProgram(m_MeshProgram);
            UploadFrameUniforms(lightViewProjection, shadowIndex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_ShadowDepth);

            for (std::size_t index = 0u; index < m_Draws.size(); ++index)
                if (m_Draws[index].Material.AlphaMode != MaterialAlphaMode::Blend)
                    DrawOne(m_Draws[index]);

            std::vector<std::size_t> transparent;
            transparent.reserve(m_Draws.size());
            for (std::size_t index = 0u; index < m_Draws.size(); ++index)
                if (m_Draws[index].Material.AlphaMode == MaterialAlphaMode::Blend)
                    transparent.push_back(index);
            using kairo::foundation::math::Dot;
            using kairo::foundation::math::ExtractTranslation;
            const auto cameraPosition = m_Camera.Position();
            std::stable_sort(transparent.begin(), transparent.end(),
                [&](std::size_t left, std::size_t right)
                {
                    const auto a = ExtractTranslation(m_Draws[left].Model) - cameraPosition;
                    const auto b = ExtractTranslation(m_Draws[right].Model) - cameraPosition;
                    return Dot(a, a) > Dot(b, b);
                });
            if (!transparent.empty())
            {
                glEnablei(GL_BLEND, 0u);
                glDisablei(GL_BLEND, 1u);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                for (const std::size_t index : transparent) DrawOne(m_Draws[index]);
                glDepthMask(GL_TRUE);
                glDisablei(GL_BLEND, 0u);
            }
            DrawDebugLines();
            glBindVertexArray(0u);
            glUseProgram(0u);
            glBindFramebuffer(GL_FRAMEBUFFER, 0u);
            ThrowIfGLError("rendering an OpenGL frame");
        }

        void UploadFrameUniforms(
            const kairo::foundation::math::Mat4f& lightViewProjection,
            std::optional<std::size_t> shadowIndex) const
        {
            SetMatrix(m_MeshProgram, "uView", m_Camera.View());
            SetMatrix(m_MeshProgram, "uProjection",
                OpenGLCameraProjection(m_ViewportWidth, m_ViewportHeight));
            SetMatrix(m_MeshProgram, "uLightViewProjection", lightViewProjection);
            SetUniform(m_MeshProgram, "uCameraPosition", m_Camera.Position());
            SetUniform(m_MeshProgram, "uAmbient",
                m_Environment.AmbientColor * m_Environment.AmbientIntensity);
            SetUniform(m_MeshProgram, "uBackground", m_Environment.BackgroundColor);
            SetUniform(m_MeshProgram, "uExposure", m_Environment.ExposureEV100);
            SetUniform(m_MeshProgram, "uEnvironmentIntensity",
                m_Environment.EnvironmentIntensity);
            SetUniform(m_MeshProgram, "uShadingMode", static_cast<int>(m_ShadingMode));
            SetUniform(m_MeshProgram, "uLightCount", static_cast<int>(m_Lights.size()));
            SetUniform(m_MeshProgram, "uShadowLightIndex",
                shadowIndex.has_value() ? static_cast<int>(*shadowIndex) : -1);
            SetUniform(m_MeshProgram, "uShadowEnabled",
                m_ShadowSettings.Enabled && shadowIndex.has_value() ? 1 : 0);
            SetUniform(m_MeshProgram, "uShadowStrength", m_ShadowSettings.Strength);
            SetUniform(m_MeshProgram, "uShadowTexel",
                1.0f / static_cast<float>(ShadowResolution));
            SetUniform(m_MeshProgram, "uReceiverBias", m_ShadowSettings.ReceiverBias);
            for (std::size_t index = 0u; index < m_Lights.size(); ++index)
            {
                const RenderLight& light = m_Lights[index];
                const auto direction = kairo::foundation::math::SafeNormalize(
                    light.Direction, kairo::foundation::math::Vec3f::Up());
                SetUniform4(m_MeshProgram, IndexedUniform("uLightPositionType", index),
                    light.Position.x, light.Position.y, light.Position.z,
                    static_cast<float>(light.Type));
                SetUniform4(m_MeshProgram, IndexedUniform("uLightDirectionRange", index),
                    direction.x, direction.y, direction.z, light.Range);
                SetUniform4(m_MeshProgram, IndexedUniform("uLightColorIntensity", index),
                    light.Color.x, light.Color.y, light.Color.z, light.Intensity);
                SetUniform4(m_MeshProgram, IndexedUniform("uLightSpot", index),
                    std::cos(light.InnerConeRadians),
                    std::cos(light.OuterConeRadians), light.AreaWidth,
                    light.AreaHeight);
            }
        }

        void DrawOne(const MeshDraw& draw) const
        {
            const auto normal = ComputeNormalMatrix(draw.Model);
            SetMatrix(m_MeshProgram, "uModel", draw.Model);
            SetMatrix(m_MeshProgram, "uNormalMatrix", normal);
            SetUniform4(m_MeshProgram, "uBaseColorFactor",
                draw.Material.BaseColor.x, draw.Material.BaseColor.y,
                draw.Material.BaseColor.z, draw.Material.BaseColorAlpha);
            SetUniform(m_MeshProgram, "uEmissiveFactor", draw.Material.Emissive);
            SetUniform4(m_MeshProgram, "uMaterialFactors",
                draw.Material.Metallic, draw.Material.Roughness,
                draw.Material.AmbientOcclusion, draw.Material.AlphaCutoff);
            SetUniform(m_MeshProgram, "uNormalScale", draw.Material.NormalScale);
            SetUniform(m_MeshProgram, "uAlphaMode",
                static_cast<int>(draw.Material.AlphaMode));
            SetUniformUnsigned(m_MeshProgram, "uObjectID", draw.ObjectID);
            SetUniform(m_MeshProgram, "uReceiveShadows", draw.ReceiveShadows ? 1 : 0);
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
            SetUniform(m_MeshProgram, "uEnvironmentMaxLod", maxLod);
            if (draw.Material.DoubleSided) glDisable(GL_CULL_FACE);
            else glEnable(GL_CULL_FACE);
            DrawMesh(draw.Mesh);
        }

        void DrawDebugLines()
        {
            if (m_DebugVertices.empty()) return;
            glUseProgram(m_DebugProgram);
            SetMatrix(m_DebugProgram, "uView", m_Camera.View());
            SetMatrix(m_DebugProgram, "uProjection",
                OpenGLCameraProjection(m_ViewportWidth, m_ViewportHeight));
            glBindVertexArray(m_DebugVertexArray);
            glBindBuffer(GL_ARRAY_BUFFER, m_DebugVertexBuffer);
            glBufferData(GL_ARRAY_BUFFER,
                CheckedGLSize(m_DebugVertices.size() * sizeof(DebugVertex)),
                m_DebugVertices.data(), GL_DYNAMIC_DRAW);
            glDisable(GL_CULL_FACE);
            glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_DebugVertices.size()));
        }

        void CompleteReadbacks()
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ViewportFramebuffer);
            if (m_PickRequest.has_value())
            {
                glReadBuffer(GL_COLOR_ATTACHMENT1);
                std::uint32_t id = 0u;
                glReadPixels(static_cast<GLint>(m_PickRequest->first),
                    static_cast<GLint>(m_ViewportHeight - 1u - m_PickRequest->second),
                    1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);
                m_PickResult = id;
                m_PickRequest.reset();
            }
            if (m_CaptureRequested)
            {
                ViewportCapture capture;
                capture.Width = m_ViewportWidth;
                capture.Height = m_ViewportHeight;
                capture.RGBA.resize(static_cast<std::size_t>(m_ViewportWidth) *
                    m_ViewportHeight * 4u);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glReadPixels(0, 0, CheckedGLDimension(m_ViewportWidth),
                    CheckedGLDimension(m_ViewportHeight), GL_RGBA,
                    GL_UNSIGNED_BYTE, capture.RGBA.data());
                FlipRows(capture);
                m_CaptureResult = std::move(capture);
                m_CaptureRequested = false;
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0u);
        }

        void Present(std::uint32_t width, std::uint32_t height) const
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ViewportFramebuffer);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0u);
            glEnable(GL_FRAMEBUFFER_SRGB);
            glBlitFramebuffer(0, 0, CheckedGLDimension(m_ViewportWidth),
                CheckedGLDimension(m_ViewportHeight), 0, 0,
                CheckedGLDimension(width), CheckedGLDimension(height),
                GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glBindFramebuffer(GL_FRAMEBUFFER, 0u);
        }

        [[nodiscard]] std::optional<std::size_t> ShadowLightIndex() const noexcept
        {
            for (std::size_t index = 0u; index < m_Lights.size(); ++index)
                if (m_Lights[index].Type == RenderLightType::Directional &&
                    m_Lights[index].CastShadows) return index;
            return std::nullopt;
        }

        [[nodiscard]] kairo::foundation::math::Mat4f BuildLightViewProjection(
            std::optional<std::size_t> index) const
        {
            using namespace kairo::foundation::math;
            const Vec3f direction = index.has_value()
                ? SafeNormalize(m_Lights[*index].Direction, Vec3f::Up())
                : Vec3f::Up();
            const Mat4f view = LookAt(direction * 12.0f, Vec3f::Zero(), Vec3f::Up());
            return ToOpenGLDepth(Orthographic(-8.0f, 8.0f, -8.0f, 8.0f,
                0.1f, 30.0f)) * view;
        }

        [[nodiscard]] kairo::foundation::math::Mat4f OpenGLCameraProjection(
            std::uint32_t width, std::uint32_t height) const
        {
            auto projection = m_Camera.Projection(width, height);
            // ShowcaseCamera flips Y for Vulkan's positive viewport. OpenGL's
            // lower-left viewport convention requires the original sign.
            projection(1u, 1u) *= -1.0f;
            return ToOpenGLDepth(projection);
        }

        [[nodiscard]] static kairo::foundation::math::Mat4f ToOpenGLDepth(
            kairo::foundation::math::Mat4f projection) noexcept
        {
            // KairoMath projection depth is [0,1]. OpenGL 4.1 clips against
            // [-w,+w], so z'=2z-w converts the range without changing x/y.
            for (std::size_t column = 0u; column < 4u; ++column)
                projection(2u, column) = 2.0f * projection(2u, column) -
                    projection(3u, column);
            return projection;
        }

        void DrawMesh(MeshHandle handle) const
        {
            const GpuMesh& mesh = m_Meshes.at(handle);
            glBindVertexArray(mesh.VertexArray);
            glDrawElements(GL_TRIANGLES, mesh.IndexCount, GL_UNSIGNED_INT, nullptr);
        }

        [[nodiscard]] GLuint TextureOr(TextureHandle handle, GLuint fallback) const
        {
            return handle == InvalidTextureHandle ? fallback : m_Textures.at(handle).Name;
        }

        void ValidateTextureHandle(TextureHandle handle) const
        {
            if (handle != InvalidTextureHandle && !m_Textures.contains(handle))
                throw std::invalid_argument("RenderScene references an OpenGL texture owned by another runtime.");
        }

        [[nodiscard]] bool TextureIsSubmitted(TextureHandle handle) const noexcept
        {
            if (m_Environment.EnvironmentTexture == handle) return true;
            for (const MeshDraw& draw : m_Draws)
                if (draw.Material.BaseColorTexture == handle ||
                    draw.Material.NormalTexture == handle ||
                    draw.Material.MetallicRoughnessTexture == handle ||
                    draw.Material.EmissiveTexture == handle ||
                    draw.Material.OcclusionTexture == handle) return true;
            return false;
        }

        [[nodiscard]] static PixelFormat TextureFormat(
            const kairo::assets::TextureArtifactData& texture)
        {
            using enum kairo::assets::TexturePixelFormat;
            switch (texture.Format)
            {
                case R8: return { GL_R8, GL_RED, GL_UNSIGNED_BYTE };
                case RG8: return { GL_RG8, GL_RG, GL_UNSIGNED_BYTE };
                case RGBA8:
                    return { texture.ColorSpace == kairo::assets::TextureColorSpace::SRGB
                        ? GL_SRGB8_ALPHA8 : GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
                case RGBA16Float: return { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
            }
            throw std::invalid_argument("Texture pixel format is unsupported by OpenGL.");
        }

        [[nodiscard]] static GLint ToOpenGL(TextureAddressMode mode)
        {
            switch (mode)
            {
                case TextureAddressMode::Repeat: return GL_REPEAT;
                case TextureAddressMode::MirroredRepeat: return GL_MIRRORED_REPEAT;
                case TextureAddressMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
            }
            throw std::invalid_argument("Texture address mode is unsupported by OpenGL.");
        }

        template<class Map>
        [[nodiscard]] static std::uint64_t NextHandle(std::uint64_t& next,
            const Map& resources)
        {
            if (resources.size() == std::numeric_limits<std::uint64_t>::max() - 1u)
                throw std::length_error("Renderer handle space is exhausted.");
            while (next == 0u || resources.contains(next)) ++next;
            return next++;
        }

        static void ConfigureVertexAttribute(GLuint index, GLint components,
            GLsizei stride, std::size_t offset)
        {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(index, components, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offset));
        }

        [[nodiscard]] static GLsizeiptr CheckedGLSize(std::size_t bytes)
        {
            if (bytes > static_cast<std::size_t>(
                std::numeric_limits<GLsizeiptr>::max()))
                throw std::length_error("OpenGL upload exceeds GLsizeiptr capacity.");
            return static_cast<GLsizeiptr>(bytes);
        }

        [[nodiscard]] static GLsizei CheckedGLDimension(std::uint32_t value)
        {
            if (value > static_cast<std::uint32_t>(
                std::numeric_limits<GLsizei>::max()))
                throw std::length_error("OpenGL dimension exceeds GLsizei capacity.");
            return static_cast<GLsizei>(value);
        }

        static void RequireCompleteFramebuffer(std::string_view label)
        {
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error(std::string(label) +
                    " is incomplete (status " + std::to_string(status) + ").");
        }

        static void ThrowIfGLError(std::string_view operation)
        {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR)
                throw std::runtime_error("OpenGL error " + std::to_string(error) +
                    " while " + std::string(operation) + ".");
        }

        [[nodiscard]] static GLint Uniform(GLuint program, std::string_view name)
        {
            const std::string owned(name);
            return glGetUniformLocation(program, owned.c_str());
        }

        static void SetUniform(GLuint program, std::string_view name, GLint value)
        {
            const GLint location = Uniform(program, name);
            if (location >= 0) glUniform1i(location, value);
        }

        static void SetUniform(GLuint program, std::string_view name, GLfloat value)
        {
            const GLint location = Uniform(program, name);
            if (location >= 0) glUniform1f(location, value);
        }

        static void SetUniformUnsigned(GLuint program, std::string_view name,
            GLuint value)
        {
            const GLint location = Uniform(program, name);
            if (location >= 0) glUniform1ui(location, value);
        }

        static void SetUniform(GLuint program, std::string_view name,
            const kairo::foundation::math::Vec3f& value)
        {
            const GLint location = Uniform(program, name);
            if (location >= 0) glUniform3f(location, value.x, value.y, value.z);
        }

        static void SetUniform4(GLuint program, std::string_view name,
            GLfloat x, GLfloat y, GLfloat z, GLfloat w)
        {
            const GLint location = Uniform(program, name);
            if (location >= 0) glUniform4f(location, x, y, z, w);
        }

        static void SetMatrix(GLuint program, std::string_view name,
            const kairo::foundation::math::Mat4f& matrix)
        {
            std::array<float, 16u> columnMajor{};
            for (std::size_t row = 0u; row < 4u; ++row)
                for (std::size_t column = 0u; column < 4u; ++column)
                    columnMajor[column * 4u + row] = matrix(row, column);
            const GLint location = Uniform(program, name);
            if (location >= 0)
                glUniformMatrix4fv(location, 1, GL_FALSE, columnMajor.data());
        }

        static void SetMatrix(GLuint program, std::string_view name,
            const kairo::foundation::math::Mat3f& matrix)
        {
            std::array<float, 9u> columnMajor{};
            for (std::size_t row = 0u; row < 3u; ++row)
                for (std::size_t column = 0u; column < 3u; ++column)
                    columnMajor[column * 3u + row] = matrix(row, column);
            const GLint location = Uniform(program, name);
            if (location >= 0)
                glUniformMatrix3fv(location, 1, GL_FALSE, columnMajor.data());
        }

        static void BindTextureUnit(GLuint unit, GLuint texture)
        {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, texture);
        }

        [[nodiscard]] static std::string IndexedUniform(
            std::string_view base, std::size_t index)
        {
            return std::string(base) + "[" + std::to_string(index) + "]";
        }

        [[nodiscard]] static DebugVertex MakeDebugVertex(
            const kairo::foundation::math::Vec3f& position,
            DebugColor color) noexcept
        {
            return { { position.x, position.y, position.z },
                { color.R, color.G, color.B, color.A } };
        }

        static void FlipRows(ViewportCapture& capture)
        {
            const std::size_t rowBytes = static_cast<std::size_t>(capture.Width) * 4u;
            std::vector<std::uint8_t> temporary(rowBytes);
            for (std::uint32_t y = 0u; y < capture.Height / 2u; ++y)
            {
                std::uint8_t* top = capture.RGBA.data() +
                    static_cast<std::size_t>(y) * rowBytes;
                std::uint8_t* bottom = capture.RGBA.data() +
                    static_cast<std::size_t>(capture.Height - 1u - y) * rowBytes;
                std::memcpy(temporary.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, temporary.data(), rowBytes);
            }
        }

        static void DeleteMesh(const GpuMesh& mesh) noexcept
        {
            if (mesh.IndexBuffer != 0u) glDeleteBuffers(1, &mesh.IndexBuffer);
            if (mesh.VertexBuffer != 0u) glDeleteBuffers(1, &mesh.VertexBuffer);
            if (mesh.VertexArray != 0u) glDeleteVertexArrays(1, &mesh.VertexArray);
        }

        void DeleteViewportTarget() noexcept
        {
            if (m_ViewportDepth != 0u)
                glDeleteRenderbuffers(1, &m_ViewportDepth);
            if (m_ViewportObjectID != 0u)
                glDeleteTextures(1, &m_ViewportObjectID);
            if (m_ViewportColor != 0u) glDeleteTextures(1, &m_ViewportColor);
            if (m_ViewportFramebuffer != 0u)
                glDeleteFramebuffers(1, &m_ViewportFramebuffer);
            m_ViewportDepth = m_ViewportObjectID = m_ViewportColor =
                m_ViewportFramebuffer = 0u;
        }

        void Destroy() noexcept
        {
            for (const auto& [handle, mesh] : m_Meshes)
            {
                static_cast<void>(handle);
                DeleteMesh(mesh);
            }
            m_Meshes.clear();
            for (const auto& [handle, texture] : m_Textures)
            {
                static_cast<void>(handle);
                if (texture.Name != 0u) glDeleteTextures(1, &texture.Name);
            }
            m_Textures.clear();
            if (m_FallbackBlack != 0u) glDeleteTextures(1, &m_FallbackBlack);
            if (m_FallbackNormal != 0u) glDeleteTextures(1, &m_FallbackNormal);
            if (m_FallbackWhite != 0u) glDeleteTextures(1, &m_FallbackWhite);
            if (m_DebugVertexBuffer != 0u)
                glDeleteBuffers(1, &m_DebugVertexBuffer);
            if (m_DebugVertexArray != 0u)
                glDeleteVertexArrays(1, &m_DebugVertexArray);
            if (m_ShadowDepth != 0u) glDeleteTextures(1, &m_ShadowDepth);
            if (m_ShadowFramebuffer != 0u)
                glDeleteFramebuffers(1, &m_ShadowFramebuffer);
            DeleteViewportTarget();
            if (m_DebugProgram != 0u) glDeleteProgram(m_DebugProgram);
            if (m_ShadowProgram != 0u) glDeleteProgram(m_ShadowProgram);
            if (m_MeshProgram != 0u) glDeleteProgram(m_MeshProgram);
            m_FallbackBlack = m_FallbackNormal = m_FallbackWhite = 0u;
            m_DebugVertexBuffer = m_DebugVertexArray = 0u;
            m_ShadowDepth = m_ShadowFramebuffer = 0u;
            m_DebugProgram = m_ShadowProgram = m_MeshProgram = 0u;
        }
    };
}
