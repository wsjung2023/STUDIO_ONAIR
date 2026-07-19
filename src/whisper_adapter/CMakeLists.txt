# R2-05 audited whisper.cpp transcription adapter. Built ONLY when
# CS_ENABLE_WHISPER is ON (see the root CMakeLists gate + scripts/
# bootstrap_whisper.ps1). Like cs_mlt_adapter it is an adapter, not part of the
# Qt-free/whisper-free core: it links the whisper library and implements the
# transcription::ITranscriptionProvider port. It stays Qt-free (cs_add_qtfree_
# library enforces it) so the only thing it adds over cs_transcription is the
# native speech engine, never Qt.
cs_add_qtfree_library(cs_whisper_adapter
    SOURCES
        WhisperRuntimeManifest.h
        WhisperRuntimeManifest.cpp
        WhisperTranscriptionProvider.h
        WhisperTranscriptionProvider.cpp
    DEPENDS
        cs_core
        cs_domain
        cs_transcription
        whisper
        nlohmann_json::nlohmann_json
)

# Kept in lock-step with scripts/bootstrap_whisper.ps1 and legal/OSS_BOM.csv.
target_compile_definitions(cs_whisper_adapter PRIVATE
    CS_WHISPER_EXPECTED_VERSION="1.7.6"
    CS_WHISPER_EXPECTED_COMMIT="a8d002cfd879315632a579e73f0148d06959de36"
    CS_WHISPER_EXPECTED_MODEL="ggml-tiny.en.bin"
    CS_WHISPER_EXPECTED_MODEL_SHA256="921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f"
)
