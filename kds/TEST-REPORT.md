# 验证记录（主机侧，非真机）

环境：Ubuntu 22.04，gcc 11.4，x86_64；内核源码树 6.1.192（用于 API/语法校验）；
lz4kd codec 取自 `crypto/lz4kd/`（与本包一致）。

## 1. 内核代码编译

```
make ARCH=x86_64 defconfig
scripts/config -e ZRAM -e ZRAM_SDDC -e CRYPTO_LZ4KD
make ARCH=x86_64 prepare modules_prepare
make ARCH=x86_64 KCFLAGS="-D__GFP_CMA=0" drivers/block/zram/
```

结果：zcomp.o / zram_drv.o / zram_lz4kds.o / zram_sddc.o 全部编译通过，
`-Werror` 下无告警（`__GFP_CMA` 是小米/高通侧扩展宏，只在校验树里补了空定义）。

## 2. 编解码往返（用户态）

- 同类页面（同基准 + 少量改动）：`encode_delta` 输出 75–85 B，普通 `encode` 200–220 B；
- `decode_delta` 全部逐字节还原正确。

## 3. SDDC 逻辑仿真

把 `zram_sddc.c` 原文件接到假 zsmalloc + 真 codec，跑 20,000 次随机 store/load/free：

```
rounds=20000 plain=6198 alias=5829 delta=4685 bad=0 leaked_objects=0
orphan test: dependent survived ref slot free
```

- `bad=0`：每次写后立即读回，逐字节一致；
- `alias/delta` 命中确实发生（不是空跑）；
- `leaked_objects=0`：全部释放后 zsmalloc 对象清空，没有泄漏；
- 孤儿测试：先 free 被引用的 REF 槽，再读依赖它的 DELTA 槽，内容仍然正确。

期间修掉的两个真 bug（都在提交前的仿真里发现）：
1. 桶索引当初按"压缩流哈希"建桶、却按"页采样哈希"查桶 → alias 永远命中不了；
2. DELTA 的负载写进 wire 时忘了从 per-cpu 流缓冲拷到 wire 缓冲，写进去是残留数据。
