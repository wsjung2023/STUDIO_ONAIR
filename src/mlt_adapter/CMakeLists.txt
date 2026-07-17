cs_add_qtfree_library(cs_mlt_graph_plan
    SOURCES
        MltGraphPlan.h
        MltGraphPlan.cpp
        FrameEffects.h
        FrameEffects.cpp
    DEPENDS
        cs_edit_engine
)

if(CS_ENABLE_MLT)
    cs_add_qtfree_library(cs_mlt_adapter
        SOURCES
            MltRuntimeManifest.h
            MltRuntimeManifest.cpp
            MltEditEngine.h
            MltEditEngine.cpp
        DEPENDS
            cs_mlt_graph_plan
            MLT::PlusPlus
            nlohmann_json::nlohmann_json
    )
    target_compile_definitions(cs_mlt_adapter PRIVATE
        CS_MLT_EXPECTED_VERSION="7.40.0"
        CS_MLT_EXPECTED_COMMIT="bef9d89c0c279e558d9625dac3399c2aa3d961bc"
    )
endif()
