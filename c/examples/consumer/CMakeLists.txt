# Пример потребителя установленной библиотеки:
#   cmake -S c -B build/c && cmake --install build/c --prefix <prefix>
#   cmake -S c/examples/consumer -B build/consumer -DCMAKE_PREFIX_PATH=<prefix>
cmake_minimum_required(VERSION 3.20)
project(fqueue_consumer LANGUAGES C)
find_package(hood_melville 0.1 REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE hood_melville::fqueue)
