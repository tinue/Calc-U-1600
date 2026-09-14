#!/usr/bin/env bash
# Publishes a new Qt6-prototype build to the sibling Calc-U-1600-Binary
# repo: downloads the four platform artifacts from the GitHub Actions run
# for the current HEAD commit, packages them into the same
# Calc-U-1600-Qt6-<platform> names the README's download table already
# uses (.zip for Linux/Windows, signed+notarized .dmg for macOS), copies a
# curated set of example presets into Calc-U-1600-Binary/examples/, pauses
# so you can hand-edit Calc-U-1600-Binary/README.md (this script never
# edits it for you), then commits, pushes, and creates a GitHub Release
# with the packaged files attached.
#
# Binaries are NEVER committed to git -- only ever uploaded as release
# assets, matching Calc-U-1600-Binary's existing history (only README.md
# and examples/ have ever been committed there).
#
# Usage:
#   tools/publish_binary_release.sh --check
#   tools/publish_binary_release.sh [--notes "text"] [preset-file ...]
#
#   --check   Read-only status report: is the examples/ folder and/or the
#             latest release stale relative to the current HEAD commit?
#             Does not download artifacts, touch git, or create a release.
#   --notes   Human clause for the release title, e.g. "rendering fixes"
#             -> "Qt6 prototype (2026-09-12, rendering fixes)". Defaults to
#             the HEAD commit's subject line.
#   preset-file...  Override the default PRESETS list below (paths
#             relative to the repo root, e.g. examples/foo.pc1600).
#
# Env vars:
#   BINARY_REPO   path to the Calc-U-1600-Binary checkout (default: ../Calc-U-1600-Binary)
set -euo pipefail

cd "$(dirname "$0")/.."
REPO_ROOT=$(pwd)
BINARY_REPO=$(cd "${BINARY_REPO:-../Calc-U-1600-Binary}" && pwd)
SOURCE_REPO_SLUG="tinue/Calc-U-1600"
BINARY_REPO_SLUG="tinue/Calc-U-1600-Binary"
WORKFLOW="build.yml"

# Default set of example presets to mirror into Calc-U-1600-Binary/examples/.
# Add more here as needed -- each entry is a path (relative to the repo
# root) to a .pc1500/.pc1500a/.pc1600 preset file. Any file it references
# via `path:` or `modulespecfile:` (resolved relative to the preset's own
# directory, same as Core/PC1500/PresetFile.cpp's resolvePath()) is pulled
# in alongside it automatically.
PRESETS=(
  "examples/maxed-out-mem.pc1600"
  "examples/setup/firmware_bootstrap_utilrm_20.pc1500a"
  "examples/lissajou-1500.pc1500"
  "examples/lissajou-1600.pc1600"
  "examples/lissajou-ce150.pc1600"
  "examples/memtest_ce155.pc1500"
  "examples/memtest_bank.pc1500a"
)

# Standalone files to mirror alongside the presets above that aren't pulled
# in automatically via a preset's path:/modulespecfile: reference (e.g. asm
# source for a preset's binary, kept for reference/rebuilding).
EXTRA_FILES=(
  "examples/memtest_bank.asm"
)

log() { echo "publish_binary_release.sh: $*" >&2; }
die() { log "error: $*"; exit 1; }

# ---------------------------------------------------------------------------
# Preset dependency resolution: given a preset file, print its own path
# followed by every file it references via `path:` or `modulespecfile:`,
# all relative to $REPO_ROOT. Mirrors PresetFile.cpp's resolvePath(): a
# referenced value is resolved relative to the preset file's own directory.
# ---------------------------------------------------------------------------
preset_files() {
  local preset="$1"
  local dir
  dir=$(dirname "$preset")
  echo "$preset"
  # `|| true`: a preset with no path:/modulespecfile: lines (e.g. one using
  # inline `text:`) makes grep exit 1 -- that must not be treated as a
  # pipeline failure under `set -o pipefail`, or this whole function (and
  # the process-substitution loop reading it) aborts after the first preset.
  (grep -E '^[[:space:]]*(path|modulespecfile):[[:space:]]*[^[:space:]]' "$REPO_ROOT/$preset" || true) | while IFS= read -r line; do
    local value
    value=$(echo "$line" | sed -E 's/^[[:space:]]*(path|modulespecfile):[[:space:]]*//; s/[[:space:]]*#.*$//; s/[[:space:]]+$//')
    [ -n "$value" ] || continue
    if [[ "$value" = /* ]]; then
      echo "${value#/}"
    else
      echo "$dir/$value" | sed -E 's#/\./#/#g'
    fi
  done
}

all_preset_files() {
  local p
  for p in "${PRESETS[@]}"; do
    preset_files "$p"
  done
  for p in "${EXTRA_FILES[@]}"; do
    echo "$p"
  done
}

# ---------------------------------------------------------------------------
# --check mode
# ---------------------------------------------------------------------------
run_check() {
  local examples_stale=0
  local stale_files=()
  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    if ! cmp -s "$REPO_ROOT/$f" "$BINARY_REPO/$f" 2>/dev/null; then
      examples_stale=1
      stale_files+=("$f")
    fi
  done < <(all_preset_files)

  local head_sha latest_tag release_sha release_stale=0
  head_sha=$(git rev-parse HEAD)

  latest_tag=$(gh release list -R "$BINARY_REPO_SLUG" -L 1 --json tagName -q '.[0].tagName' 2>/dev/null || true)
  if [ -z "$latest_tag" ]; then
    release_stale=1
    release_sha="(no release yet)"
  else
    release_sha=$(gh release view "$latest_tag" -R "$BINARY_REPO_SLUG" --json body -q '.body' 2>/dev/null \
      | grep -oE 'Built from [^@]+@[0-9a-f]{7,40}' | grep -oE '[0-9a-f]{7,40}$' || true)
    if [ -z "$release_sha" ] || [ "$release_sha" != "$head_sha" ]; then
      release_stale=1
      [ -n "$release_sha" ] || release_sha="(unparsable release notes)"
    fi
  fi

  echo "HEAD commit:      $head_sha"
  echo "Latest release:   ${latest_tag:-none} (built from ${release_sha})"
  echo

  if [ "$examples_stale" -eq 0 ] && [ "$release_stale" -eq 0 ]; then
    echo "Verdict: up to date -- nothing to publish."
    return 0
  fi

  if [ "$examples_stale" -eq 1 ]; then
    echo "Verdict: examples out of date. Differing/missing files:"
    for f in "${stale_files[@]}"; do echo "  - $f"; done
  fi
  if [ "$release_stale" -eq 1 ]; then
    echo "Verdict: release out of date (latest release built from ${release_sha}, HEAD is ${head_sha})."
  fi
  return 0
}

# ---------------------------------------------------------------------------
# Publish flow
# ---------------------------------------------------------------------------
run_publish() {
  local notes="" presets_override=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --notes) notes="$2"; shift 2 ;;
      *) presets_override+=("$1"); shift ;;
    esac
  done
  if [ "${#presets_override[@]}" -gt 0 ]; then
    PRESETS=("${presets_override[@]}")
  fi

  # 1. Locate repos & validate.
  [ -d "$BINARY_REPO/.git" ] || die "$BINARY_REPO is not a git repo"
  ( cd "$BINARY_REPO" && [ "$(git branch --show-current)" = "main" ] ) \
    || die "$BINARY_REPO is not on branch main"
  ( cd "$BINARY_REPO" && [ -z "$(git status --porcelain)" ] ) \
    || die "$BINARY_REPO has uncommitted changes -- commit/stash first"
  gh auth status >/dev/null 2>&1 || die "gh is not authenticated -- run 'gh auth login'"

  # 2. Resolve the source commit & CI run.
  local sha commit_subject
  sha=$(git rev-parse HEAD)
  commit_subject=$(git log -1 --format=%s)
  log "resolving CI run for commit $sha"
  local run_id run_status run_conclusion
  run_id=$(gh run list -R "$SOURCE_REPO_SLUG" --workflow "$WORKFLOW" --commit "$sha" \
    --json databaseId --limit 1 -q '.[0].databaseId' 2>/dev/null || true)
  [ -n "$run_id" ] && [ "$run_id" != "null" ] || die "no $WORKFLOW run found for commit $sha -- push first or wait for Actions"
  run_status=$(gh run list -R "$SOURCE_REPO_SLUG" --workflow "$WORKFLOW" --commit "$sha" \
    --json status --limit 1 -q '.[0].status')
  run_conclusion=$(gh run list -R "$SOURCE_REPO_SLUG" --workflow "$WORKFLOW" --commit "$sha" \
    --json conclusion --limit 1 -q '.[0].conclusion')
  [ "$run_status" = "completed" ] || die "run $run_id is still '$run_status' -- wait for it to finish"
  [ "$run_conclusion" = "success" ] || die "run $run_id concluded '$run_conclusion', not success"
  log "using run $run_id"

  # 3. Download artifacts.
  # `work` is intentionally NOT `local`: the EXIT trap below runs after
  # run_publish returns, in the top-level script scope, so a `local` var
  # here would already be unset by then (an error under `set -u`).
  local zipdir
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  zipdir="$work/zips"
  mkdir -p "$zipdir"
  log "downloading artifacts to $work"
  gh run download "$run_id" -R "$SOURCE_REPO_SLUG" -D "$work/artifacts"

  # 4. Package each artifact.
  # (A plain case statement, not an associative array: macOS ships bash 3.2
  # as /bin/bash, which predates associative-array support entirely.)
  local zips=()
  local name dir zip_name
  for name in calcu1600qt-linux-x86_64 calcu1600qt-linux-arm64 calcu1600qt-macos-arm64 calcu1600qt-windows-x86_64; do
    dir="$work/artifacts/$name"
    [ -d "$dir" ] || { log "no artifact '$name' (skipped, e.g. linux-arm64 continue-on-error) -- skipping"; continue; }
    case "$name" in
      calcu1600qt-linux-x86_64)   zip_name="Calc-U-1600-Qt6-linux-x86_64.zip" ;;
      calcu1600qt-linux-arm64)    zip_name="Calc-U-1600-Qt6-linux-arm64.zip" ;;
      calcu1600qt-macos-arm64)    zip_name="Calc-U-1600-Qt6-macos-arm64.dmg" ;;
      calcu1600qt-windows-x86_64) zip_name="Calc-U-1600-Qt6-windows-x86_64.zip" ;;
    esac

    if [ "$name" = "calcu1600qt-macos-arm64" ]; then
      # The macOS job now uploads a single already-signed/notarized/stapled
      # .dmg (not a raw .app directory) -- just rename it, don't re-zip it
      # (zipping would strip the notarization ticket staple).
      cp "$dir/CalcU1600Qt.dmg" "$zipdir/$zip_name"
      zips+=("$zipdir/$zip_name")
      log "packaged $zip_name"
      continue
    fi

    if [ -f "$dir/CalcU1600Qt" ]; then chmod +x "$dir/CalcU1600Qt"; fi

    (cd "$dir" && zip -r -q "$zipdir/$zip_name" .)
    zips+=("$zipdir/$zip_name")
    log "packaged $zip_name"
  done
  [ "${#zips[@]}" -gt 0 ] || die "no artifacts downloaded -- nothing to publish"

  # 5. Determine the release tag.
  local today base_tag tag existing
  today=$(date +%Y-%m-%d)
  base_tag="qt6-prototype-$today"
  existing=$(gh release list -R "$BINARY_REPO_SLUG" --json tagName -q '.[].tagName' | grep -E "^${base_tag}(-[a-z])?\$" || true)
  if [ -z "$existing" ]; then
    tag="$base_tag"
  else
    # Existing tags for today: base (no letter) counts as 'a'; find the max
    # letter used and pick the next one.
    local alphabet=(a b c d e f g h i j k l m n o p q r s t u v w x y z)
    local max_idx=0 t letter idx
    for t in $existing; do
      if [[ "$t" =~ -([a-z])$ ]]; then
        letter="${BASH_REMATCH[1]}"
      else
        letter="a"
      fi
      for idx in "${!alphabet[@]}"; do
        [ "${alphabet[$idx]}" = "$letter" ] && [ "$idx" -ge "$max_idx" ] && max_idx=$((idx + 1))
      done
    done
    [ "$max_idx" -lt "${#alphabet[@]}" ] || die "too many releases today -- ran out of letter suffixes"
    tag="${base_tag}-${alphabet[$max_idx]}"
  fi
  log "release tag: $tag"

  # 6. Copy example presets.
  local f src dst
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    src="$REPO_ROOT/$f"
    dst="$BINARY_REPO/$f"
    [ -f "$src" ] || die "preset dependency not found: $f"
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
    log "copied $f"
  done < <(all_preset_files)

  # 7. Pause for the manual README edit.
  local asset_lines=""
  local z base
  for z in "${zips[@]}"; do
    base=$(basename "$z")
    asset_lines="${asset_lines}  https://github.com/${BINARY_REPO_SLUG}/releases/download/${tag}/${base}\n"
  done
  echo
  echo "=== Update $BINARY_REPO/README.md now ==="
  echo "Tag:          $tag"
  echo "Commit:       $sha (https://github.com/${SOURCE_REPO_SLUG}/commit/${sha})"
  echo "Actions run:  https://github.com/${SOURCE_REPO_SLUG}/actions/runs/${run_id}"
  echo "Asset URLs:"
  printf '%b' "$asset_lines"
  echo "=========================================="
  echo
  read -r -p "Edit README.md now with the details above, then press Enter to continue (or Ctrl-C to abort)... " _

  # 8. Commit -- tolerate the README/examples already having been committed
  # by hand (e.g. ahead of running this script, or from a prior aborted
  # run) as long as README.md actually references this release's tag;
  # otherwise there's no evidence it was updated for *this* release, so
  # still require the edit.
  ( cd "$BINARY_REPO" && git add README.md examples/ )
  if ( cd "$BINARY_REPO" && [ -n "$(git status --porcelain)" ] ); then
    local commit_msg="${notes:-$commit_subject}"
    ( cd "$BINARY_REPO" && git commit -m "Update to ${commit_msg} build (${tag})

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_014Rh6NeqHcCvdfyposCmTKT" )
  else
    grep -q "$tag" "$BINARY_REPO/README.md" \
      || die "nothing staged to commit, and README.md doesn't mention $tag -- did the README edit happen?"
    log "README.md/examples/ already committed and reference $tag -- skipping commit"
  fi

  # 9. Push.
  ( cd "$BINARY_REPO" && git push )

  # 10. Create the release & upload assets.
  local title="Qt6 prototype ($today${notes:+, $notes})"
  local release_notes="Built from ${SOURCE_REPO_SLUG}@${sha} (commit: ${commit_subject}). See the GitHub Actions run: https://github.com/${SOURCE_REPO_SLUG}/actions/runs/${run_id}"
  gh release create "$tag" "${zips[@]}" -R "$BINARY_REPO_SLUG" --title "$title" --notes "$release_notes"

  # 11. Final summary.
  echo
  echo "Published: https://github.com/${BINARY_REPO_SLUG}/releases/tag/${tag}"
}

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--check" ]; then
  run_check
else
  run_publish "$@"
fi
