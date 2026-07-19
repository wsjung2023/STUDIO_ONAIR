# Qt-free DSP core (R2-06). Stays below the application boundary: no Qt,
# FFmpeg, MLT or capture-backend types — cs_assert_qt_free enforces it at build
# time. nlohmann_json (MIT, already in the OSS BOM) is linked now for the
# loudness telemetry that later stages of this module add; the standard-based
# BS.1770/R128 math pulls in no new dependency.
cs_add_qtfree_library(cs_audio_dsp
    SOURCES
        AudioFormat.h
        AudioFormat.cpp
        AudioBuffer.h
        DspMath.h
        IAudioProcessor.h
        GainProcessor.h
        GainProcessor.cpp
        CompressorProcessor.h
        CompressorProcessor.cpp
        LimiterProcessor.h
        LimiterProcessor.cpp
        AudioProcessingChain.h
        AudioProcessingChain.cpp
        LoudnessNormalizer.h
        LoudnessNormalizer.cpp
        KWeightingFilter.h
        KWeightingFilter.cpp
        LoudnessMeter.h
        LoudnessMeter.cpp
        AudioLoudnessSample.h
        AudioLoudnessSerializer.h
        AudioLoudnessSerializer.cpp
        AudioLoudnessNdjsonSink.h
        AudioLoudnessNdjsonSink.cpp
    DEPENDS
        cs_core
        nlohmann_json::nlohmann_json
)
