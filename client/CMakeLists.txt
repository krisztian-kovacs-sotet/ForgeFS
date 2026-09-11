add_executable(forgefs_client
    main.cpp
    Client.cpp
    TokenStore.cpp
)

target_include_directories(forgefs_client PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_client PRIVATE
    forgefs_protocol
    forgefs_network
    forgefs_common
    forgefs_crypto
)
