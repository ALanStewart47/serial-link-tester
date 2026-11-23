cmake_minimum_required(VERSION 3.19)
project(serial_test_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 处理 .qrc 资源（默认指令库 default_commands.json 内置到 exe）
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort Network Core)

qt_standard_project_setup()

qt_add_executable(serial_test_tool
    WIN32 MACOSX_BUNDLE
    main.cpp
    app/MainWindow.cpp
    app/MainWindow.h
    core/SerialTransport.cpp
    core/SerialTransport.h
    core/IByteTransport.h
    core/NetworkTransport.cpp
    core/NetworkTransport.h
    core/NetworkTransportWorker.cpp
    core/NetworkTransportWorker.h
    core/IoClock.h
    core/LinkHub.cpp
    core/LinkHub.h
    core/Ch9121ConfigProtocol.cpp
    core/Ch9121ConfigProtocol.h
    core/Ch9121ConfigClient.cpp
    core/Ch9121ConfigClient.h
    core/PacketBuilder.cpp
    core/PacketBuilder.h
    core/CommandItem.cpp
    core/CommandItem.h
    core/TestPlan.cpp core/TestPlan.h
    core/PlanAssessment.cpp core/PlanAssessment.h
    core/CommandLibrary.cpp
    core/CommandLibrary.h
    core/PlaylistStore.cpp
    core/PlaylistStore.h
    core/TestStatistics.cpp
    core/TestStatistics.h
    core/AutoSendEngine.cpp
    core/AutoSendEngine.h
    core/AppConfig.h
    core/CsvUtil.h
    core/LogManager.cpp
    core/LogManager.h
    core/CommandHistory.cpp
    core/CommandHistory.h
    core/TestVerdict.h
    ui/Ch9121ConfigDialog.cpp
    ui/Ch9121ConfigDialog.h
    ui/PortConnectionBar.cpp
    ui/PortConnectionBar.h
    ui/BasicSerialPage.cpp
    ui/BasicSerialPage.h
    ui/CommandEditDialog.cpp
    ui/CommandEditDialog.h
    ui/CommandTreeBuilder.cpp
    ui/CommandTreeBuilder.h
    ui/CommandLibraryPage.cpp
    ui/CommandLibraryPage.h
    ui/AutoSendPage.cpp
    ui/AutoSendPage.h
    ui/MultichannelDialog.cpp ui/MultichannelDialog.h
    ui/TestStatsTables.cpp
    ui/TestStatsTables.h
    ui/ResultPage.cpp
    ui/ResultPage.h
    ui/RateBar.cpp
    ui/RateBar.h
    ui/TrendChart.cpp
    ui/TrendChart.h
    ui/SettingsPage.cpp
    ui/SettingsPage.h
    resources/resources.qrc
)

# 让 #include "core/Xxx.h" 这类带子目录前缀的包含可用
target_include_directories(serial_test_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    enable_language(RC)
    target_sources(serial_test_tool PRIVATE resources/app.rc)
    target_include_directories(serial_test_tool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    set_source_files_properties(resources/app.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app-icon.ico")
endif()

target_link_libraries(serial_test_tool
    PRIVATE
        Qt6::Widgets
        Qt6::SerialPort
        Qt6::Network
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

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(ch9121_config_tests
        tests/Ch9121ConfigTest.cpp
        ui/PortConnectionBar.cpp ui/PortConnectionBar.h
        core/LinkHub.cpp core/LinkHub.h core/SerialTransport.cpp core/SerialTransport.h
        core/Ch9121ConfigProtocol.cpp core/Ch9121ConfigProtocol.h
        core/Ch9121ConfigClient.cpp core/Ch9121ConfigClient.h
        core/NetworkTransport.cpp core/NetworkTransport.h core/NetworkTransportWorker.cpp core/NetworkTransportWorker.h core/IoClock.h core/IByteTransport.h
        ui/Ch9121ConfigDialog.cpp ui/Ch9121ConfigDialog.h
    )
    target_include_directories(ch9121_config_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(ch9121_config_tests PRIVATE SERIAL_TEST_TOOL_TESTING)
    target_link_libraries(ch9121_config_tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Network Qt6::SerialPort)
    set_target_properties(ch9121_config_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME ch9121_config_tests COMMAND ch9121_config_tests -platform windows -o ch9121-results.txt,txt)
    set_tests_properties(ch9121_config_tests PROPERTIES TIMEOUT 30)
    qt_add_executable(multichannel_tests
        tests/MultichannelTest.cpp
        core/LogManager.cpp core/LogManager.h
        core/TestPlan.cpp core/TestPlan.h core/PlanAssessment.cpp core/PlanAssessment.h
        core/CommandItem.cpp core/CommandItem.h core/PacketBuilder.cpp core/PacketBuilder.h
        core/AutoSendEngine.cpp core/AutoSendEngine.h core/TestStatistics.cpp core/TestStatistics.h
        core/IByteTransport.h core/CommandLibrary.cpp core/CommandLibrary.h
        core/PlaylistStore.cpp core/PlaylistStore.h
        core/CommandHistory.cpp core/CommandHistory.h
        ui/MultichannelDialog.cpp ui/MultichannelDialog.h
        ui/CommandTreeBuilder.cpp ui/CommandTreeBuilder.h
        ui/AutoSendPage.cpp ui/AutoSendPage.h
        ui/BasicSerialPage.cpp ui/BasicSerialPage.h
        ui/TestStatsTables.cpp ui/TestStatsTables.h
        ui/ResultPage.cpp ui/ResultPage.h ui/RateBar.cpp ui/RateBar.h
        ui/TrendChart.cpp ui/TrendChart.h ui/SettingsPage.cpp ui/SettingsPage.h
    )
    target_include_directories(multichannel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(multichannel_tests PRIVATE SERIAL_TEST_TOOL_TESTING)
    target_link_libraries(multichannel_tests PRIVATE Qt6::Test Qt6::Widgets Qt6::Core)
    set_target_properties(multichannel_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME multichannel_tests COMMAND multichannel_tests -platform offscreen -o multichannel-results.txt,txt)
    set_tests_properties(multichannel_tests PROPERTIES TIMEOUT 60)

    qt_add_executable(transport_flow_tests
        tests/TransportFlowTest.cpp
        core/AutoSendEngine.cpp core/AutoSendEngine.h
        core/IByteTransport.h core/IoClock.h
        core/NetworkTransport.cpp core/NetworkTransport.h
        core/NetworkTransportWorker.cpp core/NetworkTransportWorker.h
        core/PacketBuilder.cpp core/CommandItem.cpp core/TestPlan.cpp core/TestStatistics.cpp
    )
    target_include_directories(transport_flow_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(transport_flow_tests PRIVATE Qt6::Test Qt6::Network Qt6::Core)
    set_target_properties(transport_flow_tests PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME transport_flow_tests COMMAND transport_flow_tests -o transport-results.txt,txt)
    set_tests_properties(transport_flow_tests PROPERTIES TIMEOUT 30)
endif()
