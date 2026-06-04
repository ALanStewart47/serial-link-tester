#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <optional>

namespace UpperUpgrade {

constexpr quint8 Sof0 = 0x55;
constexpr quint8 Sof1 = 0xAA;
constexpr quint8 ProtocolVersion = 0x01;
constexpr qsizetype HeaderSize = 8;
constexpr qsizetype CrcSize = 2;
constexpr qsizetype ResponsePayloadSize = 16;
constexpr quint16 DefaultMaxDataPayload = 128;
constexpr quint32 MaxImageSize = 48u * 1024u;

enum class Command : quint8 {
    Query = 0x01,
    Begin = 0x02,
    Data = 0x03,
    Status = 0x04,
    Abort = 0x05,
    End = 0x06,
};

enum class Status : quint8 {
    Ok = 0,
    Busy = 1,
    BadParam = 2,
    BadState = 3,
    CrcError = 4,
    SeqError = 5,
    OffsetError = 6,
    SlaveError = 7,
    InternalError = 8,
    LengthError = 9,
    Unsupported = 10,
};

enum class SlaveState : quint8 {
    Idle = 0,
    Begin,
    WaitBegin,
    Erase,
    WaitErase,
    WaitData,
    Write,
    WaitWrite,
    Verify,
    WaitVerify,
    Commit,
    WaitCommit,
    Reboot,
    WaitReboot,
    Done,
    Error,
};

enum class SlaveResult : quint8 {
    None = 0,
    Ok,
    Busy,
    BadParam,
    CommError,
    SlaveError,
    Timeout,
    Aborted,
};

struct Frame {
    quint8 version = ProtocolVersion;
    quint8 command = 0;
    quint16 sequence = 0;
    QByteArray payload;
};

struct ResponsePayload {
    Status status = Status::InternalError;
    SlaveState state = SlaveState::Idle;
    quint8 progressPercent = 0;
    SlaveResult result = SlaveResult::None;
    quint32 receivedBytes = 0;
    quint32 imageSize = 0;
    quint16 maxDataPayload = DefaultMaxDataPayload;
    quint8 activeSlaveId = 0;
    quint8 flags = 0;

    bool canAcceptData() const { return (flags & 0x01u) != 0; }
};

enum class ParseError {
    NeedMoreData,
    BadCrc,
    BadLength,
    BadVersion,
};

struct ParseResult {
    std::optional<Frame> frame;
    std::optional<ParseError> error;
    qsizetype consumed = 0;
};

class FrameParser
{
public:
    void append(const QByteArray &data);
    QList<Frame> takeFrames();
    void clear();

private:
    QByteArray m_buffer;
};

quint16 crc16CcittFalse(const QByteArray &data);
quint32 crc32Ethernet(const QByteArray &data);

QByteArray makeFrame(Command command, quint16 sequence, const QByteArray &payload = {});
QByteArray makeBeginPayload(quint8 slaveId,
                            quint8 targetSlot,
                            quint32 imageSize,
                            quint32 imageCrc32,
                            quint32 version);
QByteArray makeDataPayload(quint32 offset, const QByteArray &data);

std::optional<Frame> parseFrame(const QByteArray &data);
std::optional<ResponsePayload> parseResponsePayload(const QByteArray &payload);

bool isResponseFor(const Frame &frame, Command command);
QString commandName(quint8 command);
QString statusText(Status status);
QString stateText(SlaveState state);
QString resultText(SlaveResult result);
QString responseSummary(const ResponsePayload &payload);

} // namespace UpperUpgrade
