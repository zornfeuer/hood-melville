cmake_minimum_required(VERSION 3.20)
project(hood_melville_c C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Библиотека.
add_library(fqueue src/fqueue.c)
target_include_directories(fqueue PUBLIC include)

# Тесты (Catch2).
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.0
)
FetchContent_MakeAvailable(Catch2)
enable_testing()
add_executable(fqueue_test tests/fqueue_test.cxx)
target_link_libraries(fqueue_test PRIVATE fqueue Catch2::Catch2WithMain)
add_test(NAME fqueue_test COMMAND fqueue_test --skip-benchmarks)

# Нагрузка для сравнения с Rust (just bench-*): максимальная оптимизация
# независимо от CMAKE_BUILD_TYPE.
add_executable(fqueue_bench bench/fqueue_bench.c src/fqueue.c)
target_include_directories(fqueue_bench PRIVATE include)
target_compile_options(fqueue_bench PRIVATE -O3 -march=native -flto -DNDEBUG)
target_link_options(fqueue_bench PRIVATE -flto)
