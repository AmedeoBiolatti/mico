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

add_executable(mico
  src/main.cpp
  src/mcp.cpp
  src/bench.cpp
  src/selftest.cpp
  src/regression_test.cpp
  src/term/surface.cpp
  src/term/links.cpp
  src/term/text.cpp
  src/term/input.cpp
  src/term/encoder.cpp
  src/term/vt.cpp
  src/term/keys.cpp
  src/core/pty.cpp
  src/core/models.cpp
  src/core/clipboard.cpp
  src/core/opener.cpp
  src/core/images.cpp
  src/core/x11_clipboard.cpp
  src/core/log.cpp
  src/core/session.cpp
  src/core/settings.cpp
  src/net/proto.cpp
  src/net/server.cpp
  src/net/client.cpp
  src/term/term.cpp
  src/core/json.cpp
  src/core/jsonl.cpp
  src/core/claude.cpp
  src/core/codex.cpp
  src/core/store.cpp
  src/core/usage.cpp
  src/core/search.cpp
  src/core/activity.cpp
  src/core/edits.cpp
  src/core/commands.cpp
  src/core/files.cpp
  src/ui/layout.cpp
  src/ui/app.cpp
  src/ui/picker.cpp
  src/ui/app_flows.cpp
  src/views/project_list.cpp
  src/views/chat_list.cpp
  src/views/chat_view.cpp
  src/core/pi.cpp
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
  src/math/atlas.cpp
  src/math/parse.cpp
  src/math/layout.cpp
  src/math/math.cpp
  src/math/kitty.cpp
  src/math/deflate.cpp
  src/math/sixel.cpp
  src/math/raster.cpp
  src/math/picture.cpp
)
# The glyph atlas is linked in by an .incbin in atlas.cpp.
set(MICO_MATH_ATLAS ${CMAKE_SOURCE_DIR}/src/math/atlas.bin)
set_source_files_properties(src/math/atlas.cpp PROPERTIES
  COMPILE_DEFINITIONS MICO_MATH_ATLAS="${MICO_MATH_ATLAS}"
  OBJECT_DEPENDS ${MICO_MATH_ATLAS})
target_include_directories(mico PRIVATE src)
target_link_libraries(mico PRIVATE util)
# Resolve every symbol at load time: lazy PLT binding showed up at ~10% of
# samples in a profile dominated by short-lived hot loops.
target_link_options(mico PRIVATE -Wl,-z,now -static-libstdc++ -static-libgcc)
target_compile_options(mico PRIVATE -Wall -Wextra -Wno-unused-parameter)

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

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME daemon-input
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/daemon_input.py $<TARGET_FILE:mico>)
  foreach(check claude_questions claude_permission claude_paste)
    string(REPLACE "_" "-" name ${check})
    add_test(NAME ${name} COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/${check}.py)
  endforeach()
  set_tests_properties(daemon-input claude-questions claude-permission claude-paste
    PROPERTIES ENVIRONMENT "${MICO_TEST_ENV}")
endif()
