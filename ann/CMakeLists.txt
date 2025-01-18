add_executable(model_arith model_arith.cpp)

add_subdirectory(arithcoder)

target_link_libraries(model_arith "${TORCH_LIBRARIES}" arithcoder ton_crypto_lib)

set_property(TARGET model_arith PROPERTY CXX_STANDARD 17)

add_library(ann
  model_arith.h
  model_arith.cpp
)

target_link_libraries(ann PRIVATE "${TORCH_LIBRARIES}" arithcoder ton_crypto_lib)

target_include_directories(ann
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

