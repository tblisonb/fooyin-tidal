add_executable(plugin-check plugin-check.cpp)
target_link_libraries(plugin-check PRIVATE Fooyin::Core Qt6::Core)

add_executable(stream-check stream-check.cpp)
target_link_libraries(stream-check PRIVATE tidal_common)

add_executable(catalog-check catalog-check.cpp)
target_link_libraries(catalog-check PRIVATE tidal_common)

add_executable(cover-check cover-check.cpp)
target_link_libraries(cover-check PRIVATE tidal_common Qt6::Gui)
