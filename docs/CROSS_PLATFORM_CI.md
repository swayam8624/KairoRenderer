# Cross-platform CI

KairoRenderer is validated independently on Linux/Clang, macOS/LLVM, and Windows/MSVC. Standalone fallback dependencies are pinned to immutable commit SHAs and fetched without shallow-clone assumptions so PR heads and historical release commits remain directly buildable.
