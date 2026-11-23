#include "core/PacketBuilder.h"

namespace PacketBuilder {
namespace {
int hexDigit(QChar character)
{
    const ushort value = character.unicode();
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    return -1;
}
} // namespace

QByteArray fromHexText(const QString &text, bool *ok)
{
    // 收集所有十六进制字符，忽略空格/逗号/制表/换行等分隔符。
    QString hex;
    hex.reserve(text.size());
    for (const QChar &c : text) {
        if (c.isSpace() || c == QLatin1Char(',')) {
            continue;
        }
        // 只接受 ASCII 十六进制字符。不要把 QChar 转成可能为负的 char 再交给 isxdigit()
        // （非 ASCII 字符会触发未定义行为）。
        if (hexDigit(c) < 0) {
            if (ok != nullptr) {
                *ok = false;
            }
            return {};
        }
        hex.append(c);
    }

    if (hex.size() % 2 != 0) {
        // 半个字节，非法
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }

    QByteArray out;
    out.reserve(hex.size() / 2);
    for (int i = 0; i < hex.size(); i += 2) {
        out.append(static_cast<char>(hexDigit(hex.at(i)) * 16 + hexDigit(hex.at(i + 1))));
    }

    if (ok != nullptr) {
        *ok = true;
    }
    return out;
}

QByteArray fromAsciiText(const QString &text)
{
    return text.toLatin1();
}

QByteArray fromAsciiEscaped(const QString &text, bool *ok)
{
    QByteArray out;
    out.reserve(text.size());
    if (ok) {
        *ok = true;
    }
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('\\')) {
            out.append(static_cast<char>(c.toLatin1()));
            continue;
        }
        // 反斜杠转义
        if (i + 1 >= text.size()) {
            out.append('\\'); // 末尾孤立反斜杠，按字面处理
            break;
        }
        const QChar n = text.at(++i);
        switch (n.toLatin1()) {
        case 'r':
            out.append('\r');
            break;
        case 'n':
            out.append('\n');
            break;
        case 't':
            out.append('\t');
            break;
        case '0':
            out.append('\0');
            break;
        case '\\':
            out.append('\\');
            break;
        case 'x':
        case 'X': {
            if (i + 2 >= text.size()) {
                if (ok)
                    *ok = false;
                return {};
            }
            const int high = hexDigit(text.at(i + 1));
            const int low = hexDigit(text.at(i + 2));
            if (high < 0 || low < 0) {
                if (ok)
                    *ok = false;
                return {};
            }
            out.append(static_cast<char>(high * 16 + low));
            i += 2;
            break;
        }
        default:
            // 未知转义：保留反斜杠和该字符的字面（宽容处理）
            out.append('\\');
            out.append(static_cast<char>(n.toLatin1()));
            break;
        }
    }
    return out;
}

QByteArray decodeCommandText(const QString &format, const QString &text, bool *ok)
{
    if (format == QStringLiteral("hex"))
        return fromHexText(text, ok);
    if (format == QStringLiteral("ascii_escaped"))
        return fromAsciiEscaped(text, ok);

    const bool valid = format == QStringLiteral("ascii");
    if (ok)
        *ok = valid;
    return valid ? fromAsciiText(text) : QByteArray{};
}

quint8 bccXor(const QByteArray &data)
{
    quint8 bcc = 0;
    for (const char byte : data) {
        bcc ^= static_cast<quint8>(byte);
    }
    return bcc;
}

QByteArray appendBcc(const QByteArray &data)
{
    QByteArray out = data;
    out.append(static_cast<char>(bccXor(data)));
    return out;
}

QString toHexText(const QByteArray &data)
{
    QString out;
    out.reserve(data.size() * 3);
    for (int i = 0; i < data.size(); ++i) {
        if (i > 0) {
            out.append(QLatin1Char(' '));
        }
        out.append(QString::number(static_cast<quint8>(data.at(i)), 16)
                       .rightJustified(2, QLatin1Char('0'))
                       .toUpper());
    }
    return out;
}

QString toAsciiText(const QByteArray &data)
{
    QString out;
    out.reserve(data.size());
    for (const char byte : data) {
        const auto u = static_cast<quint8>(byte);
        out.append((u >= 0x20 && u < 0x7F) ? QChar(u) : QLatin1Char('.'));
    }
    return out;
}

} // namespace PacketBuilder
