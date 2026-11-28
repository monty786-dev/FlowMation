#!/usr/bin/env bash
# Runs FlowBrowserWine.exe under real Wine (+ Wine Gecko) on Linux with a virtual display and lets it execute a tiny flow
# against a local web page. This is the closest thing to Winlator that can run in CI.
#
#   tests/smoke/wine_smoke.sh path/to/FlowBrowserWine.exe [output-folder]
#
# Needs: wine (64-bit), Wine Gecko installed in the prefix, xvfb-run, python3. Exit code 0 = the flow ran and produced
# the expected file. All logs are copied to the output folder (default smoke-out/).
set -u
EXE=$(readlink -f "${1:?usage: wine_smoke.sh FlowBrowserWine.exe [outdir]}")
OUT=$(readlink -f "${2:-smoke-out}")
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT/app"
cp "$EXE" "$OUT/app/FlowBrowserWine.exe"
: > "$OUT/app/portable.txt"                       # portable mode: all data (flows, logs, UI files) goes to app/data
export WINEPREFIX=${WINEPREFIX:-$HOME/.wine-flowbrowser}
export WINEDEBUG=${WINEDEBUG:--all,err+all,fixme+mshtml}
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-winemenubuilder.exe=d}
rm -f /tmp/flowbrowser-smoke.txt /tmp/flowbrowser-smoke.png

echo "== wine: $(wine --version 2>&1)"
python3 -m http.server 8099 --bind 127.0.0.1 --directory "$ROOT/tests/site" >"$OUT/http.log" 2>&1 &
HTTP=$!
trap 'kill $HTTP 2>/dev/null' EXIT

echo "== preparing the Wine prefix (installs Wine Gecko / Mono if they are available)"
xvfb-run -a wineboot --init >"$OUT/wineboot.log" 2>&1
wineserver -w 2>/dev/null

FLOW=$(winepath -w "$HERE/smoke.fbflow" 2>/dev/null)
echo "== running the flow"
timeout 240 xvfb-run -a -s "-screen 0 1280x800x24" wine "$OUT/app/FlowBrowserWine.exe" "$FLOW" --run smoke --exit >"$OUT/wine.log" 2>&1
RC=$?
echo "exit code: $RC"

mkdir -p "$OUT/data"
cp -r "$OUT/app/data/logs" "$OUT/data/" 2>/dev/null
[ -f /tmp/flowbrowser-smoke.png ] && cp /tmp/flowbrowser-smoke.png "$OUT/"
GOT=$(cat /tmp/flowbrowser-smoke.txt 2>/dev/null)
echo "output file: ${GOT:-<missing>}"
echo "--- host.log (last lines)"; tail -n 25 "$OUT/data/logs/host.log" 2>/dev/null || echo "(no host.log: the app never started)"
if [ "$RC" -eq 0 ] && [ "$GOT" = "heading=Sign up;typed=Ana" ]; then
  echo "SMOKE TEST PASSED"; exit 0
fi
echo "SMOKE TEST FAILED (see $OUT/wine.log and $OUT/data/logs/host.log)"
exit 1
