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
add_executable(fqueue_test tests/fqueue_test.cxx tests/fqueue_generic_test.cxx
    tests/fqueue_generic.c)
target_link_libraries(fqueue_test PRIVATE fqueue Catch2::Catch2WithMain)
add_test(NAME fqueue_test COMMAND fqueue_test --skip-benchmarks)

# Пути ошибок: те же исходники с тестовым аллокатором (может отказать в
# выбранной аллокации) и малыми пределами, чтобы до них можно было дойти.
set(FQUEUE_TEST_LEN_LIMIT 40)
set(FQUEUE_TEST_REFS_LIMIT 100)
add_library(fqueue_limits STATIC src/fqueue.c)
target_include_directories(fqueue_limits PUBLIC include)
target_compile_definitions(fqueue_limits PRIVATE
    FQUEUE_MALLOC=fqueue_test_malloc FQUEUE_FREE=fqueue_test_free
    FQUEUE_IMPL_LEN_LIMIT=${FQUEUE_TEST_LEN_LIMIT}
    FQUEUE_IMPL_REFS_LIMIT=${FQUEUE_TEST_REFS_LIMIT})
add_executable(fqueue_limits_test tests/fqueue_limits_test.cxx)
target_compile_definitions(fqueue_limits_test PRIVATE
    TEST_LEN_LIMIT=${FQUEUE_TEST_LEN_LIMIT}
    TEST_REFS_LIMIT=${FQUEUE_TEST_REFS_LIMIT})
target_link_libraries(fqueue_limits_test PRIVATE fqueue_limits Catch2::Catch2WithMain)
add_test(NAME fqueue_limits_test COMMAND fqueue_limits_test)

# Нагрузка для сравнения с Rust (just bench-*): максимальная оптимизация
# независимо от CMAKE_BUILD_TYPE.
add_executable(fqueue_bench bench/fqueue_bench.c src/fqueue.c)
target_include_directories(fqueue_bench PRIVATE include)
target_compile_options(fqueue_bench PRIVATE -O3 -march=native -flto -DNDEBUG)
target_link_options(fqueue_bench PRIVATE -flto)
