#!/usr/bin/env bash
# Run from a configured KairoRenderer standalone build on a real Mac.
# Unlike CTest's portable suite, a native smoke returning 77 fails here.
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "This gate requires macOS and a usable native Metal device." >&2
    exit 1
fi
if [[ $# -ne 1 || ! -f "$1/CMakeCache.txt" ]]; then
    echo "Usage: bash scripts/require_metal_smoke.sh <configured-renderer-build-dir>" >&2
    exit 2
fi
build="$(cd "$1" && pwd)"
logdir="$build/metal-smoke-diagnostics"
mkdir -p "$logdir"
cmake --build "$build" --parallel "${KAIRO_BUILD_JOBS:-4}" --target \
    KairoRendererMetalSkinningSmoke KairoRendererMetalInstancingSmoke
export MTL_DEBUG_LAYER=1
for target in KairoRendererMetalSkinningSmoke KairoRendererMetalInstancingSmoke; do
    executable="$build/$target"
    if [[ ! -x "$executable" ]]; then
        echo "Missing $executable; use a standalone single-config Ninja build." >&2
        exit 1
    fi
    # The programs explicitly request Metal and require completed readbacks.
    # pipefail preserves the test status even when tee succeeds.
    "$executable" 2>&1 | tee "$logdir/$target.log"
done
echo "Required native Metal smoke tests executed successfully."
