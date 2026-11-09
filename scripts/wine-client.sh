#!/usr/bin/env bash
set -euo pipefail
: "${ODL_CLIENT_EXE:?Set ODL_CLIENT_EXE to open-dll-loader.exe}"
exec wine "$ODL_CLIENT_EXE" "$@"
