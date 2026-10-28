create_fooyin_plugin(
    tidal
    DEPENDS Fooyin::Core
            Qt6::Network
    SOURCES tidalplugin.cpp
            tidalplugin.h
)

target_compile_definitions(tidal PRIVATE FOOYIN_TIDAL_VERSION="${PROJECT_VERSION}")
