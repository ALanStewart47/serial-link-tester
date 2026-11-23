#pragma once

// FPGA 固件升级协议（FA/FB + Modbus CRC16）的纯逻辑层。
//
// 不依赖 Qt Widgets / QSerialPort / QNetwork，只用 QByteArray，便于离线单元测试。
// 协议来源（已交叉验证）：
//   - 00_Reference/legacy/software/Boot_STM32_anlu/MDK-ARM/APP/Src/protocol_public.c
//     （CMD_40_handle：FA/FB 命令与状态响应）
//   - 00_Reference/legacy/software/Boot_STM32_anlu/MDK-ARM/APP/Src/uart.c
//     （uart_fpga_updata：擦除 W25Q128 12×64KB @ 0x0C0000、写块、完成后 FPGA 多启动复位）
//
// 物理通道：
//   - 直连串口：USART3 @ 115200 8N1（Bootloader 硬编码）
//   - CH9121 网络：TCP → CH9121 透传 → USART1 @ 19200 8N1（Bootloader 由 Flash 配置，默认 19200）
//
// 帧格式：
//   命令：FA <subcmd/addr> <params...> <CRC16_MSB> <CRC16_LSB>   （CRC 大端在前，与 MCU 一致）
//   应答：73 单字节 ACK / 65 参数错误 / FB <status> FB 状态帧 / FA FF FF FF FF <CRC> 坏帧应答
//   就绪 beacon：FA 68 01 16（MCU 进入 FPGA 升级模式时发送一次，仅提示，不可依赖）

#include <QByteArray>
#include <QList>
#include <QtGlobal>

namespace FpgaProtocol {

// —— 常量 ——
constexpr quint8  FrameCmd      = 0xFA;           // PC → MCU 命令帧头
constexpr quint8  FrameResp     = 0xFB;           // MCU → PC 状态帧头
constexpr quint16 MinBlockSize  = 256;            // MCU 校验：256..2048 且整除 256
constexpr quint16 MaxBlockSize  = 2048;
constexpr quint16 DefaultBlockSize = 1024;        // 协议层默认块大小（UI 可配）
constexpr quint32 MaxFpgaImageSize = 12u * 64u * 1024u; // 安路：擦除区固定 12×64KB = 768KB
constexpr quint32 MaxGowinImageSize = 512u * 1024u;     // 高云硬上限（Boot 未强制；避免误灌超大文件）
constexpr quint32 GowinImageWarnSize = 0x1B200u;        // Boot 注释掉的 max_addr，仅警告不拒绝

// 单字节 ACK
constexpr quint8  AckOk         = 0x73;
constexpr quint8  AckErr        = 0x65;

// FB <status> FB 状态码
constexpr quint8  StatusIdle    = 0x00;           // 空闲/就绪
constexpr quint8  StatusBusy    = 0x01;           // 忙（擦除或写入 W25Q128 中）
constexpr quint8  StatusVerifyFail = 0x02;        // 校验失败

// —— CRC ——
// Modbus CRC-16（多项式 0xA001，初值 0xFFFF），与 MCU modbus_crc() 一致。
quint16 modbusCrc16(const QByteArray &data);

// —— PC → MCU 建帧 ——
// FA 68 AA 00 00 <CRC16> —— 触发 MCU 进入 FPGA 升级模式（在 App 模式发送）。
// App（CST-TPS481500C-S4-8T-MCU-V2.0 protocol_public.c fpga_upguade_cmdhandle）
// 收到后回 73，写 0x55 到 Flash 0x08003400、复位 FPGA 并系统复位；
// 若设备已在 Bootloader（FPGA 升级模式），该帧会被当作块大小 0 拒绝（回 65）。
QByteArray buildTriggerFpgaUpgrade();
// FA 68 AA <blockH> <blockL> <CRC16> （7 字节）。block 须 256..2048 且整除 256。
QByteArray buildSetBlockSize(quint16 blockSize);
// FA 00 <size[23:16]> <size[15:8]> <size[7:0]> <CRC16> （7 字节）。
// 注意：参数是「总块数」（1-based 块索引到达该值时 MCU 判定烧录完成），不是字节数。
QByteArray buildSetTotalSize(quint32 blockCount);
// FA <addr[31:24]> <addr[23:16]> <addr[15:8]> <addr[7:0]> <data[N]> <CRC16>。
// addr 为 1-based 块索引，MCU 内部映射到 W25Q128 偏移 (addr-1)*blockSize + 0x0C0000。
QByteArray buildWriteBlock(quint32 blockAddr1Based, const QByteArray &data);
// FB FF FB —— 状态轮询帧。
QByteArray buildStatusPoll();

// 把固件二进制按 blockSize 切成块；末块用 0xFF 补齐到 blockSize。
// Bootloader 写块帧必须是「7 + 块大小」字节（含 CRC），短帧会被静默丢弃。
QList<QByteArray> splitFirmware(const QByteArray &firmware, quint16 blockSize);

// —— MCU → PC 帧解析 ——
enum class ResponseType {
    None,
    AckOk,        // 0x73        命令已接受（在 ISR 中立即回复，不代表处理完成）
    AckErr,       // 0x65        参数错误（块大小非法）
    Status,       // FB xx FB    状态帧（xx=0 空闲 / 1 忙 / 2 校验失败）
    Beacon,       // FA 68 01 16 FPGA 升级模式就绪（仅启动时发一次）
    CrcErrorAck,  // FA FF FF FF FF <CRC16>  坏帧应答（MCU 收到 CRC 错误的帧时）
};

struct ParsedResponse {
    ResponseType type = ResponseType::None;
    quint8 status = 0; // Status 帧的状态字节
};

// 从累积缓冲 buffer 中解析出全部可识别响应并消费；不完整残帧保留待下次。
// 无法识别的字节被丢弃（包括命令回显、0xFF 填充等）。
QList<ParsedResponse> parseResponses(QByteArray &buffer);

} // namespace FpgaProtocol
