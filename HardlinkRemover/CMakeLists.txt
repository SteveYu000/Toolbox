cmake_minimum_required(VERSION 3.20)

project(HardLinkRemover VERSION 2.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "HardLinkRemover requires Windows.")
endif()

option(HARDLINKREMOVER_BUILD_GUI "Build the native Windows GUI" ON)
option(BUILD_TESTING "Build HardLinkRemover tests" OFF)

if(BUILD_TESTING)
    enable_testing()
endif()

add_library(hardlink_core STATIC
    src/hardlink_core.cpp
)
target_include_directories(hardlink_core PUBLIC include)
target_compile_features(hardlink_core PUBLIC cxx_std_17)
target_compile_definitions(hardlink_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(HardLinkRemover
    src/cli_main.cpp
)
target_link_libraries(HardLinkRemover PRIVATE hardlink_core)
target_compile_definitions(HardLinkRemover PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(HARDLINKREMOVER_BUILD_GUI)
    add_executable(HardLinkRemoverGUI WIN32
        src/gui_main.cpp
        src/path_selection_dialog.cpp
        resources/HardLinkRemoverGUI.manifest
    )
    target_link_libraries(HardLinkRemoverGUI PRIVATE
        hardlink_core
        comctl32
        ole32
        shell32
    )
    target_compile_definitions(HardLinkRemoverGUI PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

function(hardlinkremover_set_warnings target)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

hardlinkremover_set_warnings(hardlink_core)
hardlinkremover_set_warnings(HardLinkRemover)
if(TARGET HardLinkRemoverGUI)
    hardlinkremover_set_warnings(HardLinkRemoverGUI)
endif()

if(BUILD_TESTING)
    add_executable(hardlink_core_tests
        tests/hardlink_core_tests.cpp
    )
    target_link_libraries(hardlink_core_tests PRIVATE hardlink_core)
    target_compile_definitions(hardlink_core_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(hardlink_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/$<CONFIG>"
    )
    hardlinkremover_set_warnings(hardlink_core_tests)
    add_test(NAME hardlink_core_tests COMMAND hardlink_core_tests)
    add_test(
        NAME hardlink_cli_integration
        COMMAND ${CMAKE_COMMAND}
            -DCLI_EXECUTABLE=$<TARGET_FILE:HardLinkRemover>
            -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-integration
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_integration.cmake
    )
endif()

install(TARGETS HardLinkRemover
    RUNTIME DESTINATION bin
)
if(TARGET HardLinkRemoverGUI)
    install(TARGETS HardLinkRemoverGUI
        RUNTIME DESTINATION bin
    )
endif()
