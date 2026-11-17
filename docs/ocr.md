# Image to Text OCR

Aegisub Windows release artifacts include offline OCR support through an
in-process ONNX Runtime engine running PP-OCRv6 small. Users do not need Python,
PaddlePaddle, PaddleOCR, a separate OCR process, model downloads, or extra DLL
setup.

## How to use

1. Open Aegisub.
2. Open a video, or prepare an image file.
3. Choose `Video > Image to Text (OCR)...`.
4. Select `Current video frame` or `Image file`.
5. Click `Recognize`.
6. Use `Insert`, `Copy`, or `Replace Line...`.

`Replace Line...` always asks for confirmation before replacing the current
subtitle line text.

## Runtime layout

Windows portable zips and installers contain:

```text
ocr/
  onnx/
    onnxruntime.dll
    onnxruntime_providers_shared.dll
    ppocrv6_small_det.onnx
    ppocrv6_small_rec.onnx
    ppocrv6_dict.txt
    LICENSE.onnxruntime.txt
    ThirdPartyNotices.onnxruntime.txt
    VERSION.txt
  licenses/
    THIRD_PARTY_ONNX_OCR.txt
```

The OCR engine uses this folder next to `aegisub.exe`. If any required file is
deleted, Aegisub reports the exact missing path, and the OCR actions stay
disabled instead of failing mid-scan.

## OCR engines

`Tool/OCR/Backend` in the config file selects the engine:

- `auto` (default) uses the in-process ONNX Runtime engine and falls back to
  PaddleOCR-json when that engine cannot start.
- `onnx` uses only the in-process engine: `ocr/onnx/onnxruntime.dll`,
  `ppocrv6_small_det.onnx`, `ppocrv6_small_rec.onnx` and `ppocrv6_dict.txt`.
  See [ocr-onnx-models.md](ocr-onnx-models.md) for the model contract.
- `paddle` uses only the bundled PaddleOCR-json sidecar described above.

The ONNX engine runs in-process, so it avoids writing a PNG and starting a
process for every frame, and it recognizes a 4K subtitle strip in tens of
milliseconds instead of the ~0.4 s the sidecar needs. Both engines speak the
same result contract, so the dialogs, the hard subtitle scan and Image2Text do
not care which one is active.

Image2Text and the OCR dialog keep their engine loaded for the rest of the
session (a few hundred milliseconds to load, tens of milliseconds per
recognition); the hard subtitle scan dialogs warm one engine per dialog and tear
it down when the dialog closes.

## OCR self-test

The Windows smoke test and support diagnostics use a console mode:

```text
aegisub.exe --ocr-selftest frame.png --out report.json --backend onnx
```

It runs one image through the configured engine without opening a window and
writes a JSON report (text, per-line confidence and boxes, engine start and
recognition time, diagnostics). The exit code is 0 when text was recognized, 1
when the image held no text, and 2 when the engine could not run. Point
`AEGISUB_OCR_ASSETS` at an unpacked `ocr/onnx` folder to test assets that are
not installed next to the executable.

## Hard subtitle scan

`Video > Hard Subtitle Scan` selects a video region, recognizes its text, and
detects the frame-exact start/end of the burned-in subtitle:

1. A fast pixel scan decodes one contiguous ascending pass over the run.
   Random frame seeks on modern codecs (4K/AV1) cost about a second each
   because a whole GOP must be decoded, while sequential decoding is a few
   milliseconds per frame, so the scan aligns the pass start to a keyframe
   (cheap seek) and then decodes forward through the whole subtitle. Both
   boundaries are read off the resulting per-frame coverage series. The scan
   only uses the template's text pixels, so background motion does not affect
   it, and coverage hysteresis (appear vs. disappear thresholds) plus a
   debounce filter keep fades and single-frame noise from shifting the
   boundaries. The text mask is built from luminance extremes (bright white or
   very dark ink) rather than background deviation, so moving scene texture
   behind the subtitle does not make the coverage fluctuate.
2. When enabled (default), boundaries are confirmed with the OCR engine in
   detection-only mode, which only needs text boxes and is several times faster
   than full recognition (about 27 ms versus 78 ms for a 4K strip). Confirmation
   only runs when a boundary is ambiguous (a fade or occlusion leaves text-like
   coverage just outside the run), and the boundary moves at most a couple of
   frames, only toward frames where both the pixel signal and OCR agree.
3. A lightweight content check warns when the detected range may contain
   multiple subtitles (duration far longer than the text suggests, or an
   interior pixel dip). The opt-in "Strict timeline (OCR text check)" option
   performs a slower full-recognition sweep that compares each sampled frame's
   text against the reference.

### How the scan turns OCR results into rows

- Recognition runs on a tight, full-resolution crop of the detected line. The
  padded template is only used for the coverage scan, because the recognizer
  normalizes its input to 48 px height and the surrounding background is what
  short readings hallucinate on.
- A row whose first reading is short (4 characters or fewer) or low-confidence
  is read again on the following frames and the consensus reading wins. Those
  frames come from the scan decoder's sequential pass, so voting costs a few
  decode milliseconds instead of another seek.
- A reading must clear a length-aware confidence floor: 1 character needs 0.90,
  2-3 characters 0.75, 4-7 characters 0.62, 8 or more 0.55. CTC scores over
  very few time steps are inflated, so a short misread otherwise scores above
  0.9.
- Rows that describe the same subtitle (overlapping interval, same visual line,
  similar text) are folded together, and rows with no readable text are
  dropped.

`Tool/OCR/Detect Limit Side Len` in the config file caps the detector input
(0 keeps the engine default of 960). Raising it to 1280 or 1600 helps small or
faint text in a wide 4K region at a proportional detection cost; lowering it
speeds the scan up when subtitles are large. The 0/180 degree angle classifier
is skipped everywhere now, since burned-in subtitles are horizontal.

The OCR engine for boundary checks is loaded in the background when the dialog
opens, so model loading overlaps choosing the region instead of adding to the
scan time, and it is released when the dialog closes. Video playback is paused
for the duration of the scan so the decoder worker is not contended. Scans
typically finish in 1–4 seconds depending on video resolution, codec and run
length.

The portable package and the installer copy the staged OCR assets next to
`aegisub.exe`; the ONNX engine needs no separate process, no generated config
file and no language-specific model profile.

## Bundled engine

Windows artifacts ship:

- ONNX Runtime `1.32.0` for Windows x64 (CPU), from the official release zip.
- `ppocrv6_small_det.onnx` (PP-OCRv6 small text detector, opset 20).
- `ppocrv6_small_rec.onnx` (PP-OCRv6 small recognizer, opset 11).
- `ppocrv6_dict.txt` (18 383 entry recognition dictionary).

The measured latency, the model's input/output contract and the file hashes are
documented in [ocr-onnx-models.md](ocr-onnx-models.md). The engine is loaded from
`ocr/onnx` with `LoadLibrary`, so a build without the folder simply keeps the
OCR actions disabled.

### Legacy PaddleOCR-json bundle

The bundled PaddleOCR-json sidecar (PP-OCRv5 server det/rec) is no longer
packaged. The backend is still compiled in for one release (`WITH_PADDLEOCR`):
dropping the old `ocr/bin` and `ocr/models` folders next to `aegisub.exe` and
setting `Tool/OCR/Backend` to `paddle` restores that path for comparison or for
an installation that already has the files.
`tools/ocr/download_paddleocr_windows.ps1` still exists for that, but CI and the
installer no longer run it.

## Build and packaging

The pinned runtime and models are prepared by:

```powershell
tools\ocr\download_onnx_ocr.ps1 -DestinationDir build\installer-deps\ocr
```

The script downloads the release assets, verifies every file's size and
SHA-256, unpacks `onnxruntime.dll` and writes the license files; it skips files
that are already correct, so it is safe to re-run. `tools\win-installer-setup.ps1`
calls it, and the portable packaging script copies `build\installer-deps\ocr`
into the artifact as `ocr`.

CI caches `build/installer-deps/ocr` with the key
`onnxocr-ppocrv6-small-onnxruntime-1.32.0-v1`; change the key whenever the
pinned runtime or models change.

## Verifying the OCR engine in an artifact

CI runs the packaged engine, not just the build tree:

```powershell
build\aegisub-portable\aegisub.exe --ocr-selftest tests\ocr\selftest.png `
    --out build\ocr-selftest.json --backend onnx
```

The step fails unless the process exits 0, the report reads `Aegisub OCR 123`
from the fixed fixture, and the portable zip contains
`ocr/onnx/onnxruntime.dll`, both models and the dictionary. It also fails if the
retired `PaddleOCR-json.exe` reappears in the zip.

Check these files in the portable zip or installer output:

```text
ocr/onnx/onnxruntime.dll
ocr/onnx/ppocrv6_small_det.onnx
ocr/onnx/ppocrv6_small_rec.onnx
ocr/onnx/ppocrv6_dict.txt
ocr/licenses/THIRD_PARTY_ONNX_OCR.txt
```

If the engine fails to start, the OCR dialogs report the missing file or the
runtime's error message instead of an empty result, and `--ocr-selftest` returns
exit code 2 with the same text in its report.

## Updating OCR runtime versions

1. Publish a new asset release and update the file names, sizes and SHA-256
   values in `tools/ocr/download_onnx_ocr.ps1`.
2. Update `docs/ocr-onnx-models.md` (contract, hashes, measured latency).
3. Update the cache key in `.github/workflows/ci.yml`.
4. Let GitHub Actions build and smoke-test the Windows installer and portable
   artifacts.
