#include "UpgradeProtocol.h"

#include <QList>

namespace UpperUpgrade {
namespace {

quint16 readU16Le(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint16>(static_cast<quint8>(data[offset])) |
           static_cast<quint16>(static_cast<quint8>(data[offset + 1]) << 8);
}

quint32 readU32Le(const QByteArray &data, qsizetype offset)
{
    return static_cast<quint32>(static_cast<quint8>(data[offset])) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 8) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 16) |
           (static_cast<quint32>(static_cast<quint8>(data[offset + 3])) << 24);
}

void appendU16Le(QByteArray &data, quint16 value)
{
    data.append(static_cast<char>(value & 0xFFu));
    data.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32Le(QByteArray &data, quint32 value)
{
    data.append(static_cast<char>(value & 0xFFu));
    data.append(static_cast<char>((value >> 8) & 0xFFu));
    data.append(static_cast<char>((value >> 16) & 0xFFu));
    data.append(static_cast<char>((value >> 24) & 0xFFu));
}

QString unknownValue(const QString &prefix, quint8 value)
{
    return QStringLiteral("%1(0x%2)").arg(prefix, QString::number(value, 16).toUpper().rightJustified(2, QLatin1Char('0')));
}

} // namespace

quint16 crc16CcittFalse(const QByteArray &data)
{
    quint16 crc = 0xFFFFu;
    for (char raw : data) {
        crc ^= static_cast<quint16>(static_cast<quint8>(raw)) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = static_cast<quint16>((crc << 1) ^ 0x1021u);
            } else {
                crc = static_cast<quint16>(crc << 1);
            }
        }
    }
    return crc;
}

quint32 crc32Ethernet(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (char raw : data) {
        crc ^= static_cast<quint8>(raw);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

QByteArray makeFrame(Command command, quint16 sequence, const QByteArray &payload)
{
    QByteArray frame;
    frame.reserve(HeaderSize + payload.size() + CrcSize);
    frame.append(static_cast<char>(Sof0));
    frame.append(static_cast<char>(Sof1));
    frame.append(static_cast<char>(ProtocolVersion));
    frame.append(static_cast<char>(command));
    appendU16Le(frame, sequence);
    appendU16Le(frame, static_cast<quint16>(payload.size()));
    frame.append(payload);

    const QByteArray crcInput = frame.mid(2);
    appendU16Le(frame, crc16CcittFalse(crcInput));
    return frame;
}

QByteArray makeBeginPayload(quint8 slaveId,
                            quint8 targetSlot,
                            quint32 imageSize,
                            quint32 imageCrc32,
                            quint32 version)
{
    QByteArray payload;
    payload.reserve(16);
    payload.append(static_cast<char>(slaveId));
    payload.append(static_cast<char>(targetSlot));
    appendU16Le(payload, 0);
    appendU32Le(payload, imageSize);
    appendU32Le(payload, imageCrc32);
    appendU32Le(payload, version);
    return payload;
}

QByteArray makeDataPayload(quint32 offset, const QByteArray &data)
{
    QByteArray payload;
    payload.reserve(4 + data.size());
    appendU32Le(payload, offset);
    payload.append(data);
    return payload;
}

std::optional<Frame> parseFrame(const QByteArray &data)
{
    if (data.size() < HeaderSize + CrcSize) {
        return std::nullopt;
    }
    if (static_cast<quint8>(data[0]) != Sof0 || static_cast<quint8>(data[1]) != Sof1) {
        return std::nullopt;
    }

    const quint16 payloadLength = readU16Le(data, 6);
    const qsizetype expectedLength = HeaderSize + payloadLength + CrcSize;
    if (data.size() != expectedLength) {
        return std::nullopt;
    }

    const quint16 receivedCrc = readU16Le(data, HeaderSize + payloadLength);
    const quint16 calculatedCrc = crc16CcittFalse(data.mid(2, 6 + payloadLength));
    if (receivedCrc != calculatedCrc) {
        return std::nullopt;
    }

    Frame frame;
    frame.version = static_cast<quint8>(data[2]);
    frame.command = static_cast<quint8>(data[3]);
    frame.sequence = readU16Le(data, 4);
    frame.payload = data.mid(HeaderSize, payloadLength);
    return frame;
}

std::optional<ResponsePayload> parseResponsePayload(const QByteArray &payload)
{
    if (payload.size() != ResponsePayloadSize) {
        return std::nullopt;
    }

    ResponsePayload response;
    response.status = static_cast<Status>(static_cast<quint8>(payload[0]));
    response.state = static_cast<SlaveState>(static_cast<quint8>(payload[1]));
    response.progressPercent = static_cast<quint8>(payload[2]);
    response.result = static_cast<SlaveResult>(static_cast<quint8>(payload[3]));
    response.receivedBytes = readU32Le(payload, 4);
    response.imageSize = readU32Le(payload, 8);
    response.maxDataPayload = readU16Le(payload, 12);
    response.activeSlaveId = static_cast<quint8>(payload[14]);
    response.flags = static_cast<quint8>(payload[15]);
    return response;
}

void FrameParser::append(const QByteArray &data)
{
    m_buffer.append(data);
}

QList<Frame> FrameParser::takeFrames()
{
    QList<Frame> frames;
    QByteArray sof;
    sof.append(static_cast<char>(Sof0));
    sof.append(static_cast<char>(Sof1));

    while (true) {
        const qsizetype sofIndex = m_buffer.indexOf(sof);
        if (sofIndex < 0) {
            m_buffer.clear();
            break;
        }
        if (sofIndex > 0) {
            m_buffer.remove(0, sofIndex);
        }
        if (m_buffer.size() < HeaderSize) {
            break;
        }

        const quint16 payloadLength = readU16Le(m_buffer, 6);
        const qsizetype frameLength = HeaderSize + payloadLength + CrcSize;
        if (payloadLength > 4 + DefaultMaxDataPayload || frameLength > HeaderSize + ResponsePayloadSize + CrcSize + 256) {
            m_buffer.remove(0, 1);
            continue;
        }
        if (m_buffer.size() < frameLength) {
            break;
        }

        const QByteArray rawFrame = m_buffer.left(frameLength);
        m_buffer.remove(0, frameLength);
        if (auto frame = parseFrame(rawFrame)) {
            frames.append(*frame);
        }
    }

    return frames;
}

void FrameParser::clear()
{
    m_buffer.clear();
}

bool isResponseFor(const Frame &frame, Command command)
{
    return frame.command == (static_cast<quint8>(command) | 0x80u);
}

QString commandName(quint8 command)
{
    const quint8 base = command & 0x7Fu;
    QString name;
    switch (static_cast<Command>(base)) {
    case Command::Query: name = QStringLiteral("QUERY"); break;
    case Command::Begin: name = QStringLiteral("BEGIN"); break;
    case Command::Data: name = QStringLiteral("DATA"); break;
    case Command::Status: name = QStringLiteral("STATUS"); break;
    case Command::Abort: name = QStringLiteral("ABORT"); break;
    case Command::End: name = QStringLiteral("END"); break;
    default: name = unknownValue(QStringLiteral("CMD"), base); break;
    }
    return (command & 0x80u) != 0u ? name + QStringLiteral("_RESP") : name;
}

QString statusText(Status status)
{
    switch (status) {
    case Status::Ok: return QStringLiteral("命令成功");
    case Status::Busy: return QStringLiteral("主机或从机忙，正在重试当前命令");
    case Status::BadParam: return QStringLiteral("参数非法");
    case Status::BadState: return QStringLiteral("当前状态不允许执行该命令");
    case Status::CrcError: return QStringLiteral("帧校验错误");
    case Status::SeqError: return QStringLiteral("升级序号异常");
    case Status::OffsetError: return QStringLiteral("固件分片偏移不连续或长度不匹配");
    case Status::SlaveError: return QStringLiteral("从机返回错误");
    case Status::InternalError: return QStringLiteral("主机内部错误");
    case Status::LengthError: return QStringLiteral("帧长度或负载长度错误");
    case Status::Unsupported: return QStringLiteral("协议版本或命令不支持");
    }
    return unknownValue(QStringLiteral("未知状态"), static_cast<quint8>(status));
}

QString stateText(SlaveState state)
{
    switch (state) {
    case SlaveState::Idle: return QStringLiteral("空闲");
    case SlaveState::Begin: return QStringLiteral("开始升级");
    case SlaveState::WaitBegin: return QStringLiteral("等待从机开始响应");
    case SlaveState::Erase: return QStringLiteral("擦除从机 inactive slot");
    case SlaveState::WaitErase: return QStringLiteral("等待擦除完成");
    case SlaveState::WaitData: return QStringLiteral("等待固件分片");
    case SlaveState::Write: return QStringLiteral("写入从机");
    case SlaveState::WaitWrite: return QStringLiteral("等待写入响应");
    case SlaveState::Verify: return QStringLiteral("校验固件");
    case SlaveState::WaitVerify: return QStringLiteral("等待校验完成");
    case SlaveState::Commit: return QStringLiteral("提交新固件");
    case SlaveState::WaitCommit: return QStringLiteral("等待提交完成");
    case SlaveState::Reboot: return QStringLiteral("重启从机");
    case SlaveState::WaitReboot: return QStringLiteral("等待从机重启");
    case SlaveState::Done: return QStringLiteral("升级完成");
    case SlaveState::Error: return QStringLiteral("升级错误");
    }
    return unknownValue(QStringLiteral("未知状态机"), static_cast<quint8>(state));
}

QString resultText(SlaveResult result)
{
    switch (result) {
    case SlaveResult::None: return QStringLiteral("无结果");
    case SlaveResult::Ok: return QStringLiteral("升级成功");
    case SlaveResult::Busy: return QStringLiteral("从机忙");
    case SlaveResult::BadParam: return QStringLiteral("升级参数错误");
    case SlaveResult::CommError: return QStringLiteral("主从通信错误");
    case SlaveResult::SlaveError: return QStringLiteral("从机升级错误");
    case SlaveResult::Timeout: return QStringLiteral("升级超时");
    case SlaveResult::Aborted: return QStringLiteral("升级已取消");
    }
    return unknownValue(QStringLiteral("未知结果"), static_cast<quint8>(result));
}

QString responseSummary(const ResponsePayload &payload)
{
    return QStringLiteral("%1，状态机：%2，结果：%3，进度：%4%，已接收：%5/%6 字节")
        .arg(statusText(payload.status),
             stateText(payload.state),
             resultText(payload.result),
             QString::number(payload.progressPercent),
             QString::number(payload.receivedBytes),
             QString::number(payload.imageSize));
}

} // namespace UpperUpgrade
