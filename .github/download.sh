#!/bin/bash
set -e

REPO="${ANT_DOWNLOAD_REPO:-theMackabu/ant}"
WORKFLOW="${ANT_DOWNLOAD_WORKFLOW:-build.yml}"
OUT_DIR="${ANT_DOWNLOAD_OUT_DIR:-$(dirname "${BASH_SOURCE[0]}")/artifacts}"

LATEST=$(gh run list \
  --repo "$REPO" \
  --workflow "$WORKFLOW" \
  --limit 10 \
  --json databaseId,status,conclusion,displayTitle,headBranch,createdAt,workflowName \
  --jq '[.[] | select(.status == "completed" and .conclusion == "success")][0]')

if [[ -z "$LATEST" || "$LATEST" == "null" ]]; then
  echo "No completed successful $WORKFLOW runs found in $REPO."
  exit 1
fi

RUN_ID=$(echo "$LATEST" | jq -r '.databaseId')
STATUS=$(echo "$LATEST" | jq -r '.status')
CONCLUSION=$(echo "$LATEST" | jq -r '.conclusion')
TITLE=$(echo "$LATEST" | jq -r '.displayTitle')
WORKFLOW_NAME=$(echo "$LATEST" | jq -r '.workflowName')
BRANCH=$(echo "$LATEST" | jq -r '.headBranch')
CREATED=$(echo "$LATEST" | jq -r '.createdAt')

echo "Latest run:"
echo "  Repo:       $REPO"
echo "  Workflow:   $WORKFLOW_NAME ($WORKFLOW)"
echo "  Title:      $TITLE"
echo "  Branch:     $BRANCH"
echo "  Run ID:     $RUN_ID"
echo "  Status:     $STATUS"
echo "  Conclusion: $CONCLUSION"
echo "  Created:    $CREATED"
echo

echo "Run completed successfully. Downloading artifacts..."
mkdir -p "$OUT_DIR"

gh api "repos/${REPO}/actions/runs/${RUN_ID}/artifacts" --jq '.artifacts[] | "\(.id) \(.name)"' | \
while read -r id name; do
  if [[ "$name" == version-* ]]; then continue; fi
  echo "  Downloading $name..."
  gh api "repos/${REPO}/actions/artifacts/${id}/zip" > "${OUT_DIR}/${name}.zip"
done

echo "Artifacts saved to $OUT_DIR"
