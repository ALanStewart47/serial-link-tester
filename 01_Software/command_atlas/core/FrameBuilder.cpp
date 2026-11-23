#include "FrameBuilder.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <utility>

namespace {

bool fail(QString *message, const QString &reason)
{
    if (message)
        *message = reason;
    return false;
}

bool parseInteger(const QVariant &value, qlonglong *number)
{
    bool ok = false;
    const qlonglong parsed = value.toLongLong(&ok);
    if (!ok)
        return false;
    *number = parsed;
    return true;
}

bool appendHex(QByteArray *output, const QString &text)
{
    QString normalized = text;
    normalized.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (normalized.isEmpty() || (normalized.size() % 2) != 0
        || !QRegularExpression(QStringLiteral("^[0-9A-Fa-f]+$")).match(normalized).hasMatch()) {
        return false;
    }
    *output += QByteArray::fromHex(normalized.toLatin1());
    return true;
}

bool isEnumValue(const QJsonObject &definition, const QVariant &candidate)
{
    const QJsonArray options = definition.value(QStringLiteral("options")).toArray();
    for (const QJsonValue &optionValue : options) {
        const QJsonObject option = optionValue.toObject();
        if (option.value(QStringLiteral("value")).toVariant() == candidate)
            return true;
    }
    return false;
}

} // namespace

QVariantMap FrameBuilder::defaultParameters(const CommandTemplate &command)
{
    QVariantMap values;
    for (const QJsonValue &value : command.parameters) {
        const QJsonObject definition = value.toObject();
        values.insert(definition.value(QStringLiteral("id")).toString(),
                      definition.value(QStringLiteral("default")).toVariant());
    }
    return values;
}

FrameBuildResult FrameBuilder::build(const CommandTemplate &command, const QVariantMap &parameters)
{
    FrameBuildResult result;
    QByteArray bytes;

    for (const QJsonValue &segmentValue : command.segments) {
        const QJsonObject segment = segmentValue.toObject();
        const QString kind = segment.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("fixed")) {
            const QString value = segment.value(QStringLiteral("value")).toString();
            const QString encoding = segment.value(QStringLiteral("encoding")).toString();
            if (encoding == QStringLiteral("hex")) {
                if (!appendHex(&bytes, value)) {
                    result.errorMessage = QStringLiteral("固定 HEX 帧段格式无效");
                    return result;
                }
            } else if (encoding == QStringLiteral("ascii")) {
                const QByteArray encoded = value.toLatin1();
                if (QString::fromLatin1(encoded) != value) {
                    result.errorMessage = QStringLiteral("固定 ASCII 帧段包含非 ASCII 字符");
                    return result;
                }
                bytes += encoded;
            } else {
                result.errorMessage = QStringLiteral("固定帧段编码不受支持：%1").arg(encoding);
                return result;
            }
        } else if (kind == QStringLiteral("parameter")) {
            const QString parameterId = segment.value(QStringLiteral("parameter")).toString();
            const QString encoding = segment.value(QStringLiteral("encoding")).toString();
            QJsonObject definition;
            for (const QJsonValue &parameterValue : command.parameters) {
                const QJsonObject candidate = parameterValue.toObject();
                if (candidate.value(QStringLiteral("id")).toString() == parameterId) {
                    definition = candidate;
                    break;
                }
            }
            if (definition.isEmpty()) {
                result.errorMessage = QStringLiteral("帧段引用了未知参数：%1").arg(parameterId);
                return result;
            }
            const QVariant value = parameters.contains(parameterId)
                ? parameters.value(parameterId) : definition.value(QStringLiteral("default")).toVariant();
            QString errorMessage;
            const bool appended = appendParameter(&bytes, definition, encoding,
                                                  segment.value(QStringLiteral("width")).toInt(),
                                                  value, &errorMessage);
            if (!appended) {
                result.errorMessage = errorMessage;
                return result;
            }
        } else if (kind == QStringLiteral("computed")) {
            const QString algorithm = segment.value(QStringLiteral("algorithm")).toString();
            if (algorithm != QStringLiteral("xor_bcc")) {
                result.errorMessage = QStringLiteral("计算字段算法暂不支持：%1").arg(algorithm);
                return result;
            }
            quint8 checksum = 0;
            for (const char byte : std::as_const(bytes))
                checksum ^= static_cast<quint8>(static_cast<unsigned char>(byte));
            bytes.append(static_cast<char>(checksum));
        } else {
            result.errorMessage = QStringLiteral("未知帧段类型：%1").arg(kind);
            return result;
        }
    }

    result.ok = true;
    result.bytes = bytes;
    return result;
}

bool FrameBuilder::appendParameter(QByteArray *output,
                                   const QJsonObject &definition,
                                   const QString &encoding,
                                   int segmentWidth,
                                   const QVariant &value,
                                   QString *errorMessage)
{
    const QString label = definition.value(QStringLiteral("label")).toString();
    const QString type = definition.value(QStringLiteral("type")).toString();

    if (type == QStringLiteral("enum") && !isEnumValue(definition, value))
        return fail(errorMessage, QStringLiteral("“%1”不是有效选项").arg(label));

    if (encoding == QStringLiteral("u8") || encoding == QStringLiteral("u16be")
        || encoding == QStringLiteral("ascii_channel") || encoding == QStringLiteral("ascii_decimal")) {
        qlonglong number = 0;
        if (!parseInteger(value, &number))
            return fail(errorMessage, QStringLiteral("“%1”必须是整数").arg(label));
        const qlonglong minimum = static_cast<qlonglong>(definition.value(QStringLiteral("min")).toDouble());
        const qlonglong maximum = static_cast<qlonglong>(definition.value(QStringLiteral("max")).toDouble());
        if (number < minimum || number > maximum)
            return fail(errorMessage, QStringLiteral("“%1”超出允许范围 %2~%3").arg(label).arg(minimum).arg(maximum));

        if (encoding == QStringLiteral("u8")) {
            if (number < 0 || number > 255)
                return fail(errorMessage, QStringLiteral("“%1”无法编码为 1 字节").arg(label));
            output->append(static_cast<char>(number));
        } else if (encoding == QStringLiteral("u16be")) {
            if (number < 0 || number > 65535)
                return fail(errorMessage, QStringLiteral("“%1”无法编码为 2 字节").arg(label));
            output->append(static_cast<char>((number >> 8) & 0xFF));
            output->append(static_cast<char>(number & 0xFF));
        } else if (encoding == QStringLiteral("ascii_channel")) {
            if (number < 1 || number > 26)
                return fail(errorMessage, QStringLiteral("“%1”无法映射为 A~Z 通道").arg(label));
            output->append(static_cast<char>('A' + number - 1));
        } else {
            const int width = segmentWidth > 0 ? segmentWidth : definition.value(QStringLiteral("width")).toInt();
            const QByteArray digits = QByteArray::number(number);
            if (width <= 0 || digits.size() > width)
                return fail(errorMessage, QStringLiteral("“%1”无法放入 %2 位 ASCII 数字").arg(label).arg(width));
            output->append(QByteArray(width - digits.size(), '0'));
            output->append(digits);
        }
        return true;
    }

    if (encoding == QStringLiteral("ascii_fixed")) {
        const QString text = value.toString();
        const QByteArray bytes = text.toLatin1();
        const int width = segmentWidth > 0 ? segmentWidth : definition.value(QStringLiteral("width")).toInt();
        bool isAscii = true;
        for (const QChar character : text) {
            if (character.unicode() > 0x7F) {
                isAscii = false;
                break;
            }
        }
        if (!isAscii || width <= 0 || bytes.size() != width)
            return fail(errorMessage, QStringLiteral("“%1”必须是 %2 字节的定长 ASCII 文本").arg(label).arg(width));
        output->append(bytes);
        return true;
    }

    return fail(errorMessage, QStringLiteral("“%1”的参数编码暂不支持：%2").arg(label, encoding));
}

QString FrameBuilder::hexText(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

QString FrameBuilder::escapedAscii(const QByteArray &bytes)
{
    QString output;
    for (const char byte : bytes) {
        const auto value = static_cast<unsigned char>(byte);
        if (value == '\\') {
            output += QStringLiteral("\\\\");
        } else if (value == '\r') {
            output += QStringLiteral("\\r");
        } else if (value == '\n') {
            output += QStringLiteral("\\n");
        } else if (value == '\t') {
            output += QStringLiteral("\\t");
        } else if (value >= 0x20 && value <= 0x7E) {
            output += QChar(static_cast<ushort>(value));
        } else {
            output += QStringLiteral("\\x%1").arg(value, 2, 16, QLatin1Char('0')).toUpper();
        }
    }
    return output;
}
