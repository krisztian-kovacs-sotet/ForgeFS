add_executable(forgefs_server
    main.cpp
    Server.cpp
    NodeRegistry.cpp
    NodeClient.cpp
    ChunkDistributor.cpp
    UploadSession.cpp
    ReplicationMonitor.cpp
)

target_include_directories(forgefs_server PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_server PRIVATE
    forgefs_protocol
    forgefs_network
    forgefs_common
    forgefs_crypto
    forgefs_database
)
