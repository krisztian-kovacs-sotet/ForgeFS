add_library(forgefs_protocol STATIC
    Protocol.cpp
)

target_include_directories(forgefs_protocol PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_protocol PUBLIC forgefs_network)
