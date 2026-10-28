# TIDAL access and streaming, shared by the plugin and the tools that test it.
add_library(
    tidal_common STATIC
    blockinghttp.cpp
    blockinghttp.h
    segmentstreamdevice.cpp
    segmentstreamdevice.h
    tidalapi.cpp
    tidalapi.h
    tidalsession.cpp
    tidalsession.h
)
set_target_properties(tidal_common PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
target_include_directories(tidal_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tidal_common PUBLIC Fooyin::Core Qt6::Network)

create_fooyin_plugin(
    tidal
    DEPENDS tidal_common
            Fooyin::Gui
            Qt6::Concurrent
            Qt6::Widgets
    SOURCES tidaldecoder.cpp
            tidaldecoder.h
            tidalplugin.cpp
            tidalplugin.h
)

target_compile_definitions(tidal PRIVATE FOOYIN_TIDAL_VERSION="${PROJECT_VERSION}")
