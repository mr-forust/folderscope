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

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent DBus LinguistTools)

add_library(folderscope_core STATIC
    src/search_core.cpp
    src/search_core.h
)
target_link_libraries(folderscope_core PUBLIC Qt6::Core)
target_compile_options(folderscope_core PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(folderscope_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

qt_add_executable(folderscope
    src/long_long_spin_box.cpp
    src/long_long_spin_box.h
    src/main.cpp
    src/main_window.h
    src/main_window.cpp
    src/search_cache.h
    src/search_cache.cpp
    src/search_engine.h
    src/search_engine.cpp
    src/results_model.h
    src/results_model.cpp
    src/options_dialog.h
    src/options_dialog.cpp
)

target_link_libraries(folderscope PRIVATE folderscope_core Qt6::Widgets Qt6::Concurrent Qt6::DBus)
target_compile_options(folderscope PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(folderscope PRIVATE FOLDERSCOPE_VERSION="${PROJECT_VERSION}")

qt_add_translations(folderscope
    TS_FILES translations/folderscope_ru.ts
)

install(TARGETS folderscope RUNTIME DESTINATION bin)
install(FILES packaging/org.folderscope.FolderScope.desktop
        DESTINATION share/applications)
install(FILES packaging/folderscope.svg
        DESTINATION share/icons/hicolor/scalable/apps
        RENAME org.folderscope.FolderScope.svg)
install(FILES packaging/org.folderscope.FolderScope.metainfo.xml
        DESTINATION share/metainfo)
install(FILES packaging/man/folderscope.1
        DESTINATION share/man/man1)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/folderscope_ru.qm"
        DESTINATION share/folderscope/translations)

set(CPACK_PACKAGE_NAME "folderscope")
set(CPACK_PACKAGE_VENDOR "FolderScope contributors")
set(CPACK_PACKAGE_CONTACT "FolderScope contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Fast advanced file search for Linux")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "DEB;RPM")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6core6 (>= 6.2), libqt6widgets6 (>= 6.2), libqt6dbus6 (>= 6.2)")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_GROUP "Applications/FileTools")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
include(CPack)

include(CTest)
if (BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(search_core_tests
        tests/search_core_test.cpp
    )
    target_link_libraries(search_core_tests PRIVATE folderscope_core Qt6::Test)
    target_compile_options(search_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME search-core COMMAND search_core_tests)

    qt_add_executable(search_cache_tests
        src/search_cache.cpp
        src/search_cache.h
        tests/search_cache_test.cpp
    )
    target_link_libraries(search_cache_tests PRIVATE folderscope_core Qt6::Test)
    target_compile_options(search_cache_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(search_cache_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME search-cache COMMAND search_cache_tests)

    qt_add_executable(search_engine_tests
        src/search_cache.cpp
        src/search_cache.h
        src/search_engine.cpp
        src/search_engine.h
        tests/search_engine_test.cpp
    )
    target_link_libraries(search_engine_tests PRIVATE
        folderscope_core Qt6::Concurrent Qt6::Test)
    target_compile_options(search_engine_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(search_engine_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME search-engine COMMAND search_engine_tests)

    qt_add_executable(options_dialog_tests
        src/long_long_spin_box.cpp
        src/long_long_spin_box.h
        src/options_dialog.cpp
        src/options_dialog.h
        tests/options_dialog_test.cpp
    )
    target_link_libraries(options_dialog_tests PRIVATE
        folderscope_core Qt6::Widgets Qt6::Test)
    target_compile_options(options_dialog_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_include_directories(options_dialog_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME options-dialog COMMAND options_dialog_tests)
    set_tests_properties(options-dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

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
