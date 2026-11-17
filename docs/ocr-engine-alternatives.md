# 硬字幕 OCR 引擎选型：PaddleOCR 之外的本地方案

> 状态：调研文档，未实施。日期 2026-11-17。
> 目标：在不引入 Python、不联网、保持 Windows x64 便携分发的约束下，让硬字幕 OCR
> 更准（短句、低对比度）、更快（全片扫描）、更省（安装包与内存）。

## 0. 结论

1. **拖后腿的不是「PaddleOCR」，而是三件可以被替换的东西**：
   - `PaddleOCR-json v1.4.1`（Paddle Inference + MKL + OpenCV 构建）这个旁路进程，
     `ocr/bin` 就有 **304 MB**；
   - **PP-OCRv5 *server*** 模型（det 84 MB + rec 81 MB，共 165 MB），是 v5 家族里最重的一档；
   - 每次调用的方式：写 PNG 临时文件 → 子进程 → 读 JSON 行。
2. **最划算的一条路：进程内 ONNX Runtime + PP-OCRv6 small**（det 9.6 MB + rec 20.4 MB）。
   预期整包 **471 MB → 约 40 MB**，单帧 **150–350 ms → 20–50 ms**，且官方基准里
   v6 small 的英文/日文识别都优于 v5 server。
3. **只想动配置、不动架构**也有一批立刻能拿的收益：`limit_side_len`、跳过 `cls`、
   把「整条行框」换成「紧贴文本的行裁切」、事件级多帧投票与去重。
4. **不推荐**：OneOCR（微软模型不允许再分发）、Tesseract（CJK 硬字幕表现差）、
   manga-ocr / 漫画检测器（场景是气泡竖排，不是横向硬字幕）、VLM 系（单行图 1–3 s，
   而且短行幻觉比 CTC 更重，只适合做少量事件的二次校验）。

## 1. 现状与约束（从代码里读出来的）

| 项目 | 现状 |
| --- | --- |
| 全片扫描输入 | 用户框选区域（典型 4K 下 3840×432）→ `FitToMax(1600)` → 临时 PNG → 常驻 `PaddleOCR-json` → JSON（`src/dialog_hardsub_scan_video.cpp:738-787`） |
| 单条扫描输入 | 模板行裁切，`scale=2` 后再 `FitToMax(1600)` 走同一路径（`src/dialog_hardsub_scan.cpp`） |
| 输出契约 | 很小：`ocr::OCRResult{ok, code, text, lines[{text, confidence, box}]}` + `OCROptions{language, keep_line_breaks}`（`src/ocr/ocr_result.h`） |
| 引擎接口 | `OCREngine`（一次性 CLI）+ `OCRProcess`（常驻管道）两套，都只认 PaddleOCR-json 的 JSON 协议 |
| 语言 | 只有一个 `japanese` profile，字典是 `ppocrv5_dict.txt`（18 383 条，含拉丁与空格） |
| 实际素材 | `build/frames` 的 44 张样帧全部是**英文**硬字幕（底特律的游戏内字幕），不是日文 |
| 平台 | Windows x64、离线、无 Python 依赖、CI 打包（`tools/ocr/download_paddleocr_windows.ps1`） |

关键结论：**OCR 后端的耦合面只有一个 `OCRResult` 结构**，换引擎的架构成本远低于直觉；
真正贵的是模型资产与打包流程。

## 2. 本机实测基线

环境：Ryzen 7 7745HX（16 线程）、Windows、`build/ocr` 现成产物、
`build/frames` 的真实 4K 帧（3840×432 底部条带）。数字取中位数，用于量级对比。

| 项目 | 实测 |
| --- | --- |
| `ocr/bin` 体积 | 304.4 MB（paddle_inference 107.4 + mklml 88.4 + opencv_world 61.7 + mkldnn 43.8 + 其他） |
| `ocr/models` 体积 | 166.4 MB（det 84.2 + rec 80.8 + cls 1.4） |
| 模型加载（init） | 1.5–2.5 s |
| 纯检测 3840×432 | 136 ms |
| 全流程 3840×432 | 151 ms（无文字帧，识别阶段被跳过） |
| 全流程，含 1 行文字的行裁切 523×120 | 154 ms |
| 全流程，同一裁切放大 2×（1046×240） | 348 ms |
| 纯识别（`-det=false`）同一裁切 | 43–47 ms |
| PNG 编码 1600×180 | 34 ms |
| PNG 编码 3840×432 | 175 ms |

三条可直接利用的结论：

1. **检测（det）是主要开销**，且对输入尺寸极度敏感：同一行文字放大 2× 后，全流程
   从 154 ms 涨到 348 ms。而识别（rec）本身只有 5–15 ms。
2. **每次调用还在「找框」**：全片扫描对每条事件只在已知的行框上跑一次全流程
   （`hardsub_timeline_scan.cpp:563-607`），等于为了读一行字又跑了一遍检测。
3. **PNG 往返不是小数**：1600 宽的条带编码就要 34 ms，4K 条带 175 ms——和推理同量级。

另外，`limit_side_len` 实测（det-only，同一批图）：

| 输入 | 默认 960 | `limit_side_len=480` | `=320` |
| --- | --- | --- | --- |
| 523×120 行裁切 | 89 ms | 76 ms | 53 ms |
| 1046×240 行裁切 | 252 ms | 67 ms | 45 ms |
| 3840×432 条带 | 171 ms | 73 ms | 54 ms |

> 与上表不是同一次运行，绝对值有 ±20% 波动，看相对比例即可；`=320` 档返回的文本框
> 数量已经和默认档不同（合并或漏检），需要按素材逐档验证后才能当默认值。

降到 480 能让检测快 2–3 倍，但小字/低对比度会开始漏（必须按素材调，不能盲目取小）。
反过来，把 `limit_side_len` 提到 1280 能消掉一条误检（`bottom_018000.png` 上多出的
`c(0.37)`）并修好漏空格（`Ah,thelittle one…` → `Ah,the little one…`），代价是单帧 +45%。

## 3. 候选方案对比

### 3.1 模型能力（官方基准，PaddleOCR 3.x 文档）

| 模型 | 体积 | 检测 avg Hmean | 识别 avg | 英文印刷体 | 日文 | CPU 推理（ORT，单图） |
| --- | --- | --- | --- | --- | --- | --- |
| PP-OCRv5_server det/rec | 84.3 + 81 MB | 81.6 | 78.1 | 85.1 | 73.7 | det 25.9 ms / rec 6.0 ms |
| PP-OCRv6_small det/rec | **9.6 + 20.4 MB** | 84.1 | 81.3 | 93.3 | 88.2 | det 21.5 ms / rec 4.5 ms |
| PP-OCRv6_medium det/rec | 59.4 + 73.3 MB | 86.2 | 83.2 | 94.1 | **90.5** | det 26.2 ms / rec 5.0 ms |
| PP-OCRv6_tiny det/rec | 1.9 + 4.4 MB | 80.6 | 73.5 | 88.4 | 不支持 | det 20.5 ms / rec 3.1 ms |

> 注意：v6 指标来自官方内部多场景集，v5 来自通用集，两组数字不能直接相减；
> 但「v6 small 用约 1/5 的体积在英文/日文上超过 v5 server」这个方向是可靠的，
> 且 tiny 明确不支持日文，不能拿来顶。

### 3.2 方案矩阵

| 方案 | 运行时/模型体积 | 单帧时延（估） | 准确率 | 许可 | 集成成本 | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| **ONNX Runtime + PP-OCRv6 small（进程内）** | ORT ≈ 12–15 MB + 30 MB 模型 | 20–50 ms | 英文/日文均优于现状 | Apache-2.0 / MIT | 中（需实现 det 前后处理 + CTC 解码） | **首选** |
| ONNX RT + PP-OCRv6 medium（进程内） | + 133 MB 模型 | 40–80 ms | 最高 | 同上 | 中 | 单帧 Image2Text 用，扫描用 small |
| ONNX RT + PP-OCRv5 mobile | + 21 MB | 20–40 ms | 低于 v6 small；v5 的日文指标在各表中差异很大 | 同上 | 中 | 只作回退 |
| 维持 PaddleOCR-json，仅调参与换模型 | 不变 | −30%～−50% | 略升 | 不变 | 低 | 过渡方案（Phase 1） |
| `Windows.Media.Ocr`（系统 OCR） | **0 MB** | 50–140 ms | 日文一般、英文干净文本尚可；只给词级框、无置信度 | 系统组件，无需分发 | 低 | 无 runtime 时的兜底 |
| OneOCR（截图工具模型） | ~50 MB | 270–330 ms | 日文最好 | **模型不许再分发**，Win11 only | 中 | **不可发布** |
| Tesseract 5 + jpn | 2.5 / 14 MB | 0.1–1.5 s | CJK 硬字幕差（二值化/行切分不适用） | Apache-2.0 | 低 | 不推荐 |
| manga-ocr / comic-text-detector | 数百 MB | 0.3–0.7 s/块 | 气泡竖排日文好，横向白字硬字幕不是它的场景 | 各有许可 | 高 | 不适用 |
| VLM：PaddleOCR-VL 1.6 / GLM-OCR 0.9B | 0.9–1.8 GB | **1–3 s/裁切** | 整页强，单行图容易幻觉 | Apache-2.0 / MIT | 高 | 只作事件级二次校验 |
| RapidOCR（C++ 库）/ ncnn 移植 | — | 与 ORT 相当或更慢 | 继承 PP-OCR 模型 | Apache-2.0 | 中 | RapidOcrOnnx 停留在 v1.2.3/v3 模型；ncnn 移植社区实测更慢；只借鉴后处理代码 |

体积账（首选方案）：

```
onnxruntime.dll                ≈ 12–15 MB
PP-OCRv6_small_det.onnx        ≈  9.6 MB
PP-OCRv6_small_rec.onnx        ≈ 20.4 MB
ppocrv6 dict                   ≈  0.1 MB
------------------------------------------
合计                           ≈ 45 MB（现状 471 MB，约 1/10）
```

## 4. 为什么「换引擎」解决不了短句幻觉、误检和重复

这三类问题在 `README` 的已知问题里，但**根因大部分在 pipeline，不在模型家族**：

| 现象 | 机制（代码/实测依据） | 换 v6 能改善多少 | 真正要改什么 |
| --- | --- | --- | --- |
| 短句幻觉（2–3 个词也读错） | ① CTC 在很少的时间步上平均分数虚高，2 字符的错误串也能拿到 >0.95；② 行裁切是「行框 + 背景」，识别器看到大片背景纹理就会编词——本机实测在纯背景裁切上吐出 `c(0.37)`、`cYaaGeowb`；③ 宽行被压到固定 320 宽会丢空格（实测 `Ah,thelittle one canwaitforusinthelivingroom.`） | 明显（v6 英文 85.1→93.3） | 紧贴文字的裁切 + 多帧投票 + 长度感知阈值 |
| 大量非字幕文本 | det 在 1600 宽的下采样图上跑，场景纹理/UI 会被当文字；只靠 `confidence >= 0.6` 一道闸（`hardsub_timeline_scan.cpp:578`） | 一点 | 字幕带先验（全片行位置直方图）+ 水印/固定 UI 框过滤 + 时长下限 |
| 输出文本重复 | track 按 `RowSignature` 距离聚合，字幕中途签名漂移会裂成两条 track；合并只在 `start/end` **完全相等**时发生（`hardsub_timeline_scan.cpp:631-638`），差一帧就不合并 | 几乎不能 | 事件级去重：时间重叠 >80% 且文本相似 >0.8 就合并；<2 字符丢弃 |
| 时间轴在低对比度下漂 | 已由像素扫描 + 滞回解决，OCR 只做确认 | 不能 | 保持现状 |

额外一点，针对本项目特有的工作流：OCR 出来的英文要拿去和本地化文件（英→中双语条目）
做模糊匹配，这本质是「带噪声信道的匹配」问题。既然本地化语料已经在手里，**用它对 OCR
做约束解码/纠错**（编辑距离最近且超过阈值的条目直接吸附）比换任何引擎都更有效，
而且能顺带把「短句读错一个字母导致匹配失败」的问题消掉。

## 5. 建议的落地路线

### Phase 0：先有标尺（半天）

从真实素材里取 200–500 条事件，人工标注「文本 + 起止帧」，定义三个指标：

- 行级 CER（字符错误率）
- 事件级误检率（非字幕被判为字幕的比例）
- 重复率（同一句出现多次的事件占比）

用现有 runtime 跑一遍基线，之后每次改动都对着这组数据比。

### Phase 1：不动架构，先拿确定性收益（1–2 天）

1. `limit_side_len` 按素材定标（默认 960 → 小字优先 1280 / 速度优先 480，实测 480 让
   det 从 ~136 ms 降到 ~70 ms）。
2. 硬字幕不旋转：全流程也跳过 `cls`（现在只有纯检测模式跳）。
3. 行裁切改成「det box + 10% 边距」，不要拿整条区域或整条行框去 rec；
   需要的话对这类单行图走 `-det=false`（实测 43–47 ms，比全流程便宜 3–7 倍）。
4. 事件文本多帧投票：在事件区间内取 3–5 帧分别识别，按字符级投票取众数
   （`src/dialog_hardsub_scan.cpp` 里已有 `ConsensusText`，可直接复用）。
5. 事件级去重与合并（见 §4 表格）。
6. 置信度阈值改成「长度感知」：2 字符的行即使 0.95 也应当存疑。
7. 用本地化语料做 OCR 纠错/吸附。

预期：重复和误检大幅下降，短句错误下降，速度小幅提升（0～30%）。风险最低。

### Phase 2：换后端（1–2 周）

1. 抽出后端接口（见 §6），把 `RegionImage` 直接交给推理，去掉 PNG 临时文件与 JSON 管道。
2. 接入 ONNX Runtime CPU + PP-OCRv6 small（det/rec）+ ppocrv6 字典；
   det 的前后处理（resize/pad/DB 后处理）需要自己实现，或只保留 OpenCV 做轮廓/`unclip`。
3. 识别阶段支持批处理（同一帧多行 / 同一事件多帧拼 batch）。
4. 保留 PaddleOCR-json 作为可选回退（一个版本后再删）。
5. 打包脚本换成 hash 固定的模型下载 + 冒烟测试，`docs/ocr.md` 的「唯一运行时」表述同步更新。

预期：整包 ≈ 45 MB；扫描单样本 150–350 ms → 20–50 ms（约 5–10×）；
常驻内存从 1–2 GB 降到数百 MB 量级；v6 small 的英文识别错误率明显低于现状。

### Phase 3：可选增强

- **事件级二次校验**：只对低置信度/时序不一致的 5–10% 事件跑一次小 VLM
  （GLM-OCR 0.9B Q8 ≈ 1.4 GB，或 PaddleOCR-VL）。300 个事件约 8–15 分钟，可接受为
  「高精度模式」，但不要放进默认路径。
- **无 runtime 兜底**：用 `Windows.Media.Ocr`（英文/日文语言包），让没装 OCR 组件的
  用户也能用基本功能。
- **GPU**：DirectML/OpenVINO EP 作为可选加速，注意动态 shape 的缓存抖动，先测再开。

## 6. 接口与打包改动清单

```text
src/ocr/ocr_backend.h        新接口：Detect(RegionImage) / Recognize(line crops) / Run(image)
src/ocr/onnx_ocr.{h,cpp}     ORT 实现（det、DB 后处理、CTC 解码、批处理）
src/ocr/ocr_engine.cpp       保留：PaddleOCR-json 后端，实现同一接口
src/ocr/ocr_process.cpp      保留：常驻子进程（回退路径）
src/dialog_hardsub_scan_video.cpp:738-787  改为直接传 RegionImage，删临时 PNG
src/dialog_hardsub_scan.cpp                同上
src/dialog_ocr.cpp                         单帧识别走 medium 档
meson_options.txt / meson.build            新增 onnxruntime 依赖与后端开关
tools/ocr/download_onnx_ocr.ps1            模型下载 + SHA256 + 冒烟测试
.github/workflows/ci.yml                   缓存键与依赖更新
```

## 7. 风险与待验证

1. **paddle2onnx 转换差异**：官方不提供 v6 的现成 ONNX 下载，需要用
   `paddlex --install paddle2onnx` + `paddlex --paddle2onnx` 自行转换；历史上出现过
   识别输出错位（如 `2020-10-10` → `020-213:10`）。必须用黄金集回归，并固定 opset 与哈希。
2. **v6 指标不可外推**：官方数字是内部多场景集，不覆盖「硬字幕白字黑边」这一分布，
   换模型前必须自测。
3. **PaddleOCR-json 加载不了 v6**：它只认 legacy `inference.pdmodel`，而 v6 是 Paddle 3
   格式；沿用旧架构只能继续用 v5。想用 v6 就必须换运行时（这正是 Phase 2 的理由）。
4. **首次加载与内存**：ORT 冷启动 + 模型加载约 0.3–1 s，需要像现在一样异步预热。
5. **系统要求**：ORT Windows 构建要求 VC++ 运行库 ≥ 14.40，Win10 1809+ 可用；
   需要确认 CI 的运行时打包。
6. **许可**：模型 Apache-2.0、ORT MIT，可随包分发；OneOCR 的模型明确「不许再分发」，
   只能调用系统已安装的那份，且 Win11 限定——不要碰。
7. **语言范围**：如果未来要覆盖日文/中文硬字幕，v6 small 是同一套模型统一支持；
   现在的 `japanese` profile 只是命名，实际字典是 v5 多语字典，换 v6 时一并理清。

## 8. 参考资料

- PP-OCRv6 介绍与基准（检测/识别分语言表、端到端速度）：
  <https://www.paddleocr.ai/latest/en/version3.x/algorithm/PP-OCRv6/PP-OCRv6.html>
- 文本检测模块（模型体积、各引擎时延）：
  <https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/docs/version3.x/module_usage/text_detection.en.md>
- 文本识别模块（模型体积、ORT 端到端时延）：
  <https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/docs/version3.x/module_usage/text_recognition.en.md>
- 获取 ONNX 模型（paddle2onnx 流程）：
  <https://www.paddleocr.ai/latest/en/version3.x/inference_deployment/others/obtaining_onnx_models.html>
- `Windows.Media.Ocr`：
  <https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine>
- OneOCR 再分发声明：<https://oneocr.github.io/>
- video-subtitle-extractor（字幕带/水印/事件合并的参考实现）：
  <https://github.com/YaoFANGUK/video-subtitle-extractor>
- manga-image-translator（行合并与裁切策略）：
  <https://github.com/zyddnys/manga-image-translator>
- PP-OCRv5 日文识别指标：
  <https://www.paddleocr.ai/latest/version3.x/algorithm/PP-OCRv5/PP-OCRv5.html>
