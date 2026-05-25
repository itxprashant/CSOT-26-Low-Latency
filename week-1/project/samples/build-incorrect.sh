#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/samples/incorrect_strategy.so"

g++ -std=c++20 -O2 -shared -fPIC -Wall -Wextra -Wpedantic \
  -I"${ROOT}/include" \
  -o "${OUT}" \
  "${ROOT}/samples/incorrect_strategy.cpp"

echo "Built ${OUT}"
file "${OUT}"
nm -D "${OUT}" | grep -F create_strategy || {
  echo "ERROR: create_strategy not exported" >&2
  exit 1
}
