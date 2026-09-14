#!/bin/sh
# Deletes ALL GitHub Actions run history for this repo (logs + artifacts).
# Irreversible. Loops because gh run list is capped per call.
set -eu
while :; do
  ids=$(gh run list --limit 100 --json databaseId -q '.[].databaseId')
  [ -z "$ids" ] && break
  echo "$ids" | xargs -n1 gh run delete
done
echo "done"
