cmake_minimum_required(VERSION 3.19)
project(cst_isp_flasher LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort Network)

qt_standard_project_setup()

# resources.qrc 需要 AUTORCC 才会被 rcc 编进 exe（qt_standard_project_setup 不开 AUTORCC）
set(CMAKE_AUTORCC ON)

# Windows exe 图标：app_icon.rc 引用 app.ico，由 windres/rc 编进可执行文件。
set(CST_ISP_WIN_RESOURCES "")
if(WIN32)
    set(CST_ISP_WIN_RESOURCES app_icon.rc)
endif()

qt_add_executable(cst_isp_flasher
    WIN32 MACOSX_BUNDLE
    main.cpp
    IspMainWindow.cpp        IspMainWindow.h
    IspSerialPort.cpp        IspSerialPort.h
    IspUpgradeController.cpp IspUpgradeController.h
    IspProtocol.cpp          IspProtocol.h
    IspTransport.h
    # FPGA 升级（FA/FB + Modbus CRC16；串口直连 + CH9121 网络透传）
    FpgaProtocol.cpp         FpgaProtocol.h
    FpgaUpgradeController.cpp FpgaUpgradeController.h
    IspNetworkTransport.cpp  IspNetworkTransport.h
    Ch9121Scanner.cpp        Ch9121Scanner.h
    W5500ConfigProtocol.cpp  W5500ConfigProtocol.h
    FirmwareFile.cpp         FirmwareFile.h
    resources.qrc
    ${CST_ISP_WIN_RESOURCES}
)

target_link_libraries(cst_isp_flasher
    PRIVATE
        Qt6::Widgets
        Qt6::SerialPort
        Qt6::Network
)

# 生成的 exe 取中性名（不含公司名），与发布物一致。
set_target_properties(cst_isp_flasher PROPERTIES OUTPUT_NAME "isp_flasher")

include(GNUInstallDirs)

install(TARGETS cst_isp_flasher
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET cst_isp_flasher
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# —— 协议层 + 状态机单元测试（仅需 Qt6::Test，不依赖 SerialPort/Network，本机可跑）——
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(TARGET Qt6::Test)
        qt_add_executable(isp_protocol_tests
            tests/IspProtocolTest.cpp
            IspProtocol.cpp          IspProtocol.h
            IspUpgradeController.cpp IspUpgradeController.h
            IspTransport.h
        )
        target_include_directories(isp_protocol_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(isp_protocol_tests
            PRIVATE
                Qt6::Test
        )
        add_test(NAME isp_protocol_tests COMMAND isp_protocol_tests)

        qt_add_executable(fpga_protocol_tests
            tests/FpgaProtocolTest.cpp
            FpgaProtocol.cpp         FpgaProtocol.h
            FpgaUpgradeController.cpp FpgaUpgradeController.h
            IspTransport.h
        )
        target_include_directories(fpga_protocol_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(fpga_protocol_tests
            PRIVATE
                Qt6::Test
        )
        add_test(NAME fpga_protocol_tests COMMAND fpga_protocol_tests)

        qt_add_executable(firmware_file_tests
            tests/FirmwareFileTest.cpp
            FirmwareFile.cpp         FirmwareFile.h
        )
        target_include_directories(firmware_file_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(firmware_file_tests
            PRIVATE
                Qt6::Test
        )
        add_test(NAME firmware_file_tests COMMAND firmware_file_tests)
    endif()
endif()
