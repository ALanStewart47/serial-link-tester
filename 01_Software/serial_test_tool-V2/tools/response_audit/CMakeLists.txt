cmake_minimum_required(VERSION 3.19)
project(response_audit LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Core)
get_filename_component(APP "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_executable(response_audit
    main.cpp
    ${APP}/core/IByteTransport.h
    ${APP}/core/NetworkTransport.cpp ${APP}/core/NetworkTransport.h
    ${APP}/core/NetworkTransportWorker.cpp ${APP}/core/NetworkTransportWorker.h
    ${APP}/core/IoClock.h
    ${APP}/core/AutoSendEngine.cpp ${APP}/core/AutoSendEngine.h
    ${APP}/core/TestStatistics.cpp ${APP}/core/TestStatistics.h
    ${APP}/core/PacketBuilder.cpp ${APP}/core/CommandItem.cpp
    ${APP}/core/TestPlan.cpp ${APP}/core/PlanAssessment.cpp
    ${APP}/core/LogManager.cpp ${APP}/core/LogManager.h
    ${APP}/ui/BasicSerialPage.cpp ${APP}/ui/BasicSerialPage.h
    ${APP}/ui/ResultPage.cpp ${APP}/ui/ResultPage.h
    ${APP}/ui/TestStatsTables.cpp ${APP}/ui/TestStatsTables.h
    ${APP}/ui/RateBar.cpp ${APP}/ui/RateBar.h
    ${APP}/ui/TrendChart.cpp ${APP}/ui/TrendChart.h
    ${APP}/ui/SettingsPage.cpp ${APP}/ui/SettingsPage.h)
target_include_directories(response_audit PRIVATE ${APP})
target_link_libraries(response_audit PRIVATE Qt6::Widgets Qt6::Network Qt6::Core)
