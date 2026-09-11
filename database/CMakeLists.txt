find_package(SQLite3 REQUIRED)

add_library(forgefs_database STATIC
    Database.cpp
    ChunkStore.cpp
    UserStore.cpp
)

target_include_directories(forgefs_database PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_database PUBLIC SQLite::SQLite3)
