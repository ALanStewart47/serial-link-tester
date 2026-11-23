#include "IspProtocol.h"

namespace IspProtocol {

namespace {
constexpr quint8 H0      = 0x72;
constexpr quint8 H1_68   = 0x68;
constexpr quint8 H1_69   = 0x69;
constexpr quint8 CMD_BOOT_READY = 0x01;
constexpr quint8 CMD_COUNT_ACK  = 0x02;
constexpr quint8 CMD_CHKSUM_BAD = 0x03;
constexpr quint8 CMD_VERSION    = 0xBB; // 设备回版本用 BB
constexpr quint8 CMD_TRIGGER    = 0xAA;
constexpr quint8 CMD_BOOT_QUERY = 0xF0;
} // namespace

QByteArray buildVersionRead()
{
    QByteArray f;
    f.append(static_cast<char>(H0));
    f.append(static_cast<char>(H1_68));
    f.append(static_cast<char>(CMD_VERSION));
    f.append(static_cast<char>(FrameEnd));
    return f; // 72 68 BB 16
}

QByteArray buildTrigger(quint8 verMajor, quint8 verMinor)
{
    QByteArray f;
    f.append(static_cast<char>(H0));
    f.append(static_cast<char>(H1_68));
    f.append(static_cast<char>(CMD_TRIGGER));
    f.append(static_cast<char>(verMajor));
    f.append(static_cast<char>(verMinor));
    f.append(static_cast<char>(FrameEnd));
    return f; // 72 68 AA maj min 16
}

QByteArray buildCount(quint16 totalPackets)
{
    QByteArray f;
    f.append(static_cast<char>(H0));
    f.append(static_cast<char>(H1_68));
    f.append(static_cast<char>((totalPackets >> 8) & 0xFF));
    f.append(static_cast<char>(totalPackets & 0xFF));
    f.append(static_cast<char>(FrameEnd));
    return f; // 72 68 cntH cntL 16
}

QByteArray buildBootVersionQuery()
{
    QByteArray f;
    f.append(static_cast<char>(H0));
    f.append(static_cast<char>(H1_68));
    f.append(static_cast<char>(CMD_BOOT_QUERY));
    f.append(static_cast<char>(FrameEnd));
    return f; // 72 68 F0 16
}

quint8 xor8(const QByteArray &data)
{
    quint8 res = 0;
    for (char c : data) {
        res ^= static_cast<quint8>(c);
    }
    return res;
}

QByteArray buildDataPacket(quint16 index1Based, const QByteArray &data256)
{
    // data256 必须正好 256 字节（由 splitFirmware 保证）。
    QByteArray payload = data256;
    if (payload.size() != DataBytesPerPacket) {
        payload.resize(DataBytesPerPacket); // 防御：截断/补 0；正常路径不会触发
        for (int i = data256.size(); i < DataBytesPerPacket; ++i) {
            payload[i] = static_cast<char>(PadByte);
        }
    }

    QByteArray f;
    f.reserve(DataFrameSize);
    f.append(static_cast<char>(H0));
    f.append(static_cast<char>(H1_69));
    f.append(static_cast<char>((index1Based >> 8) & 0xFF));
    f.append(static_cast<char>(index1Based & 0xFF));
    f.append(payload);
    f.append(static_cast<char>(xor8(payload)));
    f.append(static_cast<char>(FrameEnd));
    return f; // 72 69 idxH idxL [256] xor 16
}

QList<QByteArray> splitFirmware(const QByteArray &firmware)
{
    QList<QByteArray> packets;
    int offset = 0;
    while (offset < firmware.size()) {
        QByteArray chunk = firmware.mid(offset, DataBytesPerPacket);
        if (chunk.size() < DataBytesPerPacket) {
            const int oldSize = chunk.size();
            chunk.resize(DataBytesPerPacket);
            for (int i = oldSize; i < DataBytesPerPacket; ++i) {
                chunk[i] = static_cast<char>(PadByte);
            }
        }
        packets.append(chunk);
        offset += DataBytesPerPacket;
    }
    return packets;
}

QList<ParsedFrame> parseFrames(QByteArray &buffer)
{
    QList<ParsedFrame> out;

    auto u8 = [&buffer](int i) { return static_cast<quint8>(buffer.at(i)); };

    while (!buffer.isEmpty()) {
        // 找到第一个帧头 0x72，丢弃前面的杂散字节。
        if (u8(0) != H0) {
            int start = -1;
            for (int i = 1; i < buffer.size(); ++i) {
                if (static_cast<quint8>(buffer.at(i)) == H0) {
                    start = i;
                    break;
                }
            }
            if (start < 0) {
                buffer.clear();
                break;
            }
            buffer.remove(0, start);
            continue;
        }

        if (buffer.size() < 4) {
            break; // 至少需要 4 字节才能判断最短帧
        }

        const quint8 b1 = u8(1);
        if (b1 == H1_68) {
            const quint8 cmd = u8(2);
            if (cmd == CMD_BOOT_READY || cmd == CMD_COUNT_ACK || cmd == CMD_CHKSUM_BAD) {
                if (u8(3) == FrameEnd) {
                    ParsedFrame pf;
                    pf.type = (cmd == CMD_BOOT_READY) ? FrameType::BootReady
                            : (cmd == CMD_COUNT_ACK)  ? FrameType::CountAck
                                                      : FrameType::ChecksumFail;
                    out.append(pf);
                    buffer.remove(0, 4);
                    continue;
                }
                buffer.remove(0, 1); // 假帧头，丢 1 字节重扫
                continue;
            }
            if (cmd == CMD_VERSION) {
                if (buffer.size() < 6) {
                    break; // 等更多字节
                }
                if (u8(5) == FrameEnd) {
                    ParsedFrame pf;
                    pf.type = FrameType::Version;
                    pf.verMajor = u8(3);
                    pf.verMinor = u8(4);
                    out.append(pf);
                    buffer.remove(0, 6);
                    continue;
                }
                buffer.remove(0, 1);
                continue;
            }
            if (cmd == CMD_BOOT_QUERY) {
                if (buffer.size() < 6) {
                    break;
                }
                if (u8(5) == FrameEnd) {
                    ParsedFrame pf;
                    pf.type = FrameType::BootVersionQuery;
                    pf.querySrc = u8(3);
                    pf.queryVer = u8(4);
                    out.append(pf);
                    buffer.remove(0, 6);
                    continue;
                }
                buffer.remove(0, 1);
                continue;
            }
            buffer.remove(0, 1); // 未知 0x68 子命令
            continue;
        }

        if (b1 == H1_69) {
            if (buffer.size() < 5) {
                break;
            }
            if (u8(4) == FrameEnd) {
                ParsedFrame pf;
                pf.type = FrameType::DataAck;
                pf.index = static_cast<quint16>((u8(2) << 8) | u8(3));
                out.append(pf);
                buffer.remove(0, 5);
                continue;
            }
            buffer.remove(0, 1);
            continue;
        }

        // 0x72 后面不是 0x68/0x69，丢掉这个 0x72 重扫。
        buffer.remove(0, 1);
    }

    return out;
}

} // namespace IspProtocol
