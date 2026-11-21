#!/bin/sh
set -e
KERNEL=${1:?usage: apply-kds.sh <kernel-root>}
HERE=$(cd "$(dirname "$0")/.." && pwd)

cd "$KERNEL"
for p in "$HERE"/patches/*.patch; do
	echo "applying $(basename "$p")"
	git apply -p1 --whitespace=nowarn "$p" 2>/dev/null || patch -p1 -s --no-backup-if-mismatch < "$p"
done

python3 - "$KERNEL" "$HERE" <<'PY2'
import sys
k, h = sys.argv[1], sys.argv[2]
kc = k + '/drivers/block/zram/Kconfig'
s = open(kc).read()
if 'config ZRAM_SDDC' not in s:
    s += open(h + '/config/kds-sddc.kconfig').read()
    open(kc, 'w').write(s)
if 'CONFIG_ZRAM_SDDC' not in open(k + '/arch/arm64/configs/gki_defconfig').read():
    open(k + '/arch/arm64/configs/gki_defconfig', 'a').write(open(h + '/config/kds.fragment').read())
PY2
echo "done"
