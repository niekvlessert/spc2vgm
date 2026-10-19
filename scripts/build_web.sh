#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-web --target spc2vgm unarr_extract vgm_cmp --parallel 4
mkdir -p web/dist web/licenses
cp build-web/web/{spc2vgm,unarr_extract,vgm_cmp}.{js,wasm} web/dist/
cp THIRD_PARTY_NOTICES.md web/
cp external/vgmtools/LICENSE web/licenses/vgmtools-GPL-2.0.txt
cp extern/unarr/COPYING web/licenses/unarr-LGPL-3.0.txt
# These retain the upstream dependency license notices alongside the binaries.
cp external/smwcentral-spc-player/src/snes_spc/license.txt web/licenses/snes-spc.txt
cp external/libvgm/COPYING* web/licenses/ 2>/dev/null || true
touch web/.nojekyll
