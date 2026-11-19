#!/usr/bin/env bash
# Hosted SDK setup only. Never accept, copy, or synthesize agreement files.
set -Eeuo pipefail
exec 3>&1
exec >/dev/null 2>&1
stage=ndk-preflight
trap 'printf "FAIL: %s\n" "$stage" >&3' ERR
sdk=${ANDROID_HOME:-}
licenses="$sdk/licenses"
manager="$sdk/cmdline-tools/latest/bin/sdkmanager"
[[ -n $sdk && -d $licenses && -s "$licenses/android-sdk-license" && -x $manager ]]

license_hash() {
  python3 - "$1" <<'PY'
import hashlib, os, stat, sys
from pathlib import Path
root = Path(sys.argv[1])
if root.is_symlink() or not root.is_dir():
    raise ValueError()
manifest = hashlib.sha256()
for entry in sorted(root.rglob('*')):
    mode = entry.lstat().st_mode
    encoded = os.fsencode(entry.relative_to(root))
    manifest.update(len(encoded).to_bytes(4,'big')+encoded)
    if stat.S_ISDIR(mode):
        manifest.update(b'd')
    elif stat.S_ISREG(mode):
        digest = hashlib.sha256()
        with entry.open('rb') as file:
            for data in iter(lambda: file.read(65536),b''):
                digest.update(data)
        manifest.update(b'f'+digest.digest())
    else:
        raise ValueError()
print(manifest.hexdigest())
PY
}

stage=license-snapshot-before
if ! before=$(license_hash "$licenses"); then false; fi
stage=ndk-install
install_status=0
timeout 360 "$manager" --sdk_root="$sdk" --install 'ndk;28.2.13676358' </dev/null || install_status=$?
stage=license-snapshot-after
if ! after=$(license_hash "$licenses"); then false; fi
stage=licenses-unchanged
[[ $before == "$after" ]]
stage=ndk-install
[[ $install_status == 0 ]]
stage=ndk-receipt
grep -Eq '^Pkg.Revision[[:blank:]]*=[[:blank:]]*28\.2\.13676358[[:space:]]*$' \
  "$sdk/ndk/28.2.13676358/source.properties"
printf 'PASS: official NDK r28c with unchanged existing license files\n' >&3
