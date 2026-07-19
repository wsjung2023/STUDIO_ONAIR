# R2-05 cut-suggestion core: deterministic SILENCE + FILLER cut suggestions.
# Qt-free (CLAUDE.md 3/5): it holds value objects, validated parameters, two
# pure detectors, an analyzer, and JSON serialization, and links no Qt, FFmpeg,
# or MLT. It builds ON cs_audio_dsp (level analysis) and cs_transcription (word
# analysis); no real speech/DSP engine is added, so no new third-party
# dependency and legal/OSS_BOM.csv is untouched. The actual non-destructive CUT
# that applies these suggestions is DEFERRED to the editor + edit-engine.
cs_add_qtfree_library(cs_cut_suggest
    SOURCES
        CutReason.h
        CutReason.cpp
        CutSuggestion.h
        CutSuggestion.cpp
        TextNormalize.h
        CutSuggestParameters.h
        SilenceDetector.h
        SilenceDetector.cpp
        FillerDetector.h
        FillerDetector.cpp
        CutSuggestionAnalyzer.h
        CutSuggestionAnalyzer.cpp
        CutSuggestionSerializer.h
        CutSuggestionSerializer.cpp
    DEPENDS
        cs_core
        cs_domain
        cs_audio_dsp
        cs_transcription
        nlohmann_json::nlohmann_json
)
