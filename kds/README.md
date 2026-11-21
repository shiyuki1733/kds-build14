# 小米14 lz4kds（LZ4KD + SDDC）内核移植包

目标内核：`6.1.138-android14-11-g0c3d559bcd85-ab14529422`（GKI，4K 页，KCFI+SCS+MODVERSIONS）
目标构建链：LingLuo17/AnyKernel3 `v2.3.0-r42`（common-android14-6.1-2025-06，LTO=thin，build/build.sh）

## 这个包做了什么

1. **lz4kds 编解码后端**（`drivers/block/zram/zcomp.[ch]` + 新增 `zram_lz4kds.c`）
   绕过 crypto compress API 直连 codec，因为 delta 接口 crypto 层根本没有；
   每 CPU 32 KiB 编码状态（`lz4kd_encode_state_bytes_min()`）。
2. **SDDC**（新增 `zram_sddc.[ch]`）
   - `REF`：普通压缩槽，同时把自己登记进索引，供别人当字典；
   - `ALIAS`：压缩流与某个 REF 逐字节相同，槽里只存指针，**0 字节**；
   - `DELTA`：页面用 `lz4kd_encode_delta()` 相对某个相似 REF 编码；
   - 引用计数 + 孤儿机制：REF 槽被覆盖/回收后句柄不立刻释放，最后一个依赖者走了才释放，
     所以不存在悬空引用、不存在 ABA。
3. **zram 挂点**（`zram_drv.[ch]`）
   `ZRAM_SDDC` 槽标志；写路径在 same-filled 判定之后接管；读路径在槽锁内接管；
   `zram_free_page()` 增加释放分支；`zram_meta_alloc/free` 生命周期挂接。
4. **zsmalloc**：导出 `zs_lookup_class_index()`，用于"必须落到更小 size class 才有收益"的门槛判定。

## 怎么用（推荐 overlay 方式）

你的仓库已经打了 SukiSU 的 `lz4kd.patch`（它会改 `zcomp.c` 的算法名列表），
用补丁方式会在这段撞车，所以直接把整套文件覆盖进去最稳：

```sh
# 在 AnyKernel3 仓库的 kernel 源码根目录（就是 build.sh 跑的那个目录）
sh /path/to/kds-deliver/scripts/overlay-kds.sh .
```

脚本做的事：
- 覆盖 `drivers/block/zram/` 下 7 个文件（已保留 `lz4k` / `lz4kd` / `deflate` / `lz4kds` 算法名）；
- `Makefile` 追加 `zram_sddc.o zram_lz4kds.o`；
- `Kconfig` 追加 `config ZRAM_SDDC`；
- `mm/zsmalloc.c`、`include/linux/zsmalloc.h` 注入类索引导出；
- `arch/arm64/configs/gki_defconfig` 追加下面三行。

要开的配置：

```
CONFIG_ZRAM=y
CONFIG_ZRAM_SDDC=y
CONFIG_CRYPTO_LZ4KD=y
```

如果想用补丁方式（干净基线，没打 SukiSU lz4kd.patch 时）：

```sh
sh scripts/apply-kds.sh <kernel-root>
```

两条路线产物完全一致（已实测逐文件 diff 无差异）；overlay 只覆盖 `drivers/block/zram/`
下的源码，Kconfig / Makefile / zsmalloc / gki_defconfig 都是增量注入，可重复执行。

## 编译

照你现在的流程不变（`LTO=thin BUILD_CONFIG=common/build.config.gki.aarch64 build/build.sh`），
补丁/覆盖放在 patch 步骤之后、编译之前即可。

> 本机（Ubuntu 2 核 4G）没有 clang/arm64 交叉工具链，**只做了 x86_64 主机侧编译校验**
> （`drivers/block/zram/` 全部编过，KCFI/NEON 无关部分无警告）以及逻辑仿真，真机镜像必须走你的 Actions。

## 开机后验证

```sh
cat /sys/block/zram0/comp_algorithm        # 列表里应有 [lz4kds] 或 lz4kds
echo lz4kds > /sys/block/zram0/comp_algorithm   # 必须在写 disksize 之前
echo 12G > /sys/block/zram0/disksize
dmesg | grep -i lz4kd
```

`/sys/block/zram0/mm_stat` 第二列（压缩后占用）应该明显低于纯 lz4kd。

## 可调参数（都在 `zram_sddc.c` 顶部）

| 宏 | 现值 | 作用 |
| --- | --- | --- |
| `SDDC_MAX_CANDIDATES` | 4 | 每个桶最多挑几个候选做 delta 尝试 |
| `SDDC_BUCKET_BITS` | 14 | 16384 个桶（按页首尾采样哈希分配） |
| `SDDC_INDEX_WAYS` | 6 | 每桶 6 个 REF 位置 |

内存开销：桶表约 16 MiB（unordered 数组，非 per-object），两个 8 KiB 双页缓冲 + 32 KiB
编码状态的 per-cpu 流。若嫌大，把 `SDDC_BUCKET_BITS` 降到 12，桶表降到 4 MiB。

## 行为与安全性

- SDDC 只是缓存优化：任何一步失败（不可压缩、候选不匹配、收益不够、分配失败）都
  `return -EAGAIN` 回到原厂路径，槽内容与原来完全一致；
- 收益门槛：`delta 长度 + 24B 头 + 8B` 必须小于原压缩流长度，**且**必须落到更小的
  zsmalloc size class，否则不采纳；
- 引用图只有一层（REF ← ALIAS/DELTA），没有链、没有环、没有 delta-on-delta；
- 大页（>= huge class）直接交回原路径，不经过 SDDC。

## 已知未验证项（真机前请注意）

1. 未在 arm64 真机跑过（无工具链），只做了静态编译 + 用户态逻辑仿真；
2. `lz4kd_encode_delta` 要求 ref 与目标页**物理连续**，代码里用 `__get_free_pages(order=1)`
   的 8 KiB 缓冲保证（读侧 8 KiB、写侧 8 KiB，互不共用）；
3. 读路径在 zram 槽 bit-spinlock 内执行，全部操作不睡眠（映射→拷贝→解映射串行，
   因为 6.1 zsmalloc 不支持嵌套映射）；
4. A0 后台优化模块会 swapoff→reset→重建 zram0，算法必须在 disksize 之前写，
   否则会退回它自己的 `zstd`。
