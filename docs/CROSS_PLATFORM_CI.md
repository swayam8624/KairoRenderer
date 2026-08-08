# Cross-platform CI

KairoRenderer is validated independently on Linux/Clang, macOS/LLVM, and Windows/MSVC. Fixed release fallbacks use immutable commit SHAs and avoid shallow-clone assumptions. During the stacked Phase 1–5 review, the Assets fallback follows its named integration branch so an unmerged commit can be fetched by downstream CI; it is converted back to the merged immutable SHA before release.
