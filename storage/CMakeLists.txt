add_executable(forgefs_storage
    main.cpp
    StorageNode.cpp
)

target_include_directories(forgefs_storage PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_storage PRIVATE
    forgefs_protocol
    forgefs_network
    forgefs_common
)
