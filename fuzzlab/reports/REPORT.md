# libpng 畸形输入加固与零崩溃报告

- 被测库：`libpng 1.6.38.git`，基线 commit `43b7f9a`
- 编译器：GCC 15.2.0；zlib 1.3.1
- 消毒器：AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer
- 参数：`-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DNDEBUG`
- 运行环境：`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`
- 测试日期：2026-11-17

## 1. 结论

| 指标 | 结果 |
|---|---:|
| 畸形 PNG 总数 | 3000 |
| 干净拒绝（退出码 1） | **3000 / 3000** |
| 崩溃、SIGSEGV、SIGABRT | **0** |
| AddressSanitizer 越界/释放错误 | **0** |
| UndefinedBehaviorSanitizer 事件 | **0** |
| LeakSanitizer 泄漏事件 | **0** |
| 超时 | **0** |
| 带错误码、文件偏移和块内偏移的拒绝记录 | **3000 / 3000** |
| 合法 PNG 总数 | 500 |
| 与修复前库输出逐字节一致 | **500 / 500** |
| libpng 自带 CTest | **32 / 32 通过** |
| 测试中 libpng 峰值分配 | **51,494 字节** |

畸形输入不会进入未定义行为路径；结构错误在 libpng 前被无分配预检拒绝，
压缩流、CRC 和滤波错误由 libpng 返回，统一转成非零退出码和定位信息。

## 2. 根因与修复

### 2.1 块长与 IDAT 上界算术

`png_check_chunk_length()` 用 `width * channels * bytes_per_pixel` 计算
`row_factor`。当宽高接近 PNG 上限时，该乘法或后续 zlib 开销加法可能在
32 位 `size_t` 上回绕，极端情况下得到 0 并发生除零。

修复：

- 乘法前检查 `width` 与像素字节数的乘积是否溢出；
- `row_factor` 为 0 或高度乘法越界时直接使用 PNG 31 位上限；
- zlib/deflate 开销加法在 `PNG_UINT_31_MAX` 前分步检查，避免二次回绕；
- 保留合法图像原有的块长上限语义。

### 2.2 块缓冲超量分配

`png_read_buffer()` 原先只依赖调用方和块长检查。畸形辅助块可把块长推到
用户配置上限，再触发大块分配。

修复：

- 在真正 `png_malloc_base()` 前同时检查运行时/编译时的
  `user_chunk_malloc_max`；
- IDAT 读取若返回空缓冲，立即 `png_chunk_error()`，不再把 NULL 传给
  `png_crc_read()`；
- IDAT 本身仍按 `IDAT_read_size` 分块读取，不会按声明块长一次性分配。

### 2.3 截断 IDAT 与 stale zstream 指针

zlib 流的 `next_in`、`avail_in`、`next_out`、`avail_out` 在释放所有权时
未全部清零。截断 IDAT、提前调用 `png_read_end()` 或重复使用解压流时，
后续代码可能继续引用已释放的读缓冲。

修复：

- 新增 `png_inflate_detach_buffers()`，同时清空输入/输出指针和计数；
- 在 claim、chunk 解压释放、iCCP 成功/失败释放、IDAT finish 时统一调用；
- 截断流现在稳定返回 `Not enough image data` 或 `read past end of input`。

### 2.4 行尺寸与变换乘法

`png_check_IHDR()` 的行宽检查在部分平台使用过窄的 `~7U` 掩码；
`png_read_start_row()` 和读变换中的 `rowbytes = width * channels`
也可能在 32 位乘法中先回绕。

修复：

- IHDR 行宽掩码提升到 `png_alloc_size_t`；
- `png_read_start_row()` 对 `row_bytes` 乘法和 `+ 48` 填充逐项检查；
- 读变换中所有行字节乘法先转为 `size_t`；
- 合法图像的像素路径和变换顺序未改变。

### 2.5 严格解码驱动

`fuzzlab/harness.c` 增加无分配预检：

- PNG 签名、块头长度、块类型、CRC、IEND 位置和尾随数据；
- 块长必须不超过 PNG 31 位上限且完整落在文件内；
- 宽、高、总像素数在进入 libpng 前限制；
- 非法块序、缺失 PLTE、重复 IHDR/IEND 等结构错误直接拒绝；
- 自定义分配器拒绝单次超过 16 MiB、累计超过 256 MiB 的分配；
- 错误回调通过 `setjmp` 返回，不终止进程。

错误码：

| code | 含义 |
|---:|---|
| 1 | 签名错误 |
| 2 | 文件或块被截断 |
| 3 | 块头/块类型非法 |
| 4 | 块长非法或超出 PNG 上限 |
| 5 | 块顺序错误 |
| 6 | 尺寸或颜色/位深组合非法 |
| 7 | 分配上限或内存不足 |
| 8 | libpng 压缩流/滤波错误 |
| 9 | CRC 错误 |
| 10 | IEND 后仍有数据 |

拒绝格式：

```text
REJECT code=<code>(<name>) offset=0x<file-offset> chunk=<type>+0x<chunk-offset> \
       detail="<reason>" file=<path> alloc_peak=<bytes>
```

## 3. 最小样本

样本位于 `fuzzlab/samples/`，可直接复现：

| 文件 | 字节 | 实测结果 |
|---|---:|---|
| `truncated_idat.png` | 46 | code=2，`eof+0x0`，块体越过文件尾 |
| `truncated_idat_stream.png` | 63 | code=8，`IEND(CRC)+0x0`，`Not enough image data` |
| `illegal_chunk_length.png` | 29 | code=4，`chunk_header+0x0`，块长 `0xffffffff` |
| `oversize_dimensions.png` | 45 | code=6，`IHDR+0x0`，宽 70000 超限 |
| `oversize_pixels.png` | 45 | code=6，`IHDR+0x4`，65535×65535 总像素超限 |
| `illegal_filter.png` | 85 | code=8，`IDAT(CRC)+0x0`，滤波值 255 |
| `trailing_after_iend.png` | 82 | code=10，`IEND(end)` |
| `valid_gray8.png` | 78 | code=0，4×4 灰度解码成功 |
| `valid_rgba8.png` | 100 | code=0，4×4 RGBA 解码成功 |

所有拒绝样本均返回退出码 1；合法样本返回退出码 0。

## 4. 语料与统计

固定随机种子 `20261117` 生成：

| 类别 | 数量 | 干净拒绝 |
|---|---:|---:|
| truncation | 500 | 500 |
| idat_truncation | 500 | 500 |
| length_mismatch | 500 | 500 |
| oversize | 500 | 500 |
| illegal_filter | 500 | 500 |
| cyclic_chunk | 500 | 500 |
| **合计** | **3000** | **3000** |

合法语料 500 张，覆盖：

- 位深 1、2、4、8、16；
- 灰度、真彩、索引、灰度+alpha、RGBA；
- PLTE、tRNS、gAMA、tEXt；
- 非交错和 Adam7 交错；
- 五种 PNG 滤波类型。

每个合法文件同时用修复前基线和修复后库解码。两次输出统一转换为
RGBA8，比较宽、高、行字节数和完整像素哈希；500 张全部一致。

明细：

- `reports/summary.json`
- `reports/malformed_results.json`
- `reports/valid_comparison.json`

## 5. 复现

```bash
cd fuzzlab
./run_all.sh
```

脚本会编译 ASan/UBSan 版本、编译修复前基线、生成 3000 个畸形图和
500 个合法图，最后运行完整统计。单独执行样本：

```bash
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
  fuzzlab/build/harness check fuzzlab/samples/truncated_idat.png
```

库测试：

```bash
cmake -S . -B build-test -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DPNG_SHARED=ON -DPNG_STATIC=ON -DPNG_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-test -j2
ctest --test-dir build-test --output-on-failure
```
