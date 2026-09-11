find_package(OpenSSL REQUIRED)

add_library(forgefs_network STATIC
    Socket.cpp
    Tls.cpp
)

target_include_directories(forgefs_network PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_network PUBLIC OpenSSL::SSL)
