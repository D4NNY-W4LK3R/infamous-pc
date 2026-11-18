#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Makes the one-click builder package from this checkout: the Second Son Adaptive commits as
# patches on top of the pinned infamous-pc v1.1.0 commit, each with its SHA-256, and the source
# tree hash the patched checkout must have. Usage: make_builder_package.sh <output dir>
set -euo pipefail
if [ -n "${GITHUB_ACTIONS:-}" ]; then
    trap 'echo "::error title=builder package::line $LINENO: $BASH_COMMAND"' ERR
fi

BASE_REPO="https://github.com/MeWeGo/infamous-pc.git"
BASE_COMMIT="2c599e026f2b69166fd71eac4343f14152a34d38" # tag v1.1.0
out="${1:?output directory}"
root="$(git rev-parse --show-toplevel)"

git -C "$root" cat-file -e "${BASE_COMMIT}^{commit}" || {
    echo "the pinned base commit is not in this checkout (fetch with full history)" >&2
    exit 1
}
rm -rf "$out"
mkdir -p "$out/patches"
cp "$root/tools/builder/Build-SecondSonAdaptive.ps1" "$root/tools/builder/BUILD_ON_THIS_PC.cmd" \
   "$root/tools/builder/CHECK_ONLY.cmd" "$root/tools/builder/README_BUILDER.txt" "$out/"
git -C "$root" -c core.autocrlf=false format-patch --quiet --zero-commit --no-signature \
    --output-directory "$out/patches" "${BASE_COMMIT}..HEAD"

tree="$(git -C "$root" rev-parse 'HEAD^{tree}')"
{
    echo "{"
    echo "  \"base_repo\": \"${BASE_REPO}\","
    echo "  \"base_commit\": \"${BASE_COMMIT}\","
    echo "  \"result_commit_of_origin\": \"$(git -C "$root" rev-parse HEAD)\","
    echo "  \"result_tree\": \"${tree}\","
    echo "  \"patches\": ["
    first=1
    for p in "$out"/patches/*.patch; do
        [ $first -eq 1 ] || echo ","
        first=0
        sha="$(sha256sum "$p" | cut -d' ' -f1)"
        printf '    { "file": "%s", "sha256": "%s" }' "$(basename "$p")" "$sha"
    done
    echo ""
    echo "  ]"
    echo "}"
} > "$out/source_lock.json"
echo "builder package: $(ls "$out/patches" | wc -l) patches, result tree ${tree}"
