#!/bin/bash
set -e

REPO="themackabu/ant"
OUT_DIR="$(dirname "${BASH_SOURCE[0]}")/artifacts"

LATEST=$(gh run list --repo "$REPO" --limit 1 --json databaseId,status,conclusion,displayTitle,headBranch,createdAt)

RUN_ID=$(echo "$LATEST" | jq -r '.[0].databaseId')
STATUS=$(echo "$LATEST" | jq -r '.[0].status')
CONCLUSION=$(echo "$LATEST" | jq -r '.[0].conclusion')
TITLE=$(echo "$LATEST" | jq -r '.[0].displayTitle')
BRANCH=$(echo "$LATEST" | jq -r '.[0].headBranch')
CREATED=$(echo "$LATEST" | jq -r '.[0].createdAt')

echo "Latest run:"
echo "  Title:      $TITLE"
echo "  Branch:     $BRANCH"
echo "  Run ID:     $RUN_ID"
echo "  Status:     $STATUS"
echo "  Conclusion: $CONCLUSION"
echo "  Created:    $CREATED"
echo

if [[ "$STATUS" != "completed" ]]; then
  echo "Run is still in progress. Exiting."
  exit 1
fi

if [[ "$CONCLUSION" != "success" ]]; then
  echo "Run did not succeed (conclusion: $CONCLUSION). Exiting."
  exit 1
fi

echo "Run completed successfully. Downloading artifacts..."
mkdir -p "$OUT_DIR"

gh api "repos/${REPO}/actions/runs/${RUN_ID}/artifacts" --jq '.artifacts[] | "\(.id) \(.name)"' | \
while read -r id name; do
  if [[ "$name" == version-* ]]; then continue; fi
  echo "  Downloading $name..."
  gh api "repos/${REPO}/actions/artifacts/${id}/zip" > "${OUT_DIR}/${name}.zip"
done

echo "Artifacts saved to $OUT_DIR"