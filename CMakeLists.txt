cmake_minimum_required(VERSION 3.21)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FOLDERSCOPE_VERSION)
string(STRIP "${FOLDERSCOPE_VERSION}" FOLDERSCOPE_VERSION)
if (NOT FOLDERSCOPE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must use MAJOR.MINOR.PATCH: ${FOLDERSCOPE_VERSION}")
endif()

project(folderscope VERSION ${FOLDERSCOPE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent DBus)

qt_add_executable(searchmyfiles
    src/main.cpp
)

target_link_libraries(searchmyfiles PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::DBus)
target_compile_options(searchmyfiles PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(searchmyfiles PRIVATE FOLDERSCOPE_VERSION="${PROJECT_VERSION}")

install(TARGETS searchmyfiles RUNTIME DESTINATION bin)
install(FILES packaging/searchmyfiles.desktop
        DESTINATION share/applications)
