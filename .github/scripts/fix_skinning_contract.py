from pathlib import Path

mesh = Path("Mesh.cppm")
text = mesh.read_text()
duplicate = (
    "        [[nodiscard]] std::size_t IndexBytes() const noexcept { return m_Indices.size() * sizeof(std::uint32_t); }\n"
    "        [[nodiscard]] std::size_t IndexBytes() const noexcept { return m_Indices.size() * sizeof(std::uint32_t); }\n"
)
if duplicate not in text:
    raise SystemExit("duplicate IndexBytes marker not found")
text = text.replace(duplicate,
    "        [[nodiscard]] std::size_t IndexBytes() const noexcept { return m_Indices.size() * sizeof(std::uint32_t); }\n", 1)
mesh.write_text(text)

test = Path("tests/SkinningContractTests.cpp")
text = test.read_text()
if "#include <stdexcept>" not in text:
    text = text.replace("#include <limits>\n", "#include <limits>\n#include <stdexcept>\n", 1)
if "import Kairo.Foundation.Math;" not in text:
    text = text.replace("import Kairo.Assets;\n", "import Kairo.Assets;\nimport Kairo.Foundation.Math;\n", 1)
test.write_text(text)

Path(".github/workflows/fix-skinning-contract.yml").unlink(missing_ok=True)
Path(".github/scripts/fix_skinning_contract.py").unlink(missing_ok=True)
