find_package(OpenSSL REQUIRED)

add_library(forgefs_crypto STATIC
    Sha256.cpp
    Hex.cpp
    PasswordHash.cpp
    Random.cpp
)

target_include_directories(forgefs_crypto PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(forgefs_crypto PUBLIC OpenSSL::Crypto)
