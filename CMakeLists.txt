cmake_minimum_required(VERSION 3.16)
project(Pomodoro VERSION 1.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(pomodoro_core
    src/core/timer_engine.cpp
    src/persistence/state_store.cpp
    src/app/controller.cpp
    src/app/notifications.cpp)
target_include_directories(pomodoro_core PUBLIC include)
if(WIN32)
    # Apply to every consumer, including tests that include windows.h directly.
    # MSVC otherwise expands Win32's max macro in numeric_limits<T>::max().
    target_compile_definitions(pomodoro_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(pomodoro_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(pomodoro_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    enable_language(RC)
    # Native UI executables must share the Common Controls v6 activation
    # context, including tests. An unmanifested test defaults to Comctl32 v5.
    function(pomodoro_embed_windows_resources target)
        target_sources(${target} PRIVATE resources/pomodoro.rc)
        target_include_directories(${target} PRIVATE resources)
        if(MSVC)
            # The resource script already embeds manifest resource #1.
            target_link_options(${target} PRIVATE /MANIFEST:NO)
        endif()
    endfunction()

    add_executable(Pomodoro WIN32 src/ui/main_window.cpp
        src/platform/windows_shell.cpp src/platform/clock.cpp
        src/platform/windows_notifications.cpp
        src/platform/taskbar_status_panel.cpp)
    pomodoro_embed_windows_resources(Pomodoro)
    target_link_libraries(Pomodoro PRIVATE pomodoro_core comctl32 shell32 ole32
        uuid advapi32 user32 gdi32)
    target_compile_definitions(Pomodoro PRIVATE UNICODE _UNICODE
        _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    if(MSVC)
        target_compile_options(Pomodoro PRIVATE /W4 /permissive- /utf-8)
    elseif(MINGW)
        target_compile_options(Pomodoro PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(Pomodoro PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endif()
    install(TARGETS Pomodoro RUNTIME DESTINATION .)
    install(FILES README.md CHANGELOG.md Pomodoro_Development_Plan.md DESTINATION .)
    install(FILES Docs/Behavior.md Docs/Windows_Test_Checklist.md Docs/Validation.md
        Docs/GitHub_Releases.md DESTINATION Docs)
    install(PROGRAMS packaging/Install.ps1 packaging/Uninstall.ps1 packaging/Measure-Resources.ps1 DESTINATION .)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME "Pomodoro-${PROJECT_VERSION}-windows-x64")
    include(CPack)
else()
    message(STATUS "Building portable engine, persistence and controller tests; Win32 UI requires Windows.")
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(suite IN ITEMS timer_engine state_store controller notifications)
        add_executable(${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(${suite}_tests PRIVATE pomodoro_core)
        add_test(NAME ${suite} COMMAND ${suite}_tests)
        set_tests_properties(${suite} PROPERTIES TIMEOUT 30)
    endforeach()
    if(WIN32)
        add_executable(windows_ui_tests tests/windows_ui_tests.cpp
            src/platform/windows_shell.cpp src/platform/clock.cpp
            src/platform/windows_notifications.cpp
            src/platform/taskbar_status_panel.cpp)
        pomodoro_embed_windows_resources(windows_ui_tests)
        target_link_libraries(windows_ui_tests PRIVATE pomodoro_core comctl32 shell32
            ole32 uuid advapi32 user32 gdi32)
        target_compile_definitions(windows_ui_tests PRIVATE UNICODE _UNICODE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
        if(MSVC)
            target_compile_options(windows_ui_tests PRIVATE /W4 /permissive- /utf-8)
        elseif(MINGW)
            target_compile_options(windows_ui_tests PRIVATE -Wall -Wextra -Wpedantic)
            target_link_options(windows_ui_tests PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME windows_ui COMMAND windows_ui_tests)
        set_tests_properties(windows_ui PROPERTIES TIMEOUT 30 LABELS "windows;desktop")
    endif()
endif()
