cmake_minimum_required(VERSION 3.19)
project(serial_test_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 处理 .qrc 资源（默认指令库 default_commands.json 内置到 exe）
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort Core)

qt_standard_project_setup()

qt_add_executable(serial_test_tool
    WIN32 MACOSX_BUNDLE
    main.cpp
    app/MainWindow.cpp
    app/MainWindow.h
    core/SerialTransport.cpp
    core/SerialTransport.h
    core/PacketBuilder.cpp
    core/PacketBuilder.h
    core/CommandItem.cpp
    core/CommandItem.h
    core/CommandLibrary.cpp
    core/CommandLibrary.h
    core/TestStatistics.cpp
    core/TestStatistics.h
    core/AutoSendEngine.cpp
    core/AutoSendEngine.h
    core/AppConfig.h
    core/CsvUtil.h
    core/LogManager.cpp
    core/LogManager.h
    ui/BasicSerialPage.cpp
    ui/BasicSerialPage.h
    ui/CommandEditDialog.cpp
    ui/CommandEditDialog.h
    ui/CommandLibraryPage.cpp
    ui/CommandLibraryPage.h
    ui/AutoSendPage.cpp
    ui/AutoSendPage.h
    ui/ResultPage.cpp
    ui/ResultPage.h
    ui/RateBar.cpp
    ui/RateBar.h
    ui/SettingsPage.cpp
    ui/SettingsPage.h
    resources/resources.qrc
)

# 让 #include "core/Xxx.h" 这类带子目录前缀的包含可用
target_include_directories(serial_test_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(serial_test_tool
    PRIVATE
        Qt6::Widgets
        Qt6::SerialPort
        Qt6::Core
)

include(GNUInstallDirs)

install(TARGETS serial_test_tool
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET serial_test_tool
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
