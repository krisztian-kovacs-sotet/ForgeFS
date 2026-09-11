add_library(forgefs_common STATIC
    Logging.cpp
    PathSafety.cpp
    ThreadPool.cpp
    PasswordPrompt.cpp
)

target_include_directories(forgefs_common PUBLIC ${CMAKE_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(forgefs_common PUBLIC Threads::Threads)
