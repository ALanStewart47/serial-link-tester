cmake_minimum_required(VERSION 3.21)

project(CommandAtlas VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

find_package(Qt6 6.11.2 REQUIRED COMPONENTS Widgets)

add_executable(CommandAtlas WIN32
    main.cpp
    app/MainWindow.cpp
    app/MainWindow.h
    app/AppTheme.cpp
    app/AppTheme.h
    core/CommandCatalog.cpp
    core/CommandCatalog.h
    core/CommandDraftStore.cpp
    core/CommandDraftStore.h
    core/CommandTemplate.h
    core/FrameBuilder.cpp
    core/FrameBuilder.h
    ui/CommandDraftDialog.cpp
    ui/CommandDraftDialog.h
    resources/catalog.qrc
)

target_link_libraries(CommandAtlas PRIVATE Qt6::Widgets)
target_include_directories(CommandAtlas PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(CommandAtlas PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

if(MSVC)
    target_compile_options(CommandAtlas PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(CommandAtlas PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(CommandAtlas PROPERTIES
    OUTPUT_NAME "CommandAtlas"
    WIN32_EXECUTABLE TRUE
)
