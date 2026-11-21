#!/bin/sh
set -e
KERNEL=${1:?usage: overlay-kds.sh <kernel-root>}
HERE=$(cd "$(dirname "$0")/.." && pwd)

cp "$HERE"/files/drivers/block/zram/*.c "$HERE"/files/drivers/block/zram/*.h \
   "$KERNEL"/drivers/block/zram/

python3 - "$KERNEL" "$HERE" <<'PY'
import sys
k, h = sys.argv[1], sys.argv[2]

mk = k + '/drivers/block/zram/Makefile'
s = open(mk).read()
if 'zram_sddc.o' not in s:
    s = s.replace('zram-y\t:=\tzcomp.o zram_drv.o',
                  'zram-y\t:=\tzcomp.o zram_drv.o\nzram-$(CONFIG_ZRAM_SDDC)\t+=\tzram_sddc.o zram_lz4kds.o', 1)
    open(mk, 'w').write(s)

kc = k + '/drivers/block/zram/Kconfig'
s = open(kc).read()
if 'config ZRAM_SDDC' not in s:
    s += open(h + '/config/kds-sddc.kconfig').read()
    open(kc, 'w').write(s)

zs = k + '/mm/zsmalloc.c'
s = open(zs).read()
if 'zs_lookup_class_index' not in s:
    anchor = "\treturn min_t(int, ZS_SIZE_CLASSES - 1, idx);\n}\n"
    add = anchor + """
int zs_lookup_class_index(unsigned int size)
{
	return get_size_class_index(size);
}
EXPORT_SYMBOL_GPL(zs_lookup_class_index);
"""
    assert anchor in s, 'get_size_class_index tail not found'
    open(zs, 'w').write(s.replace(anchor, add, 1))

zh = k + '/include/linux/zsmalloc.h'
s = open(zh).read()
if 'zs_lookup_class_index' not in s:
    open(zh, 'w').write(s.replace('size_t zs_huge_class_size(struct zs_pool *pool);',
        'size_t zs_huge_class_size(struct zs_pool *pool);\nint zs_lookup_class_index(unsigned int size);', 1))
PY
dedupe() {
	f="$1"
	[ -f "$f" ] || return 0
	awk '!/^CONFIG_/ || !seen[$0]++' "$f" > "$f.kdstmp" && mv "$f.kdstmp" "$f"
}

for cfg in "$KERNEL"/arch/arm64/configs/gki_defconfig "$KERNEL"/arch/arm64/configs/ksu.fragment; do
	if [ -f "$cfg" ]; then
		grep -q CONFIG_ZRAM_SDDC "$cfg" || cat "$HERE"/config/kds.fragment >> "$cfg"
	fi
done
dedupe "$KERNEL"/arch/arm64/configs/gki_defconfig
dedupe "$KERNEL"/arch/arm64/configs/ksu.fragment
echo "overlay done"
