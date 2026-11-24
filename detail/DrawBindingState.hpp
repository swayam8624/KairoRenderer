#pragma once

#include <functional>
#include <optional>

namespace kairo::renderer::detail
{
    // A command-buffer recording pass owns one tracker. It starts unknown
    // even if the preceding pass happened to bind the same native handles.
    // No persistent GPU state is inferred across passes or command buffers.
    template<class Pipeline, class Mesh>
    class DrawBindingState final
    {
    public:
        template<class Bind>
        void PipelineFor(Pipeline pipeline, Bind&& bind)
        {
            if (m_Pipeline && *m_Pipeline == pipeline) return;
            std::invoke(bind);
            m_Pipeline = pipeline;
        }

        template<class Bind>
        void MeshFor(Mesh mesh, Bind&& bind)
        {
            if (m_Mesh && *m_Mesh == mesh) return;
            std::invoke(bind);
            m_Mesh = mesh;
        }

    private:
        std::optional<Pipeline> m_Pipeline;
        std::optional<Mesh> m_Mesh;
    };
}
