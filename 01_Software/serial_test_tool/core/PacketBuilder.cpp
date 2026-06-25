#include "core/PacketBuilder.h"

namespace PacketBuilder {

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
        const ushort u = c.unicode();
        const bool isHex = (u >= '0' && u <= '9')
                           || (u >= 'A' && u <= 'F')
                           || (u >= 'a' && u <= 'f');
        if (!isHex) {
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
        bool byteOk = false;
        const int value = hex.mid(i, 2).toInt(&byteOk, 16);
        if (!byteOk) {
            if (ok != nullptr) {
                *ok = false;
            }
            return {};
        }
        out.append(static_cast<char>(value));
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
        case 'r': out.append('\r'); break;
        case 'n': out.append('\n'); break;
        case 't': out.append('\t'); break;
        case '0': out.append('\0'); break;
        case '\\': out.append('\\'); break;
        case 'x': case 'X': {
            if (i + 2 >= text.size()) {
                if (ok) *ok = false;
                return {};
            }
            bool byteOk = false;
            const int v = text.mid(i + 1, 2).toInt(&byteOk, 16);
            if (!byteOk) {
                if (ok) *ok = false;
                return {};
            }
            out.append(static_cast<char>(v));
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
                       .rightJustified(2, QLatin1Char('0')).toUpper());
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
