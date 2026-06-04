cmake_minimum_required(VERSION 3.19)
project(upper_upgrade LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort)

qt_standard_project_setup()

qt_add_executable(upper_upgrade
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    SerialTransport.cpp UpgradeController.cpp UpgradeProtocol.cpp
    SerialTransport.h UpgradeController.h UpgradeProtocol.h
)

target_link_libraries(upper_upgrade
    PRIVATE
        Qt6::Widgets
        Qt6::SerialPort
)

include(GNUInstallDirs)

install(TARGETS upper_upgrade
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET upper_upgrade
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
