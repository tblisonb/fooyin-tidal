add_executable(plugin-check plugin-check.cpp)
target_link_libraries(plugin-check PRIVATE Fooyin::Core Qt6::Core)
