#!/usr/bin/env bash
set -euo pipefail
build="${1:-build}"
donefile="$(mktemp)"
rm -f "$donefile"
log="$build/diagnostics/visible-window.log"
"$build/KairoRendererVulkanPresentationSmoke" "$donefile" >"$log" 2>&1 &
pid=$!
cleanup(){ rm -f "$donefile"; if kill -0 "$pid" 2>/dev/null; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi; cat "$log"; }
trap cleanup EXIT
for i in $(seq 1 100); do
    if grep -q PRESENTATION_READY "$log"; then break; fi
    kill -0 "$pid" || exit 1
    sleep .1
done
grep -q PRESENTATION_READY "$log"
wid="$(xdotool search --onlyvisible --name '^Kairo Vulkan Presentation Smoke$' | head -n 1)"
test -n "$wid"
# Capture the real X11 window, independent of the engine's viewport readback.
import -window "$wid" -resize 160x120\! -depth 8 rgb:"$build/diagnostics/visible-window.rgb"
python3 - "$build/diagnostics/visible-window.rgb" <<'PY'
import pathlib,sys
data=pathlib.Path(sys.argv[1]).read_bytes()
assert len(data)==160*120*3, len(data)
for x,y,expected in [(20,20,(0,255,0)),(140,20,(255,0,0)),(20,100,(255,0,0)),(140,100,(0,0,255))]:
    p=(y*160+x)*3
    actual=tuple(data[p:p+3])
    assert all(abs(a-b)<=5 for a,b in zip(actual,expected)),(x,y,actual,expected)
print("Visible Vulkan presentation passed: top-left, top-right, bottom-left, bottom-right and resized descriptor")
PY
touch "$donefile"
wait "$pid"
