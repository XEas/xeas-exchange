cmake_minimum_required(VERSION 3.24)
project(xeas_exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- core library -----------------------------------------------------------
add_library(xeas_core src/book.cpp)
target_include_directories(xeas_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(xeas_core PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(xeas_core PRIVATE -Wall -Wextra)
endif()

# --- tests -------------------------------------------------------------------
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
# For Windows: prevent overriding the parent project's compiler/linker settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(book_test tests/book_test.cpp)
target_link_libraries(book_test PRIVATE xeas_core GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(book_test PRIVATE -Wall -Wextra)
endif()

include(GoogleTest)
gtest_discover_tests(book_test)
