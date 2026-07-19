# Audited RNNoise denoise adapter (R2 audio DSP). Built ONLY when
# CS_ENABLE_RNNOISE is ON. This target links the statically-built, audited
# RNNoise library (BSD-3-Clause) staged by scripts/bootstrap_rnnoise.ps1 into
# CS_RNNOISE_ROOT; cs_audio_dsp itself stays Qt-free AND rnnoise-free — only
# this separate, enabled-only target ever touches RNNoise.
#
# It is still a cs_add_qtfree_library: like cs_mlt_adapter it links a native
# third-party library but must not pull in Qt (ARCHITECTURE.md 14). The
# rnnoise::rnnoise imported target below carries only C sources and a plain
# header, so the Qt-free guard passes.

if(NOT TARGET rnnoise::rnnoise)
    if(WIN32)
        set(_cs_rnnoise_lib "${CS_RNNOISE_ROOT}/lib/rnnoise.lib")
    else()
        set(_cs_rnnoise_lib "${CS_RNNOISE_ROOT}/lib/librnnoise.a")
    endif()
    if(NOT EXISTS "${_cs_rnnoise_lib}")
        message(FATAL_ERROR
            "CS_RNNOISE_ROOT does not contain the audited RNNoise static "
            "library (${_cs_rnnoise_lib}). Run scripts/bootstrap_rnnoise.ps1.")
    endif()
    add_library(rnnoise::rnnoise STATIC IMPORTED)
    set_target_properties(rnnoise::rnnoise PROPERTIES
        IMPORTED_LOCATION "${_cs_rnnoise_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${CS_RNNOISE_ROOT}/include"
    )
endif()

cs_add_qtfree_library(cs_rnnoise_adapter
    SOURCES
        RnnoiseRuntimeManifest.h
        RnnoiseRuntimeManifest.cpp
        RnnoiseDenoiseProcessor.h
        RnnoiseDenoiseProcessor.cpp
    DEPENDS
        cs_audio_dsp
        rnnoise::rnnoise
        nlohmann_json::nlohmann_json
)

target_compile_definitions(cs_rnnoise_adapter PRIVATE
    CS_RNNOISE_EXPECTED_VERSION="0.1.1"
    CS_RNNOISE_EXPECTED_COMMIT="6cbfd53eb348a8d394e0757b4025c6ded34eb2b6"
    CS_RNNOISE_EXPECTED_ARCHIVE_SHA256="1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987"
)
