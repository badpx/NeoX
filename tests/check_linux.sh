#!/usr/bin/env bash
# Real Linux platform acceptance. Requires compiler, CMake, Mesa/EGL, Xvfb,
# Weston and DejaVu fonts; never installs packages or modifies the host.
set -euo pipefail
[[ $(uname -s) == Linux ]] || { echo 'A real Linux runtime is required' >&2; exit 1; }
source_dir=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?usage: check_linux.sh OUTPUT_DIRECTORY}
mkdir -p "$work"
work=$(cd "$work" && pwd)
build="$work/full"
cmake -S "$source_dir" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
  -DNEOX_BACKEND_GL=ON -DNEOX_BACKEND_METAL=OFF -DNEOX_PRESENT=ON \
  -DNEOX_SANITIZE=ON -DNEOX_WARNINGS_AS_ERRORS=ON
cmake --build "$build" --parallel 4
export NEOX_REQUIRE_GL=1
# Window cases are run under both display protocols below, never counted as skips.
ctest --test-dir "$build" --output-on-failure -E '^(present|ex05_smoke)$'
export NEOX_REQUIRE_PRESENT=1 NEOX_PRESENT_FRAMES=2
for gpu in 0 1; do
  echo "X11 acceptance, example GPU=$gpu"
  NEOX_PRESENT_BACKEND=x11 NEOX_PRESENT_GPU=$gpu \
    xvfb-run -a -s '-screen 0 1280x1024x24' \
    ctest --test-dir "$build" --output-on-failure -R '^(present|ex05_smoke)$'
done
runtime=$(mktemp -d "$work/wayland.XXXXXX")
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=neox-test
weston --backend=headless --socket=neox-test --width=1280 --height=1024 > "$runtime/weston.log" 2>&1 &
compositor=$!
trap 'kill "$compositor" 2>/dev/null || true' EXIT
for ((i=0;i<100;i++)); do
  [[ -S "$runtime/neox-test" ]] && break
  kill -0 "$compositor" || { cat "$runtime/weston.log"; exit 1; }
  sleep .1
done
[[ -S "$runtime/neox-test" ]]
for gpu in 0 1; do
  echo "Wayland acceptance, example GPU=$gpu"
  NEOX_PRESENT_BACKEND=wayland NEOX_PRESENT_GPU=$gpu \
    ctest --test-dir "$build" --output-on-failure -R '^(present|ex05_smoke)$'
done
unset NEOX_REQUIRE_PRESENT WAYLAND_DISPLAY XDG_RUNTIME_DIR
# Backend/Text/Present configuration matrix; Scene/Capture combinations and
# installation/subdirectory consumption are covered by the separate script.
for gl in OFF ON; do
  if [[ $gl == ON ]]; then export NEOX_REQUIRE_GL=1; else unset NEOX_REQUIRE_GL; fi
  for text in OFF ON; do
    for present in OFF ON; do
      b="$work/gl-$gl-text-$text-present-$present"
      cmake -S "$source_dir" -B "$b" -DCMAKE_BUILD_TYPE=Debug \
        -DNEOX_BACKEND_GL="$gl" -DNEOX_BACKEND_METAL=OFF -DNEOX_WITH_TEXT="$text" \
        -DNEOX_PRESENT="$present" -DNEOX_SANITIZE=ON -DNEOX_WARNINGS_AS_ERRORS=ON
      cmake --build "$b" --parallel 4
      ctest --test-dir "$b" --output-on-failure -E '^(present|ex05_smoke)$'
    done
  done
done
unset NEOX_REQUIRE_GL
python3 "$source_dir/tests/check_modules.py" --workdir "$work/modules"
echo 'PASS: Linux Soft/EGL/X11/Wayland, backend matrix and module consumers'
