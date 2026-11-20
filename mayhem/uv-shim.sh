#!/usr/bin/env bash
# Stand-in for `uv` (not in the image) used only by tests/CMakeLists.txt's
# fixture rule: `uv run --quiet make_gguf_fixtures.py`. The generator is
# stdlib-only Python, so run it with the image's python3.
set -euo pipefail
[ "${1:-}" = run ] || { echo "uv-shim: only 'run' is supported" >&2; exit 2; }
shift
while [ "${1:-}" = --quiet ]; do shift; done
exec python3 -I "$@"
