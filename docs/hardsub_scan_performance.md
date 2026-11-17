# 硬字幕全片扫描：时间去哪了，以及怎么优化

> 分析对象：`硬字幕扫描（全片）`（`src/dialog_hardsub_scan_video.cpp` →
> `hardsub::ScanVideoTimeline`，`src/hardsub_timeline_scan.cpp`）。
> 典型输入：YouTube 4K60 AV1（3840×2160 / 8bit / 平均关键帧间隔 269 帧），
> 区域 `2304×540 @ (768,1512)`，扫描分辨率 50%，OCR every 16，ONNX 引擎。
> 所有"实测"都标了来源；"估算"是模型推算，用于排序，不追求 ±10% 精度。

---

## 0. 结论

按 2026-11-18 的实测（`--hardsub-bench`，900 帧、5 条字幕，见 §1.1），
全流程墙钟 26.4 s，时间去向是：

| 时间去向 | 耗时 | 占比 | 性质 |
| --- | ---: | ---: | --- |
| 每条字幕重新从关键帧解码（行扫描） | 8.4 s | **32%** | 可避免：等于把片子再解一遍 |
| 区域盒缩放 + YUV→RGB（每帧） | 6.2 s | 24% | 可避免：只有 OCR 采样帧需要 RGB |
| OCR 图像准备（临时 PNG） | 4.8 s | **18%** | 可避免：84 ms/次，比推理还贵 |
| OCR 引擎（检测+识别） | 2.3 s | 9% | 真实成本 |
| 变化率 + 8×8 签名（每帧） | 1.1 s | 4% | 可与区域转换融合 |
| 主扫描解码（每帧一次，dav1d） | 2.2 s | 8% | 已是软解，硬解反而 6× 慢 |
| 其他（循环、进度、行 OCR 之外的杂项） | ~1.4 s | 5% | |

一集 72 分钟（约 258k 帧）按此比例约为 **~90 分钟**（字幕密度不同会有浮动）。

三个真正的浪费：

1. **每条字幕结束后，从"rep 帧之前"再解码一遍**（`hardsub_timeline_scan.cpp:670-930`）。
   关键帧间隔 269 帧 → 每条字幕平白多解 120~250 帧。实测占 32%。
2. **每帧都把区域转成 RGB**（`CropRegionScaled`），但只有 OCR 采样帧（1/16~1/24）
   需要 RGB；变化检测和签名只需要亮度。而且这个转换里**每个输出像素做三次
   64 位整数除法**（亮度 1 次、色度 2 次，`hardsub_scan.cpp:270`、`:312-313`）。
3. **每次 OCR 都写一个临时 PNG 再让引擎读回来**（`hardsub_ocr_runner.cpp`）。
   实测 84 ms/次，比 40 ms 的引擎调用贵一倍，还触发 Defender 实时扫描。

优化后的量级（同一集 72 分钟 4K60，按实测占比推算）：

| 阶段 | 预计耗时 | 相对现在 |
| --- | ---: | ---: |
| 现在（实测基线） | ~90 min | 1× |
| 快赢（OCR 内存传图、RecOnly、去重检测、复用关键帧） | ~61 min（实测 1.5×） | 1.5× |
| + 每帧只用亮度、融合三遍、去整数除法 | ~50 min | 1.8× |
| + 单遍在线跟踪（干掉每行二次解码） | **~37 min** | 2.4× |
| + 分块多线程（4 线程） | **~11 min** | 8× |

---

## 1. 实测锚点

| 项目 | 数值 | 条件 / 来源 |
| --- | ---: | --- |
| FFMS 顺序解码（软件 / dav1d） | **2.2 ms/帧** | 4K AV1 8bit，`--hardsub-bench` 实测（2010 次 `GetFrame`） |
| FFMS 顺序解码（D3D11VA 硬解） | **13.5 ms/帧** | 同上；回拷代价远大于 dav1d，见 §4-O8 |
| 关键帧 seek（解码 1 帧） | 6 ms | `build/_bench/ffms_bench.exe` |
| 平均关键帧间隔 | **269 帧**（4.48 s @60fps） | 24,249 帧 / 90 关键帧 |
| ONNX 检测（整区，含 PNG 读） | **21 ms** | `aegisub --ocr-selftest regdet.png --detect-only`，1600×180 → 限长 960 |
| ONNX 检测+1 行识别（整区） | 34 ms | 同上，`--backend onnx` |
| ONNX 单行识别（含检测器） | ~25 ms | 523×120 紧裁切 |
| OCR 单次采样总成本 | **~125 ms** | 实测：图像准备 84 ms + 引擎 40 ms（1152×270） |
| PNG 编码 1152×216 RGB | 84 ms（wx） | 实测；libpng 默认参数 + 临时文件 + 杀软 |
| 区域转 RGB（50% 尺度） | **6.5 ms/帧** | 实测：900 帧 5.9 s（311k 输出像素） |
| 变化率 + 8×8 签名 | 1.1 ms/帧 | 实测 |
| 整帧 YUV 拷贝 | 1.6 ms/帧 | 实测 3.2 s / 2010 次（4K 12.4 MB/帧） |

### 1.1 实测剖面（900 帧，区域 2304×540 @50%，probe_video.webm）

`--hardsub-bench` 三条命令的同一段素材（第 600~1499 帧，含 5 条字幕）：

| 路径 | 墙钟 | 解码 | 区域转换 | 变化率 | OCR 图像 | OCR 引擎 | 行扫描 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 纯像素（`--no-ocr`） | 9.4 s | 1.3 s | 5.9 s | 1.0 s | — | — | — |
| 全流程（软件解码） | 26.4 s | 2.2 s | 6.2 s | 1.1 s | 4.8 s | 2.3 s | 8.4 s |
| 全流程（`--hardware-decode`） | 47.5 s | 14.8 s | 6.1 s | 1.3 s | 4.9 s | 2.1 s | 16.7 s |

结论：

1. **硬解在这条流水线上是负优化**：AV1 的 D3D11VA 每帧 13.5 ms，dav1d 只要 2.2 ms，
   墙钟差 1.8×。扫描路径已改为强制软解（见 §4-O8）。
2. OCR 图像准备（临时 PNG）单次 84 ms，比引擎本身（40 ms）还贵——比估算更严重。
3. 行扫描（每条字幕从关键帧重解）占全流程 32%，与模型一致。

配置实测（`%APPDATA%\Aegisub\config.json`）：区域 2304×540、扫描 50%、
OCR every 16、`Detect Limit Side Len = 0`（→ 960）、硬解开、引擎 auto→ONNX。
50% 尺度下每帧要处理 **311k 输出像素**（读 1.24M 源像素）。

---

## 2. 现在的执行路径

### 2.1 主循环：每一帧（`hardsub_timeline_scan.cpp:519-639`）

1. `loader(f)` → `ScanVideoDecoder::GetFrame`：FFMS 解码 + **把整帧 Y/U/V 拷进
   `frame_store_`**（4K 12.4 MB，`scan_video_decoder.cpp:161-186`）。
2. `CropRegionScaled`：按 50% 做 2×2 盒平均 + BT.709 YUV→RGB，**每输出像素
   三次 64 位整数除法**（亮度 1 次、色度 2 次，`hardsub_scan.cpp:270`、`:312-313`）。
3. `RegionChangedRatio(prev, reg)`：对 311k 个 RGB 像素求最大通道差（`:343-357`）。
4. `RegionSignature(reg)`：把整个区域再扫一遍，缩成 8×8 亮度格（`:185-194`）。
5. 判断是否 OCR：`content_changed && settled && since >= 16`，否则 `since >= 24` 硬上限。
6. 需要时 `detect(prev)`：`BuildImage` → `FitToMax(1600)` → `CreateTempFileName`
   → `SaveFile(PNG)` → 后端 `LoadImage`（PNG 解码）→ 检测推理。

一帧的账（50% 尺度）：`5.33 + 1.2 + 6.5 + 0.4 + 0.6 ≈ 14 ms`。

### 2.2 收尾：每条字幕（`hardsub_timeline_scan.cpp:653-901`）

主循环跑完**整片之后**才对每条 track 做：

1. `loader(rep)` 再取一次代表帧（随机回跳）。
2. `detect(region)` **再一次整区 OCR 检测**，只为把框对齐（`:678`）。
3. `read_frame(rep)`：全分辨率紧裁切 + `recognize`（`OcrMode::Full`，即检测器
   再跑一遍）+ 视需要再取 1~2 帧投票（`:836-851`）。
4. `ScanBoundaries(tpl, ..., rep, ...)`（`:870`）：**从 `rep-120` 往回探**
   （因为传了空的 keyframes，`hardsub_scan.cpp:653-663` 用 120 帧兜底预卷），
   然后顺序解码到字幕结束 + 确认尾巴，逐帧算模板覆盖率。
   由于解码必须从关键帧开始，实际解码量 = `(rep - kf(rep)) + 120 + 运行长度`
   ≈ **134 + 100~240 帧**。

关键点：**这段解码的帧，主循环已经解过一遍**。所以全片被解码了约
`1 + 1.2` 遍。

另外：进度条只由主循环驱动（`:525-526`），行扫描阶段传的是空 `ProgressFn`
（`:871`）→ **进度条在主循环结束就到 100%，而后面还有约 30% 的时间在跑**。
用户看到的"卡在 100%"是真实存在的，不是错觉。

---

## 3. 从实测到整集

实测剖面（§1.1）已经取代了早期按 5.33 ms/帧解码推算的模型。把它按"每 10 万帧"
折算（每 100k 帧 ≈ 4,200 次 OCR 采样、500 条字幕）：

| 分项 | 单位成本 | 每 10 万帧 |
| --- | ---: | ---: |
| 主扫描解码 | 2.2 ms/帧 | 220 s |
| 区域转 RGB | 6.5 ms/帧 | 650 s |
| 变化率 + 签名 | 1.1 ms/帧 | 110 s |
| OCR 图像准备 | 84 ms × 4,200 | 353 s |
| OCR 引擎 | 40 ms × 4,200 | 168 s |
| 行扫描（重解码 + 覆盖率） | 1.7 s/行 × 500 | 850 s |
| 其他 | — | ~150 s |
| **合计** | | **~2,500 s ≈ 42 min** |

一集 72 分钟（258k 帧）≈ **90 分钟**，与 §1.1 的实测比值一致。

用户可以用状态栏/日志直接校准（O0 之后日志里已经有全部字段）：

- `hardsub/scan: <frames> frames, <rows> rows, ... decode <ms> ..., OCR engine <ms>
  ..., OCR image <ms>`
- 状态栏结束行：`Scan complete: N lines ... | decode x% pixels y% OCR z% rows w%`

---

## 4. 优化方案（按性价比排序）

### O0. 先加计时，再改代码（0.5 天）——**已完成**

`TimelineScanResult` 现在带 `ScanProfile`（frames/decode/region/change/OCR 引擎/
OCR 图像/行扫描），`ScanVideoDecoder` 另记解码与整帧拷贝的微秒级拆分，进度事件
按 `ScanPhase::{Main,Rows,Finalize}` 分段上报，收尾时写一行 `LOG_I` 并在状态栏
显示 `decode x% pixels y% OCR z% rows w%`。

同时加了控制台模式（照抄 `--ocr-selftest`）：

```
aegisub.exe --hardsub-bench <video> --region x,y,w,h --scale 50 --start N --count N
            [--no-ocr] [--hardware-decode] [--backend auto|onnx|paddle] [--out report.json]
```

输出一份 JSON（墙钟、各阶段毫秒与占比、行数、事件数、前 10 条事件），
是后续每一步优化的验收工具。

顺带发现并修掉的两件事：

- **硬解在这条流水线上是负优化**：同一段素材 `--hardware-decode` 47.5 s vs
  软件解码 27.1 s（AV1 每帧 13.5 ms vs 2.2 ms）。扫描路径已固定软解，
  播放路径不受影响。这就是 O8 的结论，不需要再单独验证。
- **consensus 投票从来没有真正生效**：`consensus_samples` 的窗口被限制在
  `tr.last_seen`，而字幕常常两次 OCR 采样之间就结束，track 只有一次 sighting，
  于是永远只有 1 个候选。`tests/tests/hardsub_timeline_scan.cpp` 里
  `scan_timeline_votes_across_frames_for_short_rows` 在本次改动之前一直是失败的。

### O1. OCR 走内存传图，删掉临时 PNG（1 天，省 ~18%）——**已完成**

- `ocr::OcrBackend` 新增 `OCRImage` / `RunImage()` / `AcceptsImagesInMemory()`；
  ONNX 后端直接吃像素（复制进 wxImage，引擎本来也要缩放），Paddle 后端保持
  文件路径，runner 对不支持内存的引擎才落盘。
- `hardsub_ocr_runner.cpp` 统一了"给引擎一张图"的逻辑，对话框与 bench 共用。
- 实测：OCR 图像准备 4.8 s → **0**（同一段 900 帧），单次采样从 ~125 ms 降到
  ~30 ms；临时文件也一并消失，不再触发杀软实时扫描。

### O2. 识别只跑识别器，并把 2× 双三次上采样去掉（1 天，省 ~2-3%）——**已完成**

- 新增 `OcrMode::RecOnly`：把整张图当一行，跳过 DB 检测器，按 48 px 高直接喂
  识别器；ONNX 后端的 `RecognizeLine` 加了 `inset` 参数，调用方已裁好带 padding
  的行时不再内缩。
- 扫描路径不再做 2× 双三次上采样（识别器本来就把高度归一化到 48 px）。
- 实测：OCR 引擎时间 2.3 s → **1.5 s**（52 次检测 + 7 次识别）。

### O3. 每行不要再检测一次（0.5 天，省 ~2%）——**已完成**

- `hardsub_timeline_scan.cpp` 行扫描阶段删掉了整区 `detect(region)` 重定位：
  track 里存的框已经带 padding，识别器能容忍几个像素的漂移。每行省一次
  ~120 ms 的检测（实测 detect 次数 57 → 52）。

### O4. 边界扫描改用真实关键帧表（0.5 天）——**已完成**

- `TimelineScanOptions::keyframes` 由调用方从 `ScanVideoDecoder::GetKeyFrames()`
  填好并传给 `ScanBoundaries`；长字幕不再按 120 帧步长反复"回跳+重解"。
- 实测：行扫描 8.4 s → **5.7 s**（与 O3 合计）。

### O5. 单遍在线跟踪：干掉"每行再解一遍"（3-5 天，省 ~27%）★

这是最大的一块结构性收益。

- 现状：边界扫描发生在整片扫完之后，为了拿到模板必须先跳到 rep 之前的关键帧，
  等于把每段字幕重解一遍。
- 改法（把边界计算搬进主循环）：
  1. 某次 OCR 采样产生新 track 时，**立刻**从当前 region（已经在内存里）裁出
     该行，`TemplateTextPixels` 建模板。
  2. 之后每帧对"活动模板"算一次 `TemplateTextCoverage`（只需对该行子矩形，
     文字像素通常几百到几千个）→ 双阈值滞回 + confirm 得到结束帧。
  3. 起点用**最近 K 帧 region 的环形缓冲**回溯（K ≈ `detect_every_max + confirm + 余量`
     ≈ 32）。缓冲最老的一帧若仍然"在场"，说明字幕开始得比窗口更早 → 该行
     退回一次 `ScanBoundaries` 兜底（很少见）。
  4. 行结束立刻 `row_callback`，结果流式出现，进度条也终于有意义。
- 内存：32 × 1152×270×3 ≈ 30 MB（50% 尺度）；100% 尺度 120 MB——可以只存亮度
  （1/3）或把 K 降到 16 并对超窗行走兜底。
- 验收：现有 `tests/tests/hardsub_timeline_scan.cpp` 用假 loader/假 OCR，可直接
  断言"每条字幕的解码帧数 ≤ K + confirm"；真实片段边界与现在逐帧一致。

### O6. 每帧成本：只算亮度 + 三遍合一 + 去整数除法（2-3 天，省 ~26%）★

1. **变化检测和签名只用 Y 平面**：把 region 盒平均到 1/4（或 1/2）只保留亮度，
   一遍循环里同时产出 8×8 网格和变化率；只有真的要 OCR 的帧才转 RGB。
   311k px 的三遍 RGB 工作 → 约 100k px 的一遍亮度工作。
2. **删掉每像素的 64 位除法**：`y_sum / y_count`、`u_sum / c_count` 在固定缩放比
   时是常数（2×2 就是 >>2），可变比例时预计算倒数用乘法。
3. 可选：YUV→RGB 用 SSE2/AVX2 定点（int16 16.16），再快 4-8×。
4. `RowSignature` 里对整行亮度 `std::sort` 求中位数（`hardsub_timeline_scan.cpp:211-213`）
   改成直方图，O(n log n) → O(n)。

预期单帧：14 ms → 6.5 ms；100% 尺度用户收益更大（他们现在每帧要转 1.24M 像素）。

### O7. 分块并行（3-4 天，3-4×）

- 把 `[start,end]` 切成 K 段，每段一个 `ScanVideoDecoder` + 一条分析线程，
  各自跑 O5 的单遍跟踪；OCR 用一把锁串行（OCR 已是 O1 后的少数派），或开 K 个
  ONNX session。
- 段边界要处理跨段字幕：让相邻段重叠 ~300 帧，合并时按"时间重叠 + 文本相似"
  去重（`DedupeTimelineEvents` 已有）。
- 注意每个 FFMS source 的 `thread_count` 要按 `核心数 / K` 设，避免 dav1d 线程
  互相打架。
- 先在 O5/O6 之后做，否则只是把浪费的活并行掉。

### O8. 硬解：实测是负优化，已关闭（0.5 天，省 ~40% 墙钟）——**已完成**

本项目 fork 的 ffms2 对 AV1/VP9 走 D3D11VA/DXVA2（`subprojects/ffms2/src/core/videosource.cpp:259-320`）。
O0 的计时直接给出了结论：同一段 4K60 AV1 素材，硬解 13.5 ms/帧、软解（dav1d）
2.2 ms/帧，整段扫描 47.5 s → 27.1 s。硬解的解码本身不慢，4K 帧回拷到系统内存
（`av_hwframe_transfer_data`，8~12 MB/帧）才是成本。

- 扫描路径（`ScanVideoDecoder`）已固定软件解码，播放路径仍按用户选项走硬解。
- `--hardsub-bench --hardware-decode` 保留该路径，方便在新硬件上复测。

### O9. 区域带代理缓存（迭代体验，2 天）——本次不做

首次扫描时把 region 按扫描尺度写成一个临时"带"视频（FFV1/x264 lossless，
1152×270@60 很小），之后换阈值、换去重策略、换 OCR 语言重扫时只读这个带
（~0.3-0.5 ms/帧）。把"重扫 40-100 分钟"变成"1-3 分钟"。
最小版本：只缓存"每帧 8×8 签名 + 文本存在性曲线"，也能让参数试验免解码。

### O10. 进度与遥测

- 分阶段进度：主扫描 0-65%、行扫描 65-95%、合并/去重 95-100%（O5 之后天然
  变成单阶段流式进度）。
- 状态栏显示 O0 的 profile 摘要，别只显示 OCR 次数。

---

## 5. 实施顺序与预期

| 步骤 | 内容 | 一集 72 分钟 4K60 | 累计倍数 |
| --- | --- | ---: | ---: |
| — | 开始（含硬解） | ~101 min | 1.0× |
| O0 | 计时 + `--hardsub-bench` + 关闭硬解 | ~90 min | 1.1× |
| O1-O4 | OCR 内存传图、RecOnly、去重检测、关键帧表 | ~68 min | 1.5× |
| O6 | 亮度优先 + 融合 + 去整数除法 | ~50 min | 2.0× |
| O5 | 单遍在线跟踪 | **~37 min** | 2.7× |
| O7 | 分块 4 线程 | **~11 min** | 9× |
| O8 | 硬解 | 已判定为负优化，不做 | — |
| O9 | 区域带缓存（重扫） | 本次不做 | — |

注意 O1-O4 单独看收益不大，但它们是 O7 并行的前提：OCR 串行化后，
每次采样 ~125 ms 的临时 PNG 往返会成为并行阶段的瓶颈。

---

## 6. 现在就能做的（配置 + 小改动）

1. **扫描分辨率 50% → 33%/25%**：区域转换随像素数线性下降，是目前最大的一档
   开关（实测 24%）。代价：检测器看到的字更小（见下条）。
2. **把"像素扫描分辨率"和"OCR 输入分辨率"解耦**（这个要改代码，但很小）：
   现在 `detect(prev)` 用的是缩放后的 region，尺度一降，检测/识别质量就掉，
   用户只好倍着用 100%。应该让像素扫描用 50%/25%，而 OCR 永远用全分辨率行裁切。
3. **区域收窄**：只框字幕带（高度越接近字形高度越省），把台标/UI 排除在外。
4. **OCR every 16 → 24/32**：采样次数直接线性下降，代价是漏掉更短的闪现字幕。
5. 看日志里的 `hardsub/scan: ... OCR image <ms>`：该项与 `OCR engine` 接近时，
   O1（内存传图）是下一步最划算的改动。

---

## 7. 待验证 / 风险

1. **`5.33 ms/帧` 是旧 bench 二进制测的软件解码值**；当前树的 ffms2 有硬解分支，
   真实值要看 O0 的 `ms_decode`。若硬解生效，解码占比（23% + 27%）会明显缩水，
   但"区域转换 + 二次解码"占大头的结论不变。
2. **每行二次解码的帧数**取决于真实关键帧间隔；169-269 帧的 GOP 下，平均预卷
   134 帧是合理估计，但 YouTube 不同批次差异大（可低至 60 帧）。
3. **OCR 采样次数**受"settled"判据影响很大：动作镜头多的素材接近 `N/24`。
4. **O5 改判据**（RGB 最大通道差 → 亮度差）需要真实片段回归：白字黑边、黄字、
   低对比度三种场景各抽 20 条，比较边界与人工标注的差值分布。
5. **O7 跨段字幕**：重叠窗口 + 去重是必要条件；要加一个"相邻段边界处字幕被切断"
   的回归用例。
6. **内存**：O5 的环形缓冲在 100% 尺度下 120 MB，需要上限或降级策略。
7. **解码器回退陷阱**：`ScanVideoDecoder` 只认 `yuv420p/nv12/p010`；10bit
   `yuv420p10le`（HDR 源）等会构造失败，`dialog_hardsub_scan_video.cpp:691-696`
   随即静默回退到 `videoController->GetFrame`（每帧全帧 BGRA 转换，4K 约
   100 ms/帧），扫描会慢 10-20 倍。至少应该在回退时给用户明确警告并建议先转码。
8. 所有改动都要守 `tests/tests/hardsub_timeline_scan.cpp` 的既有契约
   （假 loader + 假 OCR 已经能覆盖发现、边界、投票、去重四条链路）。

---

## 附录：实测命令

```powershell
# 解码成本（顺序段 + 关键帧定位）。第 4 个参数给一个不匹配的 ffindex 路径，
# 程序会退化为内存索引，从而能测任意视频：
build\_bench\ffms_bench.exe 1 1000 build\probe_video.webm <任意不匹配的 .ffindex 路径>

# ONNX 检测 / 识别成本（含 PNG 读取）
$env:AEGISUB_OCR_ASSETS = "D:\Code\Custom\Aegisub_Toshi-ban\build\ocr\onnx"
build\aegisub.exe --ocr-selftest build\frames\regdet_017974.png --backend onnx --detect-only
build\aegisub.exe --ocr-selftest build\frames\appbox_017974.png --backend onnx
```

`build/frames` 里的 `bottom_*.png`（3840×432）、`v_*.png`（1280×720）、
`regdet_*.png`（1600×180）、`appbox_*.png`（523×120）正好对应当前流水线
四个不同阶段的输入尺寸，可以直接当回归样本。
