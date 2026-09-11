include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# --- Unit tests: individual modules in isolation, no network/subprocesses ---
# server/SessionManager.cpp is compiled straight in rather than via a
# forgefs_server_lib target, since Server.cpp and friends currently only
# exist as part of the forgefs_server executable; SessionManager has no
# dependency on the rest of the server, so this is a clean way to test it
# without restructuring server/CMakeLists.txt.
add_executable(forgefs_unit_tests
    unit/ProtocolTest.cpp
    unit/CryptoTest.cpp
    unit/ChunkStoreTest.cpp
    unit/SessionManagerTest.cpp
    unit/PathSafetyTest.cpp
    ${CMAKE_SOURCE_DIR}/server/SessionManager.cpp
)
target_include_directories(forgefs_unit_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_unit_tests PRIVATE
    forgefs_protocol
    forgefs_network
    forgefs_crypto
    forgefs_database
    forgefs_common
    GTest::gtest_main
)

# --- Integration tests: spawn real forgefs_server/forgefs_storage processes ---
# and drive them over the actual protocol via client::Client. Client.cpp is
# compiled in directly for the same reason SessionManager.cpp is above.
add_executable(forgefs_integration_tests
    integration/TestHarness.cpp
    integration/ClusterTest.cpp
    ${CMAKE_SOURCE_DIR}/client/Client.cpp
)
target_include_directories(forgefs_integration_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_integration_tests PRIVATE
    forgefs_protocol
    forgefs_network
    forgefs_crypto
    forgefs_common
    GTest::gtest_main
)
target_compile_definitions(forgefs_integration_tests PRIVATE
    FORGEFS_SERVER_BIN="$<TARGET_FILE:forgefs_server>"
    FORGEFS_STORAGE_BIN="$<TARGET_FILE:forgefs_storage>"
)
add_dependencies(forgefs_integration_tests forgefs_server forgefs_storage)

include(GoogleTest)
gtest_discover_tests(forgefs_unit_tests)
gtest_discover_tests(forgefs_integration_tests PROPERTIES TIMEOUT 60)
