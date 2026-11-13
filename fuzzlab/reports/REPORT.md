# libpng 解码器模糊测试与像素一致性报告

- 被测对象：仓库内 libpng `1.6.38.git`（commit `43b7f9a`），全量源码静态编入测试驱动
- 编译选项：`gcc 15.2.0 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -DNDEBUG`
  （ASan 含 LSan；`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`）
- 参考实现：Pillow 12.1.1（独立 libpng/zlib 解码路径），逐字节 RGB 比对
- 测试日期：2026-11-14

## 1. 结论摘要

| 指标 | 结果 |
|---|---|
| 畸形图总数 | 3000（5 类 × 600） |
| 崩溃（SIGSEGV/SIGABRT/超时） | **0** |
| AddressSanitizer 越界事件 | **0** |
| UndefinedBehaviorSanitizer 事件 | **0** |
| LeakSanitizer 泄漏事件 | **0** |
| 干净拒绝（带块类型+偏移） | 2906 |
| 宽容解码（规范允许，见 §4） | 94，全部为 IEND 后冗余块 |
| 合法图总数 | 500 |
| 与参考实现逐像素一致 | **500 / 500（100%）** |
| 合法图消毒器/泄漏事件 | **0** |

未发现任何内存安全缺陷或像素解码偏差。

## 2. 畸形语料分类结果

| 类别 | 数量 | 拒绝 | 宽容解码 | 主要 libpng 报错 |
|---|---|---|---|---|
| truncation 截断 | 600 | 600 | 0 | read past end of input (470), Not a PNG file (130) |
| length_mismatch 块长错位 | 600 | 600 | 0 | IHDR invalid (142), IDAT CRC (136), integer out of range (120), PLTE CRC (70), EOF (65), IEND/IHDR CRC (67) |
| oversize 超大尺寸 | 600 | 600 | 0 | test safety limit (500), Invalid IHDR data (100，含 0 宽/高) |
| illegal_filter 非法滤波 | 600 | 600 | 0 | bad adaptive filter value (459), Not enough image data (141) |
| cyclic_chunk 循环/乱序块 | 600 | 506 | 94 | Missing PLTE before IDAT (260), IHDR out of place (146), EOF (100) |

拒绝定位覆盖的块名分布：IDAT 1739、IHDR 608、PLTE 203、IEND 83；
块外位置按 `signature+偏移`（73）与 `eof+偏移`（200）标注。
**全部 2906 条拒绝记录均含块类型与文件偏移，无缺失。**

## 3. 失败定位格式

每个拒绝输出一行机器可解析记录：

```
REJECT file=<path> chunk=<块名>+<块内偏移十六进制> file_offset=0x<绝对偏移> libpng=error
```

示例（最小反例实测）：

```
REJECT ... chunk=IHDR+0x0        file_offset=0x29  (重复 IHDR，IHDR out of place)
REJECT ... chunk=IDAT(CRC)+0x0   file_offset=0x45  (滤波类型 255，bad adaptive filter value)
REJECT ... chunk=eof+0x10        file_offset=0x10  (块声明长度越过文件尾)
REJECT ... chunk=signature+0x3   file_offset=0x3   (签名被截断)
```

块表失步（声明长度与磁盘不符）时回退标注最近块 `(framing)`，签名区与文件尾分别用
`signature+` / `eof+`，保证任何拒绝都可定位。

## 4. 94 例“宽容解码”说明（非缺陷）

全部集中在 cyclic_chunk 的两个变体：

- 变体 3（52）：IEND 之后追加 tEXt 与第二个 IEND
- 变体 4（42）：IEND 前插入冗余 IDAT 且出现重复 IEND

PNG 规范规定 IEND 标记数据流结束，解码器忽略其后字节属于合规宽容行为；
libpng 正常返回且消毒器零事件。语料中真正的结构性违规（重复 IHDR、IDAT 先于 PLTE、
PLTE 晚于 IDAT、二次签名+IHDR）均被确定性拒绝（506 例）。

## 5. 合法图像素比对

500 张由独立纯 Python 编码器生成，覆盖全部合法组合：

- 位深：1/2/4/8/16；颜色类型：0 灰度、2 真彩、3 索引、4 灰+alpha、6 RGBA
- Adam7 交错约占 25%；含 PLTE、tRNS（含调色板/灰度/RGB 透明）、gAMA、tIME、tEXt
- 五种滤波类型（None/Sub/Up/Average/Paeth）随机混合

harness 输出统一转换链（调色板展开、tRNS→alpha、16 位取高字节、灰度→RGB、
filler 后取 RGB），与 Pillow 参考端按相同语义对齐后逐像素比较：

- 匹配 500/500，失配像素总数 0
- 16 位语义差（libpng 取高字节 vs Pillow 内部右移）在参考端按 libpng 语义对齐，
  不属于解码分歧

## 6. 最小反例

目录 `fuzzlab/min/`，每类一个手工构造的最小文件：

| 文件 | 字节 | 畸形点 | 实测拒绝 |
|---|---|---|---|
| min_truncation.png | 21 | IHDR 体中途截断 | read past end of input，eof+0x10 |
| min_length_mismatch.png | 20 | IHDR 声明长度 255，文件即结束 | integer out of range / EOF |
| min_oversize.png | 45 | IHDR 声明 70000×8 | 维度安全限制 / 块序检查 |
| min_illegal_filter.png | 85 | 首扫描线滤波字节=255 | bad adaptive filter value，IDAT |
| min_cyclic_chunk.png | 110 | IHDR 重复出现 | IHDR out of place，IHDR+0x0 |

复现：`ASAN_OPTIONS=detect_leaks=1 ./build/harness decode min/<file>`

## 7. 复现方式

```
cd fuzzlab
./run_all.sh
```

产物：

- `reports/malformed_results.json`：3000 例逐条结果（kind/rc/REJECT/stderr 尾部）
- `reports/valid_compare.json`：500 例逐像素比对结果与首批失配坐标
- `reports/summary.json`：汇总计数与合法组合覆盖
- `corpus/malformed|valid/`：可复现语料（固定随机种子）及 manifest

## 8. 测试过程中修复的测试设施问题（非 libpng 缺陷）

- harness 长跳转路径未释放行缓冲 → 已用文件级缓冲 + 错误路径统一回收，LSan 清零
- harness 未启用 Adam7 多 pass 处理 → 改为 `png_set_interlace_handling` +
  `png_read_image`，交错图像素恢复正确
- 自研 PNG 编码器的 bpp 单位、Adam7 跨步采样、16 位打包、tRNS 对齐等问题已修正，
  保证输入语料本身合法
