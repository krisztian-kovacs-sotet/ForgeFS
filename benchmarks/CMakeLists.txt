add_executable(forgefs_benchmark
    main.cpp
    ${CMAKE_SOURCE_DIR}/client/Client.cpp
)

target_include_directories(forgefs_benchmark PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_benchmark PRIVATE
    forgefs_protocol
    forgefs_network
    forgefs_crypto
    forgefs_common
)
