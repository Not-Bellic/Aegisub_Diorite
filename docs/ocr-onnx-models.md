# OCR model assets (PP-OCRv6 small + ONNX Runtime)

The in-process OCR backend (Phase 2) loads its models from the bundled `ocr/onnx`
folder. The files are published as assets of the GitHub release
[`ocr-models-v1`](https://github.com/Not-Bellic/Aegisub_Diorite/releases/tag/ocr-models-v1)
so the packaging script can fetch them by name and verify a pinned hash.

| Asset | Size | SHA-256 |
| --- | ---: | --- |
| `ppocrv6_small_det.onnx` | 9 929 786 B | `dc69df47664b71b7ed7f50cfec8b602d3826a6e90627c8b709d8c95e0e3a41db` |
| `ppocrv6_small_rec.onnx` | 21 104 070 B | `fd36dd7f279644b7b623c66f59c748ab6936fa2f228d8848a945b29f0954bef7` |
| `ppocrv6_dict.txt` | 74 012 B | `d1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b` |
| `onnxruntime-win-x64-1.32.0.zip` | 7 941 670 B | `b874e79159101226bb593562cfdf1a1e4a48a11efd112a3f999f619b833446ed` |
| `SHA256SUMS.txt` | 362 B | `8bbf01077b7adcce1edb7cf98b2931079e83549b496ffb2ca9e0e4863fec4d2f` |

Sources and licenses:

- Models: PaddleOCR's official `PP-OCRv6_small_{det,rec}_onnx` archives
  (`https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0/`),
  Apache-2.0.
- Runtime: ONNX Runtime 1.32.0 Windows x64 CPU, MIT. The DLLs are the ones
  shipped in the `onnxruntime` 1.32.0 PyPI wheel (same Microsoft build as the
  release archive); the zip also carries `LICENSE`, `ThirdPartyNotices.txt` and
  `VERSION.txt`.

## Model contract

Both models were exported with PaddleOCR 3.x and expect **BGR** input
(`DecodeImage.img_mode: BGR`), so the C++ side must swap R and B.

### Detection (`PP-OCRv6_small_det`, opset 20)

| Item | Value |
| --- | --- |
| Input | `x`: `[N, 3, H, W]`, dynamic; H and W multiples of 32 |
| Resize | longest side 960 by default, keep aspect ratio (`DetResizeForTest.resize_long`) |
| Normalize | `x/255`, BGR mean `[0.485, 0.456, 0.406]`, std `[0.229, 0.224, 0.225]`, HWC→CHW, pad right/bottom with 0 |
| Output | probability map `[N, 1, H, W]` |
| Post-process | DB: `thresh 0.3`, `box_thresh 0.6`, `unclip_ratio 1.5`, `max_candidates 1000`, min box side 3 px |

The backend only needs axis-aligned boxes (burned-in subtitles are horizontal),
so it threshold the map, groups pixels into connected components, expands each
component by the DB unclip distance and reports its bounding rectangle. No
OpenCV and no rotated rectangles.

### Recognition (`PP-OCRv6_small_rec`, opset 11)

| Item | Value |
| --- | --- |
| Input | `x`: `[N, 3, 48, W]`, W multiple of 32, up to 3200 |
| Resize | height 48, width `ceil(48 * aspect)` rounded up to 32, right-padded with 0 |
| Normalize | `(x/255 - 0.5) / 0.5`, BGR |
| Output | `[N, T, 18385]` logits (CTC) |

Class mapping: index `0` is the CTC blank, `1..18383` are the lines of
`ppocrv6_dict.txt` in file order, and index `18384` is a space. The dictionary
contains no plain space character and its first entry is an ideographic space
(U+3000); the file is UTF-8, LF, one character per line, without a BOM.

## Measured latency

Development machine: Ryzen 7 7745HX (16 threads), ONNX Runtime 1.32.0 CPU,
`intra_op_num_threads = 8`, no PNG encode (buffers passed directly).

| Workload | Time |
| --- | ---: |
| Detector, 960×108 strip (from a 3840×432 region) | 12.4 ms |
| Detector, 1280×144 strip | 19.7 ms |
| Detector, 1600×180 strip | 32.0 ms |
| Recognizer, one 48×~610 line | 21.9 ms |
| Recognizer, batch of 4 identical lines | 99 ms |
| Recognizer, batch of 8 identical lines | 219 ms |

Batching gives no throughput win on CPU: ORT already parallelises a single
inference across threads, and larger batches only add memory traffic. Recognizer
cost scales with line width, so the backend should keep long lines single and
batch only short, similar-width lines if at all.

End-to-end on the sample frames in `build/frames` (detector + recognizer, no
PNG round trip): 42–71 ms per 3840×432 strip, versus roughly 150 ms for the
detector alone and 232 ms for the full pipeline in the PaddleOCR-json path
(plus a 175 ms PNG encode at native resolution).

## Validation against the current engine

The ONNX pair reproduces the PaddleOCR-json results on the same frames:

| Frame | ONNX result | Confidence |
| --- | --- | ---: |
| `bottom_018000.png` | `- She's human.` | 0.980 |
| `bottom_021600.png` | `Ah, the little one can wait for us in the living room.` | 0.991 |
| `bottom_012600.png` (no subtitle) | (no boxes) | — |

## Updating the assets

Publish a new release (`ocr-models-v2`) instead of replacing files under an
existing tag, then update the file names and hashes in
`tools/ocr/download_onnx_ocr.ps1`.
