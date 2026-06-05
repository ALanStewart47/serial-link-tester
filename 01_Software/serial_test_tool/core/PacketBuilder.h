#pragma once

#include <QByteArray>
#include <QString>

// 纯静态工具：HEX/ASCII 文本 <-> 字节，以及 BCC(异或) 计算。
// 无 Qt 对象、无状态，便于复用与单测。
namespace PacketBuilder {

// 解析 HEX 文本为字节。容错空格/逗号分隔与大小写，例如 "CA 01 0A" / "ca010a" / "CA,01,0A"。
// 解析成功 ok=true；遇到非法字符或半个字节时 ok=false 并返回空数组。
QByteArray fromHexText(const QString &text, bool *ok = nullptr);

// 把 ASCII 文本按 Latin1 字节发送（串口协议通常是单字节字符）。
QByteArray fromAsciiText(const QString &text);

// 逐字节异或校验。
quint8 bccXor(const QByteArray &data);

// 在末尾追加 1 字节 BCC(异或) 后返回新数组。
QByteArray appendBcc(const QByteArray &data);

// 字节 -> "CA 01 0A" 形式的大写 HEX 文本（空格分隔）。
QString toHexText(const QByteArray &data);

// 字节 -> 可读 ASCII 文本，不可打印字符以 '.' 占位。
QString toAsciiText(const QByteArray &data);

} // namespace PacketBuilder
