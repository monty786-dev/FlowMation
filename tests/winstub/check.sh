#!/bin/sh
# Syntax-checks the Windows sources with clang against stand-in headers (Linux). Catches typos, undeclared names,
# wrong core-API usage and bad struct fields; it does not replace the real MSVC build.
cd "$(dirname "$0")/../.." || exit 1
python3 tests/winstub/gen.py >/dev/null || exit 1
rc=0
for f in src/win/win_util.c src/win/win_main.c src/win/win_cmd.c src/win/win_engine.c src/core/fb_util.c; do
  clang-18 -fsyntax-only -std=c11 -D_WIN32 -DUNICODE -D_UNICODE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -Wall -Wextra -Wno-deprecated-non-prototype -Wno-unused-parameter -Wno-unused-function -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-incompatible-pointer-types \
    -Itests/winstub -Isrc/core -Isrc/win "$f" || rc=1
done
[ $rc -eq 0 ] && echo "winstub check: OK"
exit $rc
