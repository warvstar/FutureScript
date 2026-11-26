#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
grammar=$PWD
repo=$(git rev-parse --show-toplevel)
cli=$grammar/node_modules/.bin/tree-sitter
if [[ ! -x "$cli" ]]; then
  if ! cli=$(command -v tree-sitter); then
    echo "Install the Tree-sitter CLI with npm ci in $grammar." >&2
    exit 1
  fi
fi

scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
printf '{"parser-directories":[".."]}\n' > "$scratch/config.json"
git -C "$repo" ls-files -z --cached --others --exclude-standard -- '*.fut' > "$scratch/paths"

clean=0
expected=0
failed=0
while IFS= read -r -d '' file; do
  [[ -f "$repo/$file" ]] || continue
  reason=
  case "$file" in
    tests/err_brace.fut) reason="unclosed string interpolation" ;;
    tests/err_char.fut) reason="non-ASCII character outside a string or comment" ;;
  esac

  if output=$("$cli" parse --quiet --config-path "$scratch/config.json" "$repo/$file" 2>&1); then
    if [[ -n "$reason" ]]; then
      printf '%s: expected a parse failure (%s)\n' "$file" "$reason" >&2
      ((failed += 1))
    else
      ((clean += 1))
    fi
  elif [[ -n "$reason" && ( "$output" == *'(ERROR '* || "$output" == *'(MISSING '* ) ]]; then
    printf '%s: expected parse failure (%s)\n' "$file" "$reason"
    ((expected += 1))
  else
    printf '%s\n' "$output" >&2
    ((failed += 1))
  fi
done < "$scratch/paths"

printf '%d files checked, %d parsed cleanly, %d expected parse failures, %d unexpected failures\n' \
  "$((clean + expected + failed))" "$clean" "$expected" "$failed"
((failed == 0 && clean > 0))
