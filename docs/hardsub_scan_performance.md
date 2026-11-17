# 硬字幕全片扫描：时间去哪了，以及怎么优化

> 分析对象：`硬字幕扫描（全片）`（`src/dialog_hardsub_scan_video.cpp` →
> `hardsub::ScanVideoTimeline`，`src/hardsub_timeline_scan.cpp`）。
> 典型输入：YouTube 4K60 AV1（3840×2160 / 8bit / 平均关键帧间隔 269 帧），
> 区域 `2304×540 @ (768,1512)`，扫描分辨率 50%，OCR every 16，ONNX 引擎。
> 所有"实测"都标了来源；"估算"是模型推算，用于排序，不追求 ±10% 精度。

---

## 0. 结论

现在的时间**不是花在 OCR 上**。按每 10 万帧折算（≈ 28 分钟 4K60 素材）：

| 时间去向 | 耗时 | 占比 | 性质 |
| --- | ---: | ---: | --- |
| 区域盒缩放 + YUV→RGB（每帧） | 650 s | **28%** | 可避免：只有 OCR 采样帧需要 RGB |
| 每条字幕重新从关键帧解码（含预卷） | 640 s | **27%** | 可避免：等于把片子再解一遍 |
| 主扫描解码（每帧一次） | 533 s | 23% | 硬下限（软件解码） |
| OCR 采样（检测+临时 PNG 往返） | 210 s | 9% | 一半以上是 PNG 编解码 |
| 整帧 YUV 拷贝（`ScanVideoDecoder`） | 120 s | 5% | 可避免：只需拷区域 |
| 变化率 + 8×8 签名（每帧两遍） | 100 s | 4% | 可与上一项融合 |
| 每行裁剪/覆盖率/OCR | 91 s | 4% | 结构性的重复劳动 |
| **合计** | **2,344 s** | 100% | |

一集 72 分钟（约 258k 帧）对应 **≈ 101 分钟**扫描时间。

三个真正的浪费：

1. **每帧都把区域转成 RGB**（`CropRegionScaled`），但只有 OCR 采样帧（1/16~1/24）
   需要 RGB；变化检测和签名只需要亮度。而且这个转换里**每输出像素做三次
   64 位整数除法**（亮度 1 次、色度 2 次，`hardsub_scan.cpp:270`、`:312-313`），
   在 311k 像素/帧的量级上就是几个毫秒。
2. **每条字幕结束后，从"rep 帧之前的关键帧"再解码一遍**（`hardsub_timeline_scan.cpp:653-901`）。
   关键帧间隔 269 帧 → 每条字幕平均白解 134 帧预卷，再解自己那 100~240 帧。
   全片加起来 ≈ **1.2 个额外整片解码**（总解码量 ≈ 2.2 遍全片）。
3. **每次 OCR 都写一个临时 PNG 再让引擎读回来**（`dialog_hardsub_scan_video.cpp:711-774`）。
   1152×216 RGB 的 PNG 编码约 20~40 ms，和 21 ms 的检测推理同量级甚至更贵。

优化后的量级（同一集 72 分钟 4K60）：

| 阶段 | 预计耗时 | 相对现在 |
| --- | ---: | ---: |
| 现在 | ~101 min | 1× |
| 快赢（OCR 内存传图、去重检测、复用关键帧） | ~94 min | 1.07× |
| + 每帧只用亮度、融合三遍、去整数除法 | ~68 min | 1.5× |
| + 单遍在线跟踪（干掉每行二次解码） | **~41 min** | 2.5× |
| + 分块多线程（4 线程） | **~15 min** | 6.7× |
| + 硬解 AV1 若真生效（RTX 4060 支持） | ~10 min | 10× |

---

## 1. 实测锚点

| 项目 | 数值 | 条件 / 来源 |
| --- | ---: | --- |
| FFMS 顺序解码 | **5.33 ms/帧** | 4K AV1 8bit、331 帧连续段、`build/_bench/ffms_bench.exe` |
| 关键帧 seek（解码 1 帧） | 6 ms | 同上 |
| 首帧随机访问 | 298-814 ms | 同上（含 FFMS 初始化，只发生一次） |
| 平均关键帧间隔 | **269 帧**（4.48 s @60fps） | 24,249 帧 / 90 关键帧 |
| 硬解开关对比 | hw=0 与 hw=1 **完全一致** | 说明该 bench 二进制没走硬解（见 §4-O8） |
| ONNX 检测（整区，含 PNG 读） | **21 ms** | `aegisub --ocr-selftest regdet.png --detect-only`，1600×180 → 限长 960 |
| ONNX 检测+1 行识别（整区） | 34 ms | 同上，`--backend onnx` |
| ONNX 单行识别（含检测器） | ~25 ms | 523×120 紧裁切 |
| PNG 编码 1152×216 RGB | **20~40 ms** | libpng 默认参数；PIL 代理实测 39 ms（`wxImage::SaveFile` 未设压缩选项，`imagpng.cpp:700-712` 走默认） |
| PNG 解码同图 | 3.8 ms | 同上 |
| 区域转 RGB | ~12 ns/像素 | 由 2026-08-29 实测反推：505 帧 4,777 ms，254k 像素/帧，减解码 → 裁剪+覆盖率 ≈ 3 ms |
| 整帧 YUV 拷贝 | ~1.2 ms（估算） | 4K 12.4 MB/帧，单线程 memcpy ~10 GB/s |

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

## 3. 时间去向（每 10 万帧）

| 分项 | 计算 | 耗时 |
| --- | --- | ---: |
| 主扫描解码 | 5.33 ms × 100k | 533 s |
| 整帧拷贝 | 1.2 ms × 100k | 120 s |
| 区域转 RGB | 6.5 ms × 100k | 650 s |
| 变化率+签名 | 1.0 ms × 100k | 100 s |
| OCR 采样 | 50 ms × 4,200 次 | 210 s |
| 每行二次解码 | 240 帧 × 5.33 ms × 500 行 | 640 s |
| 每行裁剪/覆盖率 | 0.3 ms × 120k 帧 | 36 s |
| 每行 OCR | 110 ms × 500 行 | 55 s |
| **合计** | | **2,344 s** |

估算里的两个变量，用户可以用状态栏/日志直接校准：

- OCR 采样次数：`Scan complete: N lines detected (OCR <S> samples, <T> s)`。
  连续运动镜头多时接近 `N/24`，静止对话多时接近字幕条数 × 2。
- OCR 引擎耗时：日志 `hardsub/scan/ocr: <S> samples, <ms> in the engine, <ms> encoding`。
  **`encoding` 那一项现在没有出现在状态栏**，建议一并显示（见 O0）。

---

## 4. 优化方案（按性价比排序）

### O0. 先加计时，再改代码（0.5 天）

现在只有 OCR 引擎与 PNG 编码两个计数器。建议在 `TimelineScanResult` 里加
`ScanProfile`：

```
frames_main, ms_decode, ms_copy, ms_convert, ms_change, ms_signature,
ocr_detect_calls, ms_detect, ms_png_encode, ms_png_decode,
rows, boundary_frames, ms_boundary, ms_row_ocr
```

`ScanVideoDecoder` 里用 `steady_clock` 记纯 `FFMS_GetFrame` 与拷贝两段；
`CropRegionScaled` 用现成的公开函数即可在外层记。收尾时 `LOG_I` 一行、
状态栏一行、并写进 `Scan complete` 文案。

另外加一个控制台模式（照抄 `--ocr-selftest` 的做法，`src/main.cpp:180`）：

```
aegisub.exe --hardsub-bench <video> --region x,y,w,h --scale 50 --max-frames 20000
```

这样这条流水线可以在 CI 和用户机器上无 GUI 复现、回归，也是后续每一步
优化的验收工具。（注意：本机不编译，构建交给 CI。）

### O1. OCR 走内存传图，删掉临时 PNG（1 天，省 ~9%）

- 现状：`detect`/`recognize` 先 `BuildImage`→`FitToMax`→`CreateTempFileName`→
  `SaveFile(PNG)`，后端再 `LoadImage` 解回 wxImage。每调用一次多花 25~45 ms。
- 改法：给 `ocr::OcrBackend` 加 `RunImage(OCRImageView const&, ...)` 重载
  （宽高 + RGB/RGBA 指针或 `std::vector<unsigned char>`）；ONNX 后端直接用
  `wxImage(w, h, data, false)` 包装（必要时 `SetData` 拷贝一次），Paddle 后端
  内部仍落盘。`hardsub::RegionImage` 已经是这个形状，可直接传。
- 顺带修掉 `wxFileName::CreateTempFileName` 每帧建文件的行为——在开着 Defender
  的机器上，这是几千次"创建+写入+删除"的实时扫描。
- 验收：`--hardsub-bench` 的 `ms_png_encode` 归零；同一批帧的文本/置信度不变。

### O2. 识别只跑识别器，并把 2× 双三次上采样去掉（1 天，省 ~2-3%）

- 现状：`recognize` 用 `OcrMode::Full`，输入是"已知一行"的紧裁切，却仍先跑
  一遍 DB 检测器；高度 <64 的裁切还会先做 2× 双三次放大（`dialog_hardsub_scan_video.cpp:754`），
  放大后的图再被识别器归一化到 48 px 高，白做。
- 改法：加 `OcrMode::RecOnly`——整张图当一行，按 `48 × round(48*w/h)` 直接喂
  识别器（宽按 32 对齐），跳过检测；扫描路径不再放大。
- 验收：单行识别 25 ms → 8~12 ms；真实帧 A/B 比对文本与置信度。

### O3. 每行不要再检测一次（0.5 天，省 ~2%）

- 现状：`:678` 对 rep 帧再跑一次整区检测，只为把框对齐。
- 改法：track 已经存了 `region_rect`；用"该 track 最近 2~3 次检测框的中位数"
  代替重新检测。`read_frame` 返回的 `OCRResult::lines` 里本来就带 box，
  可以顺手更新。
- 验收：结果行数/边界不变。

### O4. 边界扫描改用真实关键帧表（0.5 天，长字幕收益明显）

- 现状：`:870` 传的是 `std::vector<int>{}` → `ScanBoundaries` 用 120 帧兜底
  预卷；长字幕每回溯一次就是一次 120 帧的"回跳+重解"。
- 改法：`ScanVideoDecoder::GetKeyFrames()` 已经实现（`scan_video_decoder.cpp:133`）
  但从没人调用；把它通过 `TimelineScanOptions` 或新参数传进 `ScanBoundaries`
  （单条扫描对话框已经这么做了）。
- 验收：`--hardsub-bench` 里 `boundary_frames` 对长字幕明显下降；边界不变。

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

### O8. 验证硬解是否真的生效（0.5 天，潜在 2-3×解码）

本项目 fork 的 ffms2 对 AV1/VP9 走 D3D11VA/DXVA2（`subprojects/ffms2/src/core/videosource.cpp:259-320`），
但 8 月的 `ffms_bench.exe` 里 hw=0/1 完全一致，说明那个二进制没走硬解。
本机是 RTX 4060 Laptop（Ada，支持 AV1 硬解），值得在 O0 的计时里单独看
`ms_decode`：若 5.33 ms 没降，说明 `av_hwdevice_ctx_create` 失败或回拷没走通；
注意硬解后仍要 `av_hwframe_transfer_data` 回拷（4K ~8-12 MB/帧），净收益可能
只有 2-3×，不是 10×。

### O9. 区域带代理缓存（迭代体验，2 天）

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
| — | 现在 | ~101 min | 1.0× |
| O0 | 加计时 + `--hardsub-bench` | ~101 min（先能测） | 1.0× |
| O1-O4 | OCR 内存传图、RecOnly、去重检测、关键帧表 | ~94 min | 1.07× |
| O6 | 亮度优先 + 融合 + 去整数除法 | ~68 min | 1.5× |
| O5 | 单遍在线跟踪 | **~41 min** | 2.5× |
| O7 | 分块 4 线程 | **~15 min** | 6.7× |
| O8 | 硬解（若生效） | ~10 min | 10× |
| O9 | 区域带缓存（重扫） | 重扫 1-3 min | — |

注意 O1-O4 单独看收益不大（约 7%），但它们是 O7 并行的前提：
OCR 串行化后，每调用 50 ms 的 PNG 往返会成为并行阶段的瓶颈。

---

## 6. 现在就能做的（配置 + 小改动）

1. **扫描分辨率 50% → 33%/25%**：区域转换随像素数线性下降，是目前最大的一档
   开关（28% 的那一项）。代价：检测器看到的字更小（见下条）。
2. **把"像素扫描分辨率"和"OCR 输入分辨率"解耦**（这个要改代码，但很小）：
   现在 `detect(prev)` 用的是缩放后的 region，尺度一降，检测/识别质量就掉，
   用户只好倍着用 100%。应该让像素扫描用 50%/25%，而 OCR 永远用全分辨率行裁切。
3. **区域收窄**：只框字幕带（高度越接近字形高度越省），把台标/UI 排除在外。
4. **OCR every 16 → 24/32**：采样次数直接线性下降，代价是漏掉更短的闪现字幕。
5. 看日志里的 `hardsub/scan/ocr: N samples, X ms in the engine, Y ms encoding`：
   Y 和 X 差不多大就说明 O1 值得先做。

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
