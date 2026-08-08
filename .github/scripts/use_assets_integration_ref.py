from pathlib import Path
p=Path('CMakeLists.txt')
s=p.read_text().replace('set(KAIRO_RENDERER_ASSETS_REVISION ad3d8e80f2bab0cab751817ecce82a0c972e8d46)','set(KAIRO_RENDERER_ASSETS_REVISION agent/phase1-5-portability-hardening)')
p.write_text(s)
Path('.github/workflows/use-assets-integration-ref.yml').unlink()
Path('.github/scripts/use_assets_integration_ref.py').unlink()
