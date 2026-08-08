from pathlib import Path
p = Path('CMakeLists.txt')
s = p.read_text()
s = s.replace('set(KAIRO_RENDERER_MATH_REVISION d77c98d7401f63b697c1486ebf59a1fae74c6490)', 'set(KAIRO_RENDERER_MATH_REVISION e12778734a8e8ffe060255a9c03683ad6c4b1a92)')
s = s.replace('set(KAIRO_RENDERER_ASSETS_REVISION 306a715094ff21707cba209e38eecb282bbb9f1c)', 'set(KAIRO_RENDERER_ASSETS_REVISION a50d801796b69f4ee98f43ecfea0fd6cdd66de85)')
p.write_text(s)
Path('.github/workflows/refresh-pins.yml').unlink()
Path('.github/scripts/refresh_pins.py').unlink()
