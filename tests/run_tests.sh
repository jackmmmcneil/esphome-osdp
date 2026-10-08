#!/usr/bin/env bash
# Host test suite for esphome-osdp. Runs on Linux with g++, python3, git and
# OpenSSL dev headers (Debian/Ubuntu: build-essential libssl-dev python3-venv).
#
#   tests/run_tests.sh            unit tests + example config validation
#   tests/run_tests.sh --e2e      also build the door example for ESPHome's host
#                                 platform and drive it over the native API
#
# Everything is built under tests/.build (git-ignored).
set -euo pipefail

ESPHOME_VERSION="${ESPHOME_VERSION:-2026.9.1}"
ARDUINOJSON_TAG="v7.4.2"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/tests/.build"
E2E=0
[[ "${1:-}" == "--e2e" ]] && E2E=1

mkdir -p "$BUILD"
cd "$BUILD"

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- setup ---
step "ESPHome $ESPHOME_VERSION"
if [[ ! -x venv/bin/esphome ]] || ! venv/bin/esphome version | grep -q "$ESPHOME_VERSION"; then
  python3 -m venv venv
  venv/bin/pip install -q -U pip setuptools wheel
  venv/bin/pip install -q "esphome==$ESPHOME_VERSION"
fi
ESP="$(venv/bin/python -c 'import esphome, os; print(os.path.dirname(esphome.__file__))')"
venv/bin/esphome version

if [[ ! -d ArduinoJson ]]; then
  git clone -q --depth 1 -b "$ARDUINOJSON_TAG" https://github.com/bblanchon/ArduinoJson
fi

# Example configs point at the GitHub repo; test them against this checkout.
step "Validate example configs"
rm -rf cfg && mkdir cfg
cp "$ROOT/examples/secrets.yaml.example" cfg/secrets.yaml
API_KEY="$(python3 -c 'import os,base64;print(base64.b64encode(os.urandom(32)).decode())')"
sed -i "s#^api_key: .*#api_key: \"$API_KEY\"#" cfg/secrets.yaml
for f in "$ROOT"/examples/*.yaml; do
  python3 - "$f" "cfg/$(basename "$f")" "$ROOT/components" <<'EOF'
import re, sys
src, dst, comp = sys.argv[1:]
s = open(src).read()
s = re.sub(r"  - source: github://[^\n]+\n",
           f"  - source:\n      type: local\n      path: {comp}\n", s)
open(dst, "w").write(s)
EOF
  echo "-- $(basename "$f")"
  venv/bin/esphome config "cfg/$(basename "$f")" > cfg/out.txt 2>&1 || { cat cfg/out.txt; exit 1; }
  grep -q "Configuration is valid" cfg/out.txt
done

# ------------------------------------------------------ unit test build ---
step "Build host test tree"
rm -rf src obj && mkdir -p src/esphome/components obj
cp -r "$ESP/core" src/esphome/core
for c in host uart binary_sensor text_sensor key_provider sha256 json; do
  mkdir -p "src/esphome/components/$c"
  cp "$ESP/components/$c"/*.h "$ESP/components/$c"/*.cpp "src/esphome/components/$c"/ 2>/dev/null || true
done
cp -r "$ROOT/components/osdp_cp" "$ROOT/components/door_access" src/esphome/components/
cat > src/esphome/core/defines.h <<'EOF'
#pragma once
#include "esphome/core/macros.h"
#define ESPHOME_BOARD "host"
#define ESPHOME_VARIANT "HOST"
#define ESPHOME_COMPONENT_COUNT 4
#define ESPHOME_ENTITY_BINARY_SENSOR_COUNT 2
#define ESPHOME_ENTITY_TEXT_SENSOR_COUNT 1
#define USE_BINARY_SENSOR
#define USE_TEXT_SENSOR
#define USE_SHA256
#define USE_JSON
#define USE_NATIVE_64BIT_TIME
#define USE_ESPHOME_HOST_MAC_ADDRESS {0x02, 0, 0, 0, 0, 1}
EOF

CXXFLAGS=(-std=gnu++20 -O1 -DUSE_HOST -I src -I src/esphome/components/osdp_cp -I ArduinoJson/src)
compile() {  # file -> obj/<flattened>.o
  local f="$1" o="obj/$(echo "$1" | tr / _).o"
  if [[ "$f" == *.c ]]; then
    gcc -c -O1 -std=gnu17 "$f" -o "$o"
  else
    g++ -c "${CXXFLAGS[@]}" "$f" -o "$o"
  fi
}
for f in src/esphome/components/osdp_cp/*.c; do compile "$f"; done
for f in src/esphome/core/*.cpp src/esphome/components/{host,binary_sensor,text_sensor,key_provider,sha256,json}/*.cpp \
         src/esphome/components/uart/uart.cpp src/esphome/components/uart/uart_component.cpp \
         src/esphome/components/osdp_cp/osdp_cp.cpp src/esphome/components/door_access/door_access.cpp; do
  compile "$f"
done
compile "$ROOT/tests/host_stubs.cpp"
# ESPHome's host core.cpp defines main(); the tests bring their own.
objcopy --redefine-sym main=esphome_host_main "obj/src_esphome_components_host_core.cpp.o"

LIBS=(-lpthread -lcrypto)
for t in test_osdp_loopback test_door_access; do
  g++ "${CXXFLAGS[@]}" "$ROOT/tests/$t.cpp" obj/*.o -o "$t" "${LIBS[@]}"
done

step "Reader driver: CP <-> simulated reader (plaintext, Secure Channel, scan)"
./test_osdp_loopback | grep -E "===|PASS|FAIL"
step "Access logic"
./test_door_access | grep -vE "^\[" | grep -E "==|FAIL|PASS"

[[ $E2E -eq 1 ]] || { step "All tests passed"; exit 0; }

# ------------------------------------------------------------------ e2e ---
step "E2E: build examples/door.yaml for the host platform"
rm -rf e2e && mkdir e2e
cp cfg/secrets.yaml e2e/
python3 - cfg/door.yaml e2e/door.yaml <<'EOF'
import re, sys
s = open(sys.argv[1]).read()
s = s.replace("esp32:\n  board: esp32dev\n  framework:\n    type: esp-idf\n", "host:\n")
s = re.sub(r"wifi:\n(?:  .*\n)+", "", s)
s = s.replace("captive_portal:\n", "")
s = re.sub(r"ota:\n(?:  .*\n)+", "", s)
s = re.sub(r"api:\n  encryption:\n    key: [^\n]+\n", "api:\n", s)
s = re.sub(r"  tx_pin: GPIO17\n  rx_pin: GPIO16\n", "", s)
s = re.sub(r"  flow_control_pin: GPIO4\n", "  port: /dev/null\n", s)
s = re.sub(r"    pin: GPIO26[^\n]*\n", "    pin: 26\n", s)
open(sys.argv[2], "w").write(s)
EOF
venv/bin/esphome compile --only-generate e2e/door.yaml > e2e/gen.txt 2>&1 || { cat e2e/gen.txt; exit 1; }
B=e2e/.esphome/build/front-door
mkdir -p "$B/obj"
while IFS= read -r f; do
  o="$B/obj/$(echo "$f" | tr / _).o"
  if [[ "$f" == *.c ]]; then gcc -c -O0 -std=gnu17 -I "$B/src" "$f" -o "$o"
  else g++ -c -O0 -std=gnu++20 -DUSE_HOST -I "$B/src" -I ArduinoJson/src "$f" -o "$o"; fi
done < <(find "$B/src" -name '*.cpp' -o -name '*.c')
g++ "$B"/obj/*.o -o e2e/door-host "${LIBS[@]}"

step "E2E: drive the firmware over the ESPHome native API"
mkdir -p e2e/prefs
ESPHOME_PREFDIR="$BUILD/e2e/prefs" ./e2e/door-host > e2e/door.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 3
venv/bin/python "$ROOT/tests/api_e2e.py"
step "All tests passed"
