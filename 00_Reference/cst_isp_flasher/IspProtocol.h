#pragma once

// Bootloader 串口升级协议（旧串口 ISP 协议）的纯逻辑层。
//
// 不依赖 Qt Widgets / QSerialPort，只用 QByteArray，便于离线单元测试。
// 协议来源（已交叉验证）：
//   - Reference/.../G4_BootLoader_V2.0/Core/Bsp/Src/uart.c        （Bootloader 端）
//   - Reference/.../BootLoader说明文档/升级功能帧格式.doc        （帧格式）
//   - software/MDPS24120C-4TDP_V0.1.3/Protocol/protocol_transfer.c（控制器 App 端）
//
// 所有帧：UART 19200 8N1，IDLE 空闲帧分包，XOR8 校验（非 CRC）。

#include <QByteArray>
#include <QList>

namespace IspProtocol {

// —— 常量 ——
constexpr int     DataBytesPerPacket = 256;    // 每个数据帧承载的固件字节数
constexpr int     DataFrameSize      = 262;    // 72 69 idxH idxL [256] xor 16
constexpr quint8  PadByte            = 0xFF;   // 末包不足 256 字节的填充值（Flash 擦除态）
constexpr quint32 MaxPackets         = 65535;  // 包序号是 2 字节，协议上限
constexpr quint32 MaxImageSize       = MaxPackets * DataBytesPerPacket; // ≈16MB，仅协议层上限，不限制具体芯片
constexpr quint8  FrameEnd           = 0x16;

// —— PC → 设备 建帧 ——
QByteArray buildVersionRead();                                  // 72 68 BB 16          （纯读版本，无副作用）
QByteArray buildTrigger(quint8 verMajor, quint8 verMinor);      // 72 68 AA maj min 16  （比较版本，按需进 Boot）
QByteArray buildCount(quint16 totalPackets);                    // 72 68 cntH cntL 16
QByteArray buildBootVersionQuery();                             // 72 68 F0 16          （网口升 MCU 前查 Boot 版本）
QByteArray buildDataPacket(quint16 index1Based,
                           const QByteArray &data256);          // 72 69 idxH idxL [256] xor 16

// XOR8 校验：对 data 的所有字节做异或。
quint8 xor8(const QByteArray &data);

// 把固件二进制切成若干 256 字节块，末块用 0xFF 补齐到 256。
QList<QByteArray> splitFirmware(const QByteArray &firmware);

// —— 设备 → PC 帧解析 ——
enum class FrameType {
    None,
    BootReady,         // 72 68 01 16           进入 Bootloader 就绪
    CountAck,          // 72 68 02 16           收到总包数，开始下发数据
    ChecksumFail,      // 72 68 03 16           某包校验失败，需重发
    Version,           // 72 68 BB maj min 16   当前版本号
    BootVersionQuery,  // 72 68 F0 src ver 16   App/Boot 版本查询应答
    DataAck            // 72 69 idxH idxL 16    某数据包确认
};

struct ParsedFrame {
    FrameType type      = FrameType::None;
    quint16   index     = 0;   // DataAck 的序号
    quint8    verMajor  = 0;   // Version 的主版本
    quint8    verMinor  = 0;   // Version 的次版本
    quint8    querySrc  = 0;   // BootVersionQuery：01=App，02=Boot
    quint8    queryVer  = 0;   // BootVersionQuery：packed (major<<4)|minor
};

// 从累积缓冲 buffer 中尽可能多地解析出完整帧，消费掉已识别字节；
// 残帧（字节不足）保留在 buffer 头部等待下次。无法识别的前导字节被丢弃。
QList<ParsedFrame> parseFrames(QByteArray &buffer);

} // namespace IspProtocol
