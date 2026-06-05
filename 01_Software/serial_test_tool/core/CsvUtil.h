#pragma once

#include <QString>

// CSV 字段转义（RFC 4180）：含逗号/引号/换行时用双引号包裹，内部引号翻倍。
namespace CsvUtil {

inline QString escape(const QString &s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"'))
        || s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\r'))) {
        QString v = s;
        v.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + v + QLatin1Char('"');
    }
    return s;
}

} // namespace CsvUtil
