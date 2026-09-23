#!/usr/bin/env bash
# Run clang-tidy (.clang-tidy at the repo root) over the mod's own sources.
# Uses the NDK's clang-tidy so it matches the compiler in compile_commands.json.
#   scripts/tidy.sh                 all sources
#   scripts/tidy.sh touch           only files matching the regex "touch"
#   scripts/tidy.sh '' -fix         extra args go to run-clang-tidy
# Needs a configured build dir (default: build, override with BUILD_DIR).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${BUILD_DIR:-$root/build}"
sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"

tidy="$(ls -d "$sdk"/ndk/*/toolchains/llvm/prebuilt/*/bin/clang-tidy 2>/dev/null | sort -V | tail -1)"
[[ -x "$tidy" ]] || { echo "NDK clang-tidy not found under $sdk/ndk" >&2; exit 1; }

runner="$(command -v run-clang-tidy || true)"
if [[ -z "$runner" ]] && command -v brew >/dev/null; then
    runner="$(brew --prefix llvm)/bin/run-clang-tidy"
fi
[[ -x "$runner" ]] || { echo "run-clang-tidy not found (brew install llvm)" >&2; exit 1; }

[[ -f "$build/compile_commands.json" ]] || { echo "no compile_commands.json in $build" >&2; exit 1; }

filter="${1:-}"
shift || true
exec "$runner" -quiet -p "$build" -clang-tidy-binary "$tidy" "$@" \
    "$root/src/.*${filter}.*\.cpp$"
