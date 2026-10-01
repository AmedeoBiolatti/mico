#!/usr/bin/env bash
# Packages mico needs to build and test on Ubuntu 24.04, plus any extra
# packages passed as arguments. X11 is loaded at run time, so no -dev packages.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends ninja-build python3 "$@"
