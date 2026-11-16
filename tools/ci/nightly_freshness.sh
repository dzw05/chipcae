#!/usr/bin/env bash
# GitHub Actions helper: skip a scheduled nightly job when main has had no
# commits in the last 24 hours. Manual workflow_dispatch always runs.
set -euo pipefail
if [ "${EVENT_NAME:-}" != "schedule" ] || [ -n "$(git log -1 --since='24 hours ago' --pretty=%H)" ]; then
  echo "run=true" >> "$GITHUB_OUTPUT"
else
  echo "No commits in the last 24 hours; skipping the rest of this job."
  echo "run=false" >> "$GITHUB_OUTPUT"
fi
