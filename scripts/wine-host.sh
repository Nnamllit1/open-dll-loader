#!/usr/bin/env bash
set -euo pipefail
: "${ODL_HOST_EXE:?Set ODL_HOST_EXE to the Windows example host executable}"
if [[ $# -lt 2 || $1 != --plugin ]]; then
  echo 'Expected: wine-host.sh --plugin FILE [--watch]' >&2
  exit 2
fi
plugin_path=$(winepath -w "$2")
shift 2
exec wine "$ODL_HOST_EXE" --plugin "$plugin_path" "$@"
