from pathlib import Path

def rep(path, old, new):
    p=Path(path); t=p.read_text()
    if old not in t: raise SystemExit(f'missing target in {path}: {old[:80]!r}')
    p.write_text(t.replace(old,new,1))

rep('CMakeLists.txt',
'''    if(TARGET KairoRendererPhysicsDebug)
''',
'''    if(WIN32)
        add_executable(KairoRendererDirect3D12SkinningSmoke
            tests/Direct3D12SkinningSmoke.cpp)
        target_link_libraries(KairoRendererDirect3D12SkinningSmoke PRIVATE KairoRenderer)
        add_test(NAME KairoRendererDirect3D12SkinningSmoke
            COMMAND KairoRendererDirect3D12SkinningSmoke)
        set_tests_properties(KairoRendererDirect3D12SkinningSmoke PROPERTIES
            SKIP_RETURN_CODE 77)
    endif()
    if(TARGET KairoRendererPhysicsDebug)
''')

rep('detail/Direct3D12Backend.cpp',
'''        struct alignas(256) SkinConstants final
        {
            float Joints[255u * 16u]{};
        };
''',
'''        struct alignas(256) SkinConstants final
        {
            float Joints[255u * 16u]{};
        };
        static_assert(sizeof(SkinConstants) == 16u * 1024u);
''')
rep('detail/Direct3D12Backend.cpp',
'''        UINT64 ConstantCapacity = 8u * 1024u * 1024u;
        UINT64 ConstantCursor = 0u;
''',
'''        UINT64 ConstantCapacity = 8u * 1024u * 1024u;
        UINT64 ConstantCursor = 0u;
        // One upload per unique palette pointer per encoded frame. Shadow and
        // forward passes share the same CBV address instead of consuming the
        // constant arena twice for the same draw.
        std::unordered_map<const float*, D3D12_GPU_VIRTUAL_ADDRESS>
            SkinPaletteAddresses;
''')
rep('detail/Direct3D12Backend.cpp',
'''            SkinConstants constants{};
            std::copy_n(draw.SkinMatrices,
                static_cast<std::size_t>(draw.SkinJointCount) * 16u,
                constants.Joints);
            return UploadConstants(&constants,sizeof(constants));
''',
'''            if(const auto cached=SkinPaletteAddresses.find(draw.SkinMatrices);
                cached!=SkinPaletteAddresses.end()) return cached->second;
            SkinConstants constants{};
            std::copy_n(draw.SkinMatrices,
                static_cast<std::size_t>(draw.SkinJointCount) * 16u,
                constants.Joints);
            const auto address=UploadConstants(&constants,sizeof(constants));
            SkinPaletteAddresses.emplace(draw.SkinMatrices,address);
            return address;
''')
rep('detail/Direct3D12Backend.cpp',
'''        void EncodeScene(const Direct3D12Frame& frame)
        {
            ConstantCursor=0u;
''',
'''        void EncodeScene(const Direct3D12Frame& frame)
        {
            ConstantCursor=0u;
            SkinPaletteAddresses.clear();
''')

Path('.github/workflows/finalize-d3d12-skinning.yml').unlink(missing_ok=True)
Path('.github/scripts/finalize_d3d12_skinning.py').unlink(missing_ok=True)
