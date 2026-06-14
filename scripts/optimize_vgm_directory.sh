#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
	printf 'Usage: %s DIRECTORY\n' "$0" >&2
	exit 2
fi

directory=$1
if [[ ! -d "$directory" ]]; then
	printf 'Not a directory: %s\n' "$directory" >&2
	exit 1
fi

script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -n ${VGM_CMP:-} ]]; then
	vgm_cmp=$VGM_CMP
elif [[ -x "$script_directory/../bin/vgm_cmp" ]]; then
	vgm_cmp="$script_directory/../bin/vgm_cmp"
elif [[ -x "$script_directory/../bin/vgm_cmp.exe" ]]; then
	vgm_cmp="$script_directory/../bin/vgm_cmp.exe"
elif [[ -x "$script_directory/../build/vgm_cmp" ]]; then
	vgm_cmp="$script_directory/../build/vgm_cmp"
elif [[ -x "$script_directory/../build/vgm_cmp.exe" ]]; then
	vgm_cmp="$script_directory/../build/vgm_cmp.exe"
elif command -v vgm_cmp >/dev/null 2>&1; then
	vgm_cmp=$(command -v vgm_cmp)
elif [[ -x "$script_directory/../../vgmtools/build/vgm_cmp" ]]; then
	vgm_cmp="$script_directory/../../vgmtools/build/vgm_cmp"
else
	printf 'Unable to find bundled vgm_cmp. Rebuild spc2vgm or set VGM_CMP.\n' >&2
	exit 1
fi

shopt -s nullglob
files=("$directory"/*.vgm)
optimized=0
unchanged=0

for file in "${files[@]}"; do
	[[ $file == *_optimized.vgm ]] && continue
	temp="${file}.vgm_cmp_tmp.$$"
	rm -f -- "$temp"

	printf 'Optimizing %s\n' "$file"
	if ! "$vgm_cmp" "$file" "$temp"; then
		rm -f -- "$temp"
		printf 'vgm_cmp failed for %s\n' "$file" >&2
		exit 1
	fi

	if [[ -f "$temp" ]]; then
		mv -f -- "$temp" "$file"
		((optimized += 1))
	else
		((unchanged += 1))
	fi
done

printf 'Done: %d optimized, %d already optimal.\n' "$optimized" "$unchanged"
