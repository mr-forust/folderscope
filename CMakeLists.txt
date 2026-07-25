cmake_minimum_required(VERSION 3.21)
project(searchmyfiles VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

qt_add_executable(searchmyfiles
    src/main.cpp
)

target_link_libraries(searchmyfiles PRIVATE Qt6::Widgets Qt6::Concurrent)
target_compile_options(searchmyfiles PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS searchmyfiles RUNTIME DESTINATION bin)
install(FILES packaging/searchmyfiles.desktop
        DESTINATION share/applications)

