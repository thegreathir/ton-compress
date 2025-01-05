cmake_minimum_required(VERSION 3.10)
project(model_arith)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Torch_DIR "${PROJECT_SOURCE_DIR}/libtorch/share/cmake/Torch")

find_package(Torch REQUIRED)

add_executable(model_arith model_arith.cpp)

add_subdirectory(arithcoder)

target_link_libraries(model_arith "${TORCH_LIBRARIES}" arithcoder ton_crypto_lib)

set_property(TARGET model_arith PROPERTY CXX_STANDARD 17)
