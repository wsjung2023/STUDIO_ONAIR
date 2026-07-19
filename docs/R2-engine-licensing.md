# R2 Engine Licensing — whisper.cpp + ggml Whisper weights

Concise licensing determination for the local speech-to-text engine integrated
behind `transcription::ITranscriptionProvider` (see `legal/OSS_BOM.csv`).

## whisper.cpp — MIT (clear)

- Upstream: <https://github.com/ggml-org/whisper.cpp>, `LICENSE` is MIT.
- Bundled `ggml` (same org) is MIT.
- We pin **v1.7.6**, source commit `a8d002cfd879315632a579e73f0148d06959de36`,
  source archive SHA-256
  `166140e9a6d8a36f787a2bd77f8f44dd64874f12dd8359ff7c1f4f9acb86202e`
  (downloaded and verified by `scripts/bootstrap_whisper.ps1`).
- Built **CPU-only**: `GGML_CUDA/HIP/VULKAN/SYCL/METAL=OFF`,
  `WHISPER_COREML/OPENVINO=OFF`, `WHISPER_CURL=OFF` (no runtime model
  auto-download), tests/examples/server OFF. No GPL, no non-free option — there
  is nothing analogous to FFmpeg's `--enable-gpl` to avoid, but the bootstrap and
  the `WhisperBootstrapPolicy` test still refuse any such flag.
- Linked as a dynamic library **inside `cs_whisper_adapter` only**, which is an
  adapter (like `cs_mlt_adapter`). The `cs_transcription` core stays Qt-free
  **and** whisper-free; the adapter is built only when `CS_ENABLE_WHISPER=ON`.

## ggml Whisper model weights — MIT (clear)

- OpenAI released Whisper (code and model weights) under the **MIT** license
  (<https://github.com/openai/whisper/blob/main/LICENSE>). The ggml-format
  conversions distributed at <https://huggingface.co/ggerganov/whisper.cpp>
  carry the same MIT terms.
- We pin **`ggml-tiny.en.bin`** (smallest English model), SHA-256
  `921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f`
  (this is the real HuggingFace git-LFS object hash, confirmed by downloading the
  file). ARCHITECTURE §11 requires model-download hash verification: the
  bootstrap verifies it on download, and `WhisperRuntimeManifest` re-verifies the
  on-disk model hash at load before any inference.

## Net obligation

MIT for both — preserve the copyright/permission notice. No copyleft, no source
disclosure, no relinking obligation. Approved for the closed core.
