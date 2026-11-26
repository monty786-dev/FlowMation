cmake_minimum_required(VERSION 3.21)
project(FlowBrowser LANGUAGES C RC)

if(NOT WIN32)
  message(FATAL_ERROR "FlowBrowser is a Windows application (WebView2). On Linux/macOS you can only build and test the portable core: see tests/run_all.sh")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# The WebView2 static loader is built with the static CRT (/MT), so the app uses it too: no Visual C++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- WebView2 SDK (NuGet package Microsoft.Web.WebView2), fetched by tools/fetch-webview2.ps1 ----
set(WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/third_party/webview2" CACHE PATH "Folder with the extracted Microsoft.Web.WebView2 NuGet package")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
  message(FATAL_ERROR "WebView2 SDK not found in ${WEBVIEW2_DIR}.\nRun  powershell -File tools/fetch-webview2.ps1  first (or pass -DWEBVIEW2_DIR=<folder>).")
endif()
if(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
  set(WV2_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WV2_ARCH x64)
else()
  set(WV2_ARCH x86)
endif()
set(WV2_LIB "${WEBVIEW2_DIR}/build/native/${WV2_ARCH}/WebView2LoaderStatic.lib")
if(NOT EXISTS "${WV2_LIB}")
  message(FATAL_ERROR "Missing ${WV2_LIB}")
endif()

# ---- portable core (no Windows APIs beyond file/time helpers in fb_util.c) ----
set(CORE_SRC
  src/core/fb_expr.c src/core/fb_flow.c src/core/fb_json.c src/core/fb_regex.c
  src/core/fb_runner.c src/core/fb_runner_ctl.c src/core/fb_runner_data.c src/core/fb_runner_web.c
  src/core/fb_util.c src/core/fb_xlsx.c src/core/fb_zip.c)

# ---- the UI and page scripts are compiled into the .exe by tools/embed.c ----
add_executable(fb_embed tools/embed.c)
file(GLOB UI_FILES CONFIGURE_DEPENDS src/ui/*.html src/ui/*.css src/ui/*.js)
set(EMBED_ARGS "")
set(EMBED_DEPS "")
foreach(f ${UI_FILES})
  get_filename_component(n "${f}" NAME)
  list(APPEND EMBED_ARGS "ui/${n}=${f}")
  list(APPEND EMBED_DEPS "${f}")
endforeach()
list(APPEND EMBED_ARGS
  "ui/catalog.json=${CMAKE_SOURCE_DIR}/src/core/catalog.json"
  "js/fb_runtime.js=${CMAKE_SOURCE_DIR}/src/js/fb_runtime.js"
  "js/fb_tools.js=${CMAKE_SOURCE_DIR}/src/js/fb_tools.js")
list(APPEND EMBED_DEPS "${CMAKE_SOURCE_DIR}/src/core/catalog.json" "${CMAKE_SOURCE_DIR}/src/js/fb_runtime.js" "${CMAKE_SOURCE_DIR}/src/js/fb_tools.js")
set(UI_DATA_C "${CMAKE_BINARY_DIR}/ui_data.c")
add_custom_command(OUTPUT "${UI_DATA_C}"
  COMMAND fb_embed "${UI_DATA_C}" ${EMBED_ARGS}
  DEPENDS fb_embed ${EMBED_DEPS}
  COMMENT "Embedding the UI into the executable"
  VERBATIM)

add_executable(FlowBrowser WIN32
  src/win/win_main.c src/win/win_cmd.c src/win/win_engine.c src/win/win_util.c
  ${CORE_SRC} "${UI_DATA_C}" src/win/app.rc)

target_include_directories(FlowBrowser PRIVATE src/core src/win "${WEBVIEW2_DIR}/build/native/include")
target_compile_definitions(FlowBrowser PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(FlowBrowser PRIVATE "${WV2_LIB}" ole32 oleaut32 shell32 shlwapi comdlg32 user32 gdi32 advapi32 bcrypt version uuid)
if(MSVC)
  target_compile_options(FlowBrowser PRIVATE /utf-8 /W3 /GS /guard:cf $<$<CONFIG:Release>:/O2>)
  # the manifest (DPI awareness, UTF-8 code page, long paths) is embedded by app.rc
  target_link_options(FlowBrowser PRIVATE /MANIFEST:NO /DYNAMICBASE /NXCOMPAT /guard:cf)
endif()
set_target_properties(FlowBrowser PROPERTIES OUTPUT_NAME "FlowBrowser")

install(TARGETS FlowBrowser RUNTIME DESTINATION .)
