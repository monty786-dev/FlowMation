#!/bin/sh
# Builds the portable core + test programs with gcc/clang and runs every test that does not need Windows.
#   tests/run_all.sh            C tests, flow-engine tests, data-flow tests (needs python3)
#   tests/run_all.sh --web      also the browser tests (needs node + playwright + Chromium: npm i playwright)
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/linux}
mkdir -p "$OUT"
CC=${CC:-cc}
CORE="src/core/fb_expr.c src/core/fb_flow.c src/core/fb_json.c src/core/fb_legacy.c src/core/fb_regex.c src/core/fb_runner.c src/core/fb_runner_ctl.c src/core/fb_runner_data.c src/core/fb_runner_web.c src/core/fb_util.c src/core/fb_xlsx.c src/core/fb_zip.c"
CFLAGS="-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc/core"
LIBS="-lm"
echo "== building"
$CC $CFLAGS -o "$OUT/fbrun" tools/fbrun.c $CORE $LIBS
$CC $CFLAGS -o "$OUT/test_legacy" tests/test_legacy.c src/core/fb_legacy.c src/core/fb_json.c src/core/fb_util.c $LIBS
$CC $CFLAGS -o "$OUT/test_png" tests/test_png.c src/core/fb_png.c src/core/fb_util.c $LIBS
for t in test_core test_expr test_regex test_flow; do
  $CC $CFLAGS -o "$OUT/$t" "tests/$t.c" $CORE $LIBS 2>/dev/null || $CC $CFLAGS -o "$OUT/$t" "tests/$t.c" $(echo $CORE | tr ' ' '\n' | grep -v fb_runner | tr '\n' ' ') $LIBS
done
echo "== C tests"
run() { name=$1; shift; "$@" > "$OUT/$name.log" 2>&1 || { cat "$OUT/$name.log"; echo "FAILED: $name"; exit 1; }; tail -2 "$OUT/$name.log"; }
for t in test_core test_expr test_regex; do echo "-- $t"; run $t "$OUT/$t"; done
echo "-- test_flow"; run test_flow "$OUT/test_flow" src/core/catalog.json
echo "-- test_legacy (Wine / Gecko engine bridge)"; run test_legacy "$OUT/test_legacy"
echo "-- test_png (screenshots in the Wine build)"; run test_png sh -c "$OUT/test_png $OUT/shot.png && python3 tests/test_png.py $OUT/shot.png"
echo "== data flow (Excel / CSV / JSON through the runner)"
run data_flow python3 tests/test_data_flow.py "$OUT/fbrun"
if [ "$1" = "--web" ]; then
  export FBRUN="$PWD/$OUT/fbrun"
  echo "== scripts stay inside ES5 (the Wine / Gecko build runs them in JScript)"; run lint node tests/lint_es5.js
  echo "== start-up guard for the Wine / Gecko UI (white screen -> visible message)"; run boot node tests/test_boot.js
  echo "== page tools"; run tools node tests/test_tools.js
  echo "== web flows (real Chromium over CDP)"; run web_flow node tests/test_web_flow.js
  echo "== UI (mock host)"; run ui node tests/test_ui.js
  echo "== the same three in an old engine (FB_LEGACY=1: no DevTools input, no modern APIs; fbrun --legacy)"
  echo "-- page tools (legacy)"; run tools_legacy env FB_LEGACY=1 node tests/test_tools.js
  echo "-- web flows (legacy)"; run web_flow_legacy env FB_LEGACY=1 node tests/test_web_flow.js
  echo "-- UI (legacy)"; run ui_legacy env FB_LEGACY=1 node tests/test_ui.js
fi
echo "ALL OK"
