# Create the library
add_library(arithcoder
  ArithmeticCoder.cpp
  ArithmeticCoder.hpp

  BitIoStream.cpp
  BitIoStream.hpp

  FrequencyTable.cpp
  FrequencyTable.hpp
)

# Set up include directories for this library
target_include_directories(arithcoder
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
