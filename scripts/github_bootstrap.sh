#!/usr/bin/env bash
# One-shot: create the GitHub repo, push main, label and open the seed issues.
# Requires `gh auth login`. Usage: scripts/github_bootstrap.sh [owner/name]
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="${1:-epicaDB}"
gh repo create "$REPO" --public --source=. --remote=origin --push \
  --description "LSM key-value database: C++20 storage engine + Java query layer"
gh label create "good first issue" --color 7057ff --force >/dev/null
gh label create enhancement --color a2eeef --force >/dev/null
for f in .github/issues/*.md; do
  title=$(sed -n '1s/^# //p' "$f")
  gh issue create --title "$title" --body-file <(tail -n +2 "$f") \
    --label enhancement --label "good first issue"
done
