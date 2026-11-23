#include "FpgaProtocol.h"

namespace FpgaProtocol {
namespace {

constexpr quint8 CmdSubBlockSize = 0x68; // FA 68 AA ...
constexpr quint8 CmdBlockSize    = 0xAA;
constexpr quint8 CmdTotalSize    = 0x00; // FA 00 ...

constexpr qsizetype SetFrameLen = 7;     // FA + 4 参数 + CRC16 = 7 字节
constexpr qsizetype CrcBytes    = 2;

QByteArray appendCrc(QByteArray frame)
{
    const quint16 crc = modbusCrc16(frame);
    frame.append(static_cast<char>((crc >> 8) & 0xFF)); // 大端在前，与 MCU crc_temp>>8 一致
    frame.append(static_cast<char>(crc & 0xFF));
    return frame;
}

} // namespace

quint16 modbusCrc16(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (char c : data) {
        crc ^= static_cast<quint8>(c);
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001u) {
                crc = static_cast<quint16>((crc >> 1) ^ 0xA001u);
            } else {
                crc = static_cast<quint16>(crc >> 1);
            }
        }
    }
    return crc;
}

QByteArray buildTriggerFpgaUpgrade()
{
    QByteArray f;
    f.reserve(SetFrameLen);
    f.append(static_cast<char>(FrameCmd));
    f.append(static_cast<char>(CmdSubBlockSize));
    f.append(static_cast<char>(CmdBlockSize));
    f.append(static_cast<char>(0x00));
    f.append(static_cast<char>(0x00));
    return appendCrc(f); // FA 68 AA 00 00 CRC16
}

QByteArray buildSetBlockSize(quint16 blockSize)
{
    QByteArray f;
    f.reserve(SetFrameLen);
    f.append(static_cast<char>(FrameCmd));
    f.append(static_cast<char>(CmdSubBlockSize));
    f.append(static_cast<char>(CmdBlockSize));
    f.append(static_cast<char>((blockSize >> 8) & 0xFF));
    f.append(static_cast<char>(blockSize & 0xFF));
    return appendCrc(f); // FA 68 AA H L CRC16
}

QByteArray buildSetTotalSize(quint32 blockCount)
{
    QByteArray f;
    f.reserve(SetFrameLen);
    f.append(static_cast<char>(FrameCmd));
    f.append(static_cast<char>(CmdTotalSize));
    f.append(static_cast<char>((blockCount >> 16) & 0xFF));
    f.append(static_cast<char>((blockCount >> 8) & 0xFF));
    f.append(static_cast<char>(blockCount & 0xFF));
    return appendCrc(f); // FA 00 b2 b1 b0 CRC16（3 字节大端，块数）
}

QByteArray buildWriteBlock(quint32 blockAddr1Based, const QByteArray &data)
{
    QByteArray f;
    f.reserve(7 + data.size());
    f.append(static_cast<char>(FrameCmd));
    f.append(static_cast<char>((blockAddr1Based >> 24) & 0xFF));
    f.append(static_cast<char>((blockAddr1Based >> 16) & 0xFF));
    f.append(static_cast<char>((blockAddr1Based >> 8) & 0xFF));
    f.append(static_cast<char>(blockAddr1Based & 0xFF));
    f.append(data);
    return appendCrc(f); // FA addr[4] data[N] CRC16
}

QByteArray buildStatusPoll()
{
    QByteArray f;
    f.append(static_cast<char>(FrameResp));
    f.append(static_cast<char>(0xFF));
    f.append(static_cast<char>(FrameResp));
    return f; // FB FF FB
}

QList<QByteArray> splitFirmware(const QByteArray &firmware, quint16 blockSize)
{
    QList<QByteArray> blocks;
    int offset = 0;
    while (offset < firmware.size()) {
        QByteArray block = firmware.mid(offset, blockSize);
        // Bootloader CMD_40_handle 只接受 lenght==7+block_size_t。
        // 短末块会被静默丢弃（既不回 73 也不回 FA FF），必须用 0xFF 补齐。
        if (block.size() < blockSize) {
            block.append(QByteArray(blockSize - block.size(), static_cast<char>(0xFF)));
        }
        blocks.append(block);
        offset += blockSize;
    }
    return blocks;
}

QList<ParsedResponse> parseResponses(QByteArray &buffer)
{
    QList<ParsedResponse> out;

    auto u8 = [&buffer](qsizetype i) { return static_cast<quint8>(buffer.at(i)); };

    while (!buffer.isEmpty()) {
        const quint8 b0 = u8(0);

        // —— 单字节 ACK ——
        if (b0 == AckOk || b0 == AckErr) {
            ParsedResponse resp;
            resp.type = (b0 == AckOk) ? ResponseType::AckOk : ResponseType::AckErr;
            out.append(resp);
            buffer.remove(0, 1);
            continue;
        }

        // —— FB <status> FB 状态帧 ——
        if (b0 == FrameResp) {
            if (buffer.size() < 3) {
                break; // 等更多字节
            }
            if (u8(2) == FrameResp) {
                ParsedResponse resp;
                resp.type = ResponseType::Status;
                resp.status = u8(1);
                out.append(resp);
                buffer.remove(0, 3);
                continue;
            }
            buffer.remove(0, 1); // 假 FB，丢弃重扫
            continue;
        }

        // —— FA 68 01 16 就绪 beacon ——
        if (b0 == FrameCmd && buffer.size() >= 4
            && u8(1) == 0x68 && u8(2) == 0x01 && u8(3) == 0x16) {
            ParsedResponse resp;
            resp.type = ResponseType::Beacon;
            out.append(resp);
            buffer.remove(0, 4);
            continue;
        }

        // —— FA FF FF FF FF <CRC16> 坏帧应答 ——
        if (b0 == FrameCmd && buffer.size() >= 7
            && u8(1) == 0xFF && u8(2) == 0xFF && u8(3) == 0xFF && u8(4) == 0xFF) {
            ParsedResponse resp;
            resp.type = ResponseType::CrcErrorAck;
            out.append(resp);
            buffer.remove(0, 7);
            continue;
        }

        // 无法识别（命令回显、0xFF 填充等），丢弃 1 字节重扫。
        buffer.remove(0, 1);
    }

    return out;
}

} // namespace FpgaProtocol
