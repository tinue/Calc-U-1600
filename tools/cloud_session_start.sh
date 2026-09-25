#!/bin/sh
# SessionStart hook for Claude Code cloud sessions (claude.ai/code,
# `claude --cloud`), wired up in .claude/settings.json. Does nothing
# locally: it exits unless CLAUDE_CODE_REMOTE=true, which only the cloud
# session VM sets.
#
# A cloud session starts from a fresh clone, so it has the same gaps as a
# CI checkout (see the core-tests job in .github/workflows/build.yml):
# no ROMs (roms/README.md) and no Linux libsharpdx. This fills both in.
# The Qt6/zlib apt packages are NOT installed here -- they belong in the
# cloud environment's setup script (see docs/Cloud-Sessions.md), which is
# cached across sessions instead of re-running every start.
#
# Everything here is idempotent and cheap on a resumed session:
# fetch_roms.sh skips files whose md5 already matches, and the sharpdx
# fetch is skipped once libsharpdx-linux.a exists.
[ "${CLAUDE_CODE_REMOTE:-}" = "true" ] || exit 0

cd "$(dirname "$0")/.."

tools/fetch_roms.sh || echo "cloud_session_start.sh: fetch_roms.sh failed -- CoreTests and the app need roms/" >&2

if [ ! -f Core/Basic/vendor/sharpdx/libsharpdx-linux.a ]; then
  # Soft-fail, as in CI: without it the preset/BASIC sources just drop out.
  tools/fetch_sharpdx.sh || echo "cloud_session_start.sh: fetch_sharpdx.sh failed -- building without preset/BASIC loading" >&2
fi

# fetch_sharpdx.sh overwrites the tracked sharpdx.h with the release's
# copy. Stdout here lands in the session's context, so flag any drift.
if ! git diff --quiet -- Core/Basic/vendor/sharpdx/sharpdx.h; then
  echo "Note: Core/Basic/vendor/sharpdx/sharpdx.h was modified by tools/fetch_sharpdx.sh (release header differs from the committed one). This is a build artifact of the cloud setup -- do not commit it."
fi

exit 0
