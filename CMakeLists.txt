cmake_minimum_required(VERSION 3.20)
project(mico CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -fno-plt -fno-semantic-interposition")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# One library per layer. Each may use the ones listed before it and none after;
# tools/check_layers.py holds the same order and fails the build's tests when
# an #include reaches upward.
#
#   base      text, JSON, logging: no mico concepts at all
#   vt        the cell grid, the emulator that fills one from an agent's pty,
#             and the key/mouse encoding sent back to it
#   math      equation and image layout and rasterizing (content, not terminal)
#   model     mico's normalized types: Event, SessionState, Launch
#   adapters  the Adapter interface, and one per agent: claude, codex, pi/omp
#   core      sessions, the store, search, activity: everything but drawing
#   api       the state protocol: a client's view of the workspace, as messages
#   term      the host terminal mico draws on: output, input, kitty/sixel
#   tui       views and the app, drawn into cells
#   net       daemon and client
add_library(mico_base STATIC
  src/base/text.cpp
  src/base/uuid.cpp
  src/base/path.cpp
  src/base/json_write.cpp
  src/base/json.cpp
  src/base/jsonl.cpp
  src/base/log.cpp
)
target_include_directories(mico_base PUBLIC src)
# The index passes read files on worker threads.
find_package(Threads REQUIRED)
target_link_libraries(mico_base PUBLIC Threads::Threads)

add_library(mico_vt STATIC
  src/vt/surface.cpp
  src/vt/vt.cpp
  src/vt/keys.cpp
)
target_link_libraries(mico_vt PUBLIC mico_base)

add_library(mico_math STATIC
  src/math/atlas.cpp
  src/math/parse.cpp
  src/math/layout.cpp
  src/math/math.cpp
  src/math/deflate.cpp
  src/math/raster.cpp
  src/math/picture.cpp
)
target_link_libraries(mico_math PUBLIC mico_base)
# The glyph atlas is linked in by an .incbin in atlas.cpp.
set(MICO_MATH_ATLAS ${CMAKE_SOURCE_DIR}/src/math/atlas.bin)
set_source_files_properties(src/math/atlas.cpp PROPERTIES
  COMPILE_DEFINITIONS MICO_MATH_ATLAS="${MICO_MATH_ATLAS}"
  OBJECT_DEPENDS ${MICO_MATH_ATLAS})

add_library(mico_model INTERFACE)
target_link_libraries(mico_model INTERFACE mico_base)

add_library(mico_adapters STATIC
  src/adapters/adapter.cpp
  src/adapters/cmdline.cpp
  src/adapters/registry.cpp
  src/adapters/screen.cpp
  src/adapters/changes.cpp
  src/adapters/tool_calls.cpp
  src/adapters/usage_scan.cpp
  src/adapters/claude/transcript.cpp
  src/adapters/claude/session.cpp
  src/adapters/claude/screen.cpp
  src/adapters/claude/list.cpp
  src/adapters/claude/changes.cpp
  src/adapters/claude/tools.cpp
  src/adapters/claude/usage.cpp
  src/adapters/claude/commands.cpp
  src/adapters/codex/transcript.cpp
  src/adapters/codex/session.cpp
  src/adapters/codex/screen.cpp
  src/adapters/codex/list.cpp
  src/adapters/codex/changes.cpp
  src/adapters/codex/tools.cpp
  src/adapters/codex/usage.cpp
  src/adapters/codex/commands.cpp
  src/adapters/pi/transcript.cpp
  src/adapters/pi/session.cpp
  src/adapters/pi/list.cpp
  src/adapters/pi/changes.cpp
  src/adapters/pi/tools.cpp
  src/adapters/pi/usage.cpp
  src/adapters/pi/commands.cpp
)
target_link_libraries(mico_adapters PUBLIC mico_model mico_vt)

add_library(mico_core STATIC
  src/core/pty.cpp
  src/core/models.cpp
  src/core/clipboard.cpp
  src/core/opener.cpp
  src/core/images.cpp
  src/core/x11_clipboard.cpp
  src/core/session.cpp
  src/core/settings.cpp
  src/core/store.cpp
  src/core/usage.cpp
  src/core/search.cpp
  src/core/activity.cpp
  src/core/edits.cpp
  src/core/commands.cpp
  src/core/files.cpp
  src/core/workspace.cpp
  src/core/conversation.cpp
  src/core/web_access.cpp
)
target_link_libraries(mico_core PUBLIC mico_adapters mico_vt PRIVATE util)

add_library(mico_api STATIC
  src/api/client.cpp
)
target_link_libraries(mico_api PUBLIC mico_core)

add_library(mico_term STATIC
  src/term/term.cpp
  src/term/encoder.cpp
  src/term/input.cpp
  src/term/links.cpp
  src/term/kitty.cpp
  src/term/sixel.cpp
)
target_link_libraries(mico_term PUBLIC mico_vt mico_math)

add_library(mico_tui STATIC
  src/ui/layout.cpp
  src/ui/theme.cpp
  src/ui/app.cpp
  src/ui/picker.cpp
  src/ui/app_flows.cpp
  src/views/project_list.cpp
  src/views/chat_list.cpp
  src/views/chat_view.cpp
  src/views/chat_render.cpp
  src/views/markdown.cpp
  src/views/code.cpp
  src/views/latex.cpp
  src/views/session_pane.cpp
  src/views/chips.cpp
  src/views/usage.cpp
  src/views/chart.cpp
  src/views/chart_image.cpp
  src/views/chart_figure.cpp
  src/views/diagram.cpp
  src/views/progress.cpp
  src/views/json_view.cpp
  src/views/notebook.cpp
  src/views/settings_view.cpp
  src/views/tools.cpp
  src/views/diff.cpp
  src/views/search.cpp
  src/views/completion.cpp
)
target_link_libraries(mico_tui PUBLIC mico_core mico_term)

add_library(mico_net STATIC
  src/net/proto.cpp
  src/net/server.cpp
  src/net/client.cpp
  src/net/web.cpp
)
# The web view's page, script and styles are linked in by .incbin in web.cpp.
set(MICO_WEB_DIR ${CMAKE_SOURCE_DIR}/web)
set_source_files_properties(src/net/web.cpp PROPERTIES
  COMPILE_DEFINITIONS MICO_WEB_DIR="${MICO_WEB_DIR}"
  OBJECT_DEPENDS "${MICO_WEB_DIR}/index.html;${MICO_WEB_DIR}/app.js;${MICO_WEB_DIR}/app.css")
target_link_libraries(mico_net PUBLIC mico_tui mico_api)

add_executable(mico
  src/main.cpp
  src/mcp.cpp
  src/bench.cpp
  src/selftest.cpp
  src/regression_test.cpp
  src/api_test.cpp
)
target_link_libraries(mico PRIVATE mico_net)
# Resolve every symbol at load time: lazy PLT binding showed up at ~10% of
# samples in a profile dominated by short-lived hot loops.
target_link_options(mico PRIVATE -Wl,-z,now -static-libstdc++ -static-libgcc)

# `ctest` runs the self-test and the fixture-driven handoff checks. None of
# them starts a real agent. Each gets its own HOME and XDG dirs, so a local run
# never touches the real daemon or config.
enable_testing()
set(MICO_TEST_HOME ${CMAKE_BINARY_DIR}/test-home)
file(MAKE_DIRECTORY ${MICO_TEST_HOME})
set(MICO_TEST_ENV
  HOME=${MICO_TEST_HOME}
  XDG_CONFIG_HOME=${MICO_TEST_HOME}/config
  XDG_STATE_HOME=${MICO_TEST_HOME}/state
  XDG_RUNTIME_DIR=${MICO_TEST_HOME}
  # The driver checks link a test binary against mico's own objects.
  CXX=${CMAKE_CXX_COMPILER}
  MICO_BUILD=${CMAKE_BINARY_DIR})

add_test(NAME selftest COMMAND mico --selftest)
set_tests_properties(selftest PROPERTIES ENVIRONMENT "${MICO_TEST_ENV}")
add_test(NAME api COMMAND mico --api-test)
set_tests_properties(api PROPERTIES ENVIRONMENT "${MICO_TEST_ENV}")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME layers
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_layers.py ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME daemon-input
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/daemon_input.py $<TARGET_FILE:mico>)
  add_test(NAME web-view
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/web_view.py $<TARGET_FILE:mico>)
  foreach(check claude_questions claude_permission claude_paste)
    string(REPLACE "_" "-" name ${check})
    add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/${check}.py)
  endforeach()
  set_tests_properties(daemon-input web-view claude-questions claude-permission claude-paste
    PROPERTIES ENVIRONMENT "${MICO_TEST_ENV}")
endif()
