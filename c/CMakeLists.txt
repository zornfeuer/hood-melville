cmake_minimum_required(VERSION 3.20)
project(hood_melville VERSION 0.1.0 LANGUAGES C)

# Тесты и нагрузка нужны только самому проекту, не тем, кто подключает его
# через add_subdirectory/FetchContent.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(HOOD_MELVILLE_TOP_LEVEL ON)
    set(CMAKE_C_STANDARD 17)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else()
    set(HOOD_MELVILLE_TOP_LEVEL OFF)
endif()
option(HOOD_MELVILLE_BUILD_TESTS "Собирать тесты (Catch2)" ${HOOD_MELVILLE_TOP_LEVEL})
option(HOOD_MELVILLE_BUILD_BENCH "Собирать нагрузку для бенчмарков" ${HOOD_MELVILLE_TOP_LEVEL})
option(HOOD_MELVILLE_INSTALL "Генерировать правила установки" ${HOOD_MELVILLE_TOP_LEVEL})
option(HOOD_MELVILLE_WERROR "Предупреждения компилятора — ошибки (CI, just lint)" OFF)

# Предупреждения для целей самого проекта (не для Catch2 и не для тех, кто
# подключает библиотеку): библиотека, тесты, нагрузки.
function(hood_melville_warnings)
    if(NOT HOOD_MELVILLE_TOP_LEVEL OR MSVC)
        return()
    endif()
    foreach(target IN LISTS ARGN)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            $<$<BOOL:${HOOD_MELVILLE_WERROR}>:-Werror>)
    endforeach()
endfunction()

include(GNUInstallDirs)

# Библиотека: экземпляр шаблона для int. Шаблон fqueue_template.h для своих
# типов ставится вместе с ней. В разделяемой сборке видны только публичные
# функции (FQUEUE_API в fqueue.h).
add_library(fqueue src/fqueue.c)
add_library(hood_melville::fqueue ALIAS fqueue)
target_include_directories(fqueue PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(fqueue PUBLIC c_std_11) # анонимные union в заголовке
set_target_properties(fqueue PROPERTIES
    C_VISIBILITY_PRESET hidden
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})

if(HOOD_MELVILLE_INSTALL)
    include(CMakePackageConfigHelpers)
    install(TARGETS fqueue EXPORT hood_melville_targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES include/fqueue.h include/fqueue_common.h include/fqueue_template.h
        include/fqueue_pool_impl.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    set(HOOD_MELVILLE_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/hood_melville)
    install(EXPORT hood_melville_targets
        FILE hood_melvilleTargets.cmake
        NAMESPACE hood_melville::
        DESTINATION ${HOOD_MELVILLE_CMAKE_DIR})
    configure_package_config_file(
        cmake/hood_melvilleConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/hood_melvilleConfig.cmake
        INSTALL_DESTINATION ${HOOD_MELVILLE_CMAKE_DIR})
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/hood_melvilleConfigVersion.cmake
        COMPATIBILITY SameMinorVersion) # до 1.0 минорная версия ломает API
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/hood_melvilleConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/hood_melvilleConfigVersion.cmake
        DESTINATION ${HOOD_MELVILLE_CMAKE_DIR})
endif()

if(HOOD_MELVILLE_BUILD_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 20)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.0
    )
    FetchContent_MakeAvailable(Catch2)
    enable_testing()

    add_executable(fqueue_test tests/fqueue_test.cxx tests/fqueue_generic_test.cxx
        tests/fqueue_generic.c tests/fqueue_sync_test.cxx tests/fqueue_pool_test.cxx)
    find_package(Threads REQUIRED)
    target_link_libraries(fqueue_test PRIVATE Threads::Threads)
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
        FQUEUE_IMPL_REFS_LIMIT=${FQUEUE_TEST_REFS_LIMIT}
        FQUEUE_IMPL_ATOMIC_REFS_LIMIT=${FQUEUE_TEST_REFS_LIMIT})
    add_executable(fqueue_limits_test tests/fqueue_limits_test.cxx)
    target_compile_definitions(fqueue_limits_test PRIVATE
        TEST_LEN_LIMIT=${FQUEUE_TEST_LEN_LIMIT}
        TEST_REFS_LIMIT=${FQUEUE_TEST_REFS_LIMIT})
    target_link_libraries(fqueue_limits_test PRIVATE fqueue_limits Catch2::Catch2WithMain)
    add_test(NAME fqueue_limits_test COMMAND fqueue_limits_test)
endif()

if(HOOD_MELVILLE_BUILD_BENCH)
    # Нагрузки для сравнения с Rust и Zig (just bench-*): максимальная
    # оптимизация независимо от CMAKE_BUILD_TYPE. Варианты: fqueue_sync
    # (атомарные счётчики ссылок) и пул узлов (docs/pool.md).
    set(HOOD_MELVILLE_BENCH_TARGETS)
    foreach(variant "" _sync _pool _pool_sync)
        set(target fqueue_bench${variant})
        add_executable(${target} bench/fqueue_bench.c src/fqueue.c)
        target_include_directories(${target} PRIVATE include)
        target_compile_options(${target} PRIVATE -O3 -march=native -flto -DNDEBUG)
        target_link_options(${target} PRIVATE -flto)
        if(variant MATCHES "_sync")
            target_compile_definitions(${target} PRIVATE FQUEUE_BENCH_SYNC)
        endif()
        if(variant MATCHES "_pool")
            target_compile_definitions(${target} PRIVATE FQUEUE_BENCH_POOL)
        endif()
        list(APPEND HOOD_MELVILLE_BENCH_TARGETS ${target})
    endforeach()
    # Параллельная нагрузка (#6): режим threads — pthreads, режим pool —
    # OpenMP, если он найден (на macOS: brew install libomp и
    # -DOpenMP_ROOT=$(brew --prefix libomp)).
    find_package(Threads REQUIRED)
    find_package(OpenMP COMPONENTS C)
    foreach(variant "" _pool)
        set(target fqueue_par${variant})
        add_executable(${target} bench/fqueue_par.c src/fqueue.c)
        target_include_directories(${target} PRIVATE include)
        target_compile_options(${target} PRIVATE -O3 -march=native -flto -DNDEBUG)
        target_link_options(${target} PRIVATE -flto)
        target_link_libraries(${target} PRIVATE Threads::Threads)
        if(OpenMP_C_FOUND)
            target_link_libraries(${target} PRIVATE OpenMP::OpenMP_C)
        endif()
        if(variant STREQUAL "_pool")
            target_compile_definitions(${target} PRIVATE FQUEUE_BENCH_POOL)
        endif()
        list(APPEND HOOD_MELVILLE_BENCH_TARGETS ${target})
    endforeach()
endif()

hood_melville_warnings(fqueue)
if(HOOD_MELVILLE_BUILD_TESTS)
    hood_melville_warnings(fqueue_test fqueue_limits fqueue_limits_test)
endif()
if(HOOD_MELVILLE_BUILD_BENCH)
    hood_melville_warnings(${HOOD_MELVILLE_BENCH_TARGETS})
endif()
