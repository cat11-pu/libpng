# 伽马与透明通道解码缺陷：复现、修复与逐像素比对

- 被测库：仓库内 libpng `1.6.38.git`（commit `43b7f9a`），全量源码静态编入驱动
- 参考实现：`src/reference.py`，纯 Python（zlib + 自写反滤波/Adam7），不链接 libpng；
  另用 Pillow 12.1.1 对 sRGB 编码图做第三方交叉验证
- 输出约定：统一解码为 8-bit RGBA；颜色按规范做伽马转换，输出到 sRGB 编码；
  全透明像素（alpha == 0）的 R/G/B 清零
- 日期：2026-11-14

## 1. 缺陷现象与根因

### 1.1 解码后颜色偏暗（伽马处理错误）

缺陷驱动忽略文件自身的传输函数，固定调用
`png_set_gamma(png, 1.0, 0.45455)`，即向已经是 sRGB 编码的样本再套一次
完整线性化表。libpng 的 8-bit 伽马表指数为

```
exponent = 1 / (gAMA * screen_gamma)          // png_reciprocal2
out      = round(255 * (v/255) ** exponent)
```

正确做法是让输出与文件编码匹配：`screen_gamma = 2.2`，`file` 直接传 gAMA
块的原始指数（sRGB 文件为 `0.45455`）。此时 sRGB→sRGB 的指数为
`1/(0.45455*2.2) = 1`，样本恒等通过；而线性文件（gAMA=1.0）才会被正确
编码到 sRGB（指数 `1/2.2`，样本变亮）。

实测同一像素：

| 图像 | 像素 | 缺陷输出 | 正确输出 |
|---|---|---|---|
| rgba8_gama22.png | #17 | (17,17,18) 被压成 (1,1,1) 类暗值 | (17,17,18) |
| ga8_linear.png（线性文件） | #33 | (1,1,1) | (74,74,74) |
| rgba16_gama22.png | #1 | R=1 | R=15（高字节），转换后 78 |

### 1.2 透明边缘杂色（透明像素颜色未清零）

libpng 规范上允许 alpha=0 的像素携带任意 R/G/B（调色板+tRNS、RGB+tRNS、
RGBA 都可能出现）。缺陷驱动原样保留这些字节，于是边缘露出陈旧颜色：

| 来源 | alpha=0 像素携带的颜色（缺陷） | 修复后 |
|---|---|---|
| palette8_trns.png（透明调色板项为红） | (255,0,0,0) | (0,0,0,0) |
| rgba8_gama22.png（边缘） | (255,0,128,0) | (0,0,0,0) |
| rgba8_srgb_adam7.png（隔行） | (0,255,255,0) | (0,0,0,0) |
| ga8_linear.png（透明白边） | (255,255,255,0) | (0,0,0,0) |

这些值在普通 alpha 合成时被背景掩盖，但在预乘、抠图、边缘拉伸或重新编码
时会渗出，形成杂色边。

## 2. 修复说明

两个修复都在驱动层 `src/png_decode.c`，通过编译开关与缺陷版共用同一份
libpng 调用骨架，差异严格限定在两处：

1. 伽马（`DECIDE_GAMMA`）：读取 `png_get_gAMA` 得到的 gAMA 指数直接作为
   `png_set_gamma` 的 file 参数，screen 固定为 sRGB 的 2.2；无 gAMA/sRGB
   块时使用 PNG 规范默认 `0.45455`。不再错误地取倒数或强制线性输出。
2. 透明清零（`ZERO_TRANSPARENT`）：解码完成后遍历每个像素，凡 alpha==0
   就把 R/G/B 置 0。部分透明（0<alpha<255）像素保留并正常做伽马校正，
   不受影响。

构建：

```
gcc -DDECIDE_GAMMA -DZERO_TRANSPARENT src/png_decode.c <libpng.a> -lz -lm
```

## 3. 复现与比对方法

一键脚本 `run.sh` 会静态编译当前树的 libpng、构建缺陷/修复两个驱动、
生成 6 张测试图、跑逐像素比对并渲染可视化结果。

测试语料（`corpus/`，16x16，覆盖各传输函数与 alpha 来源）：

| 文件 | 位深/类型 | 传输函数 | 隔行 | 透明来源 |
|---|---|---|---|---|
| rgba8_gama22.png | 8-bit RGBA | gAMA 0.45455 | 否 | 边缘 alpha=0 |
| rgba8_srgb_adam7.png | 8-bit RGBA | sRGB | Adam7 | 散布 alpha=0 |
| rgba16_gama22.png | 16-bit RGBA | gAMA 0.45455 | 否 | 整列 alpha=0 |
| ga8_linear.png | 8-bit Gray+Alpha | gAMA 1.0（线性） | 否 | 顶边 alpha=0 |
| palette8_trns.png | 8-bit 调色板 | gAMA 0.45455 | 否 | tRNS 透明项 |
| rgb8_trns_gama22.png | 8-bit RGB | gAMA 0.45455 | 否 | RGB tRNS 单色 |
| gray8_trns_gama22.png | 8-bit 灰度 | gAMA 0.45455 | 否 | 灰度 tRNS 单级 |

## 4. 比对结果（summary）

1792 个像素（7 图 x 256）上：

- 修复驱动 vs 独立参考实现：**7/7 图像、全部 1792 像素逐字节一致（PASS）**
- 缺陷驱动累计：1313 个像素存在伽马偏色，280 个全透明像素携带非零颜色，
  共 4352 个字节与参考不符
- Pillow 12.1.1 交叉验证：6 张 sRGB 编码图在“透明像素清零”后与修复驱动
  完全一致；线性文件 ga8_linear 因 Pillow 不做伽马转换，仅由参考实现校验

逐图明细见 `comparison.json`，可视化见 `visual/`：

- `*_compare.png`：棋盘背景上左缺陷/右修复的整体对比（左侧明显偏暗）
- `*_fringe_map.png`：直接显示每个像素颜色，白框标出 alpha=0，缺陷侧
  透明像素露出红/青/白等杂色，修复侧全黑
- `*_fringe_closeup.png`：左上角透明边缘放大

## 5. 边界正确性

- alpha=255：颜色仅做伽马转换，alpha 保留
- 0<alpha<255：颜色做伽马转换、alpha 保留、不被清零（196 个样本逐一校验）
- alpha=0：R/G/B 清零、alpha 保留为 0
- 16-bit：按 libpng `png_set_strip_16` 取高字节后走同一张 8-bit 伽马表
- Adam7：参考实现独立做 pass 切分与像素归位，与 libpng 结果一致
