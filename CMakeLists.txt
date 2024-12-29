cmake_minimum_required(VERSION 3.10)

project(ton-compress-contest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    include
    include/crypto
    include/tdutils
    include/ton
    include/common
)

link_directories(.)

add_executable(solution solution.cpp)
target_link_libraries(solution PRIVATE ton_crypto_lib)

add_executable(solution_lzma_long solution_lzma_long.cpp)
target_link_libraries(solution_lzma_long PRIVATE ton_crypto_lib)

add_executable(solution_evolve solution_evolve.cpp)
target_link_libraries(solution_evolve PRIVATE ton_crypto_lib)

# 01 - Cells.cpp
add_executable(Cells "01 - Cells.cpp")
target_link_libraries(Cells PRIVATE ton_crypto_lib)

# 02 - Base64, serialization, lz4.cpp
add_executable(Base64_Serialization_LZ4 "02 - Base64, serialization, lz4.cpp")
target_link_libraries(Base64_Serialization_LZ4 PRIVATE ton_crypto_lib)

# 03 - TLB.cpp
add_executable(TLB "03 - TLB.cpp")
target_link_libraries(TLB PRIVATE ton_crypto_lib)

# 04 - TLB 2.cpp
add_executable(TLB_2 "04 - TLB 2.cpp")
target_link_libraries(TLB_2 PRIVATE ton_crypto_lib)

# 05 - Int256.cpp
add_executable(Int256 "05 - Int256.cpp")
target_link_libraries(Int256 PRIVATE ton_crypto_lib)

# 06 - Dictionary.cpp
add_executable(Dictionary "06 - Dictionary.cpp")
target_link_libraries(Dictionary PRIVATE ton_crypto_lib)

# 07 - Augmented dictionary.cpp
add_executable(AugmentedDictionary "07 - Augmented dictionary.cpp")
target_link_libraries(AugmentedDictionary PRIVATE ton_crypto_lib)
