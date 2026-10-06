#!/usr/bin/env bash
# Builds mico from this checkout and installs it, by default to ~/.local/bin.
#
#   ./install.sh                 build, install to ~/.local
#   ./install.sh --prefix DIR    install to DIR/bin instead
#   ./install.sh --test          run the tests before installing
#
# To update: git pull && ./install.sh. It never uses sudo and never stops a
# running mico daemon; it says when one should be restarted.
set -euo pipefail

prefix="$HOME/.local"
run_tests=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) prefix="${2:?--prefix needs a directory}"; shift 2 ;;
    --prefix=*) prefix="${1#--prefix=}"; shift ;;
    --test) run_tests=1; shift ;;
    -h|--help) sed -n '2,9s/^# \{0,1\}//p' "$0"; exit 0 ;;
    *) echo "install.sh: unknown option $1 (try --help)" >&2; exit 2 ;;
  esac
done
prefix=$(realpath -m -- "$prefix")  # before the cd below, which would move a relative one

cd "$(dirname "$0")"

missing=()
command -v cmake >/dev/null || missing+=(cmake)
command -v "${CXX:-c++}" >/dev/null || missing+=("a C++ compiler")
if [ ${#missing[@]} -gt 0 ]; then
  echo "install.sh: missing ${missing[*]}." >&2
  echo "  On Ubuntu or Debian: sudo apt install build-essential cmake ninja-build" >&2
  exit 1
fi

# C++23 needs GCC 13 or Clang 18. When the default compiler is older, use a
# newer GCC if one is installed beside it, as g++-13, g++-14 or g++-15.
if [ -z "${CXX:-}" ]; then
  ver=$(c++ -dumpfullversion -dumpversion 2>/dev/null || echo 0)
  if c++ --version 2>/dev/null | grep -qi clang; then need=18; else need=13; fi
  if [ "${ver%%.*}" -lt "$need" ]; then
    for v in 15 14 13; do
      if command -v "g++-$v" >/dev/null; then export CXX="g++-$v"; break; fi
    done
    if [ -z "${CXX:-}" ]; then
      echo "install.sh: c++ is version $ver; mico needs GCC 13 or Clang 18." >&2
      echo "  Install GCC 13 or newer (as g++-13 beside it is fine), or use Ubuntu 24.04 or later." >&2
      exit 1
    fi
    echo "Using $CXX (c++ is $ver)."
  fi
fi

# A build folder already configured keeps its generator; a new one uses Ninja
# when it is there.
configure=(cmake -S . -B build -DCMAKE_BUILD_TYPE=Release)
if [ ! -f build/CMakeCache.txt ] && command -v ninja >/dev/null; then configure+=(-G Ninja); fi
"${configure[@]}"
cmake --build build --parallel "$(nproc 2>/dev/null || echo 4)"
if [ "$run_tests" = 1 ]; then ctest --test-dir build --output-on-failure -j 4 --timeout 300; fi
cmake --install build --prefix "$prefix"

bin="$prefix/bin"
echo
echo "Installed $bin/mico"
case ":$PATH:" in
  *":$bin:"*) ;;
  *) echo "  $bin is not on your PATH; add it, e.g.: echo 'export PATH=\"$bin:\$PATH\"' >> ~/.bashrc" ;;
esac
# The daemon's socket, where src/net/proto.cpp puts it.
sock="${XDG_RUNTIME_DIR:-}/mico/default.sock"
[ -n "${XDG_RUNTIME_DIR:-}" ] || sock="/tmp/mico-$(id -u)/default.sock"
if [ -S "$sock" ]; then
  echo "  A mico daemon is running, from the copy it started with. To switch to this one,"
  echo "  run 'mico kill' when you are ready: the next 'mico' starts it, and resumes your agents."
fi
