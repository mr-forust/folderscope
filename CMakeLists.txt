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

add_library(folderscope_core STATIC
    src/search_core.cpp
    src/search_core.h
)
target_link_libraries(folderscope_core PUBLIC Qt6::Core)
target_compile_options(folderscope_core PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(folderscope_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

qt_add_executable(searchmyfiles
    src/long_long_spin_box.cpp
    src/long_long_spin_box.h
    src/main.cpp
)

target_link_libraries(searchmyfiles PRIVATE folderscope_core Qt6::Widgets Qt6::Concurrent Qt6::DBus)
target_compile_options(searchmyfiles PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(searchmyfiles PRIVATE FOLDERSCOPE_VERSION="${PROJECT_VERSION}")

install(TARGETS searchmyfiles RUNTIME DESTINATION bin)
install(FILES packaging/searchmyfiles.desktop
        DESTINATION share/applications)

include(CTest)
if (BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(search_core_tests
        tests/search_core_test.cpp
    )
    target_link_libraries(search_core_tests PRIVATE folderscope_core Qt6::Test)
    target_compile_options(search_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME search-core COMMAND search_core_tests)

    qt_add_executable(long_long_spin_box_tests
        src/long_long_spin_box.cpp
        src/long_long_spin_box.h
        tests/long_long_spin_box_test.cpp
    )
    target_link_libraries(long_long_spin_box_tests PRIVATE Qt6::Widgets Qt6::Test)
    target_compile_options(long_long_spin_box_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(long_long_spin_box_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME long-long-spin-box COMMAND long_long_spin_box_tests)
    set_tests_properties(long-long-spin-box PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME release-rejects-untracked
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_dirty_test.sh"
                     "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release.sh")
endif()
