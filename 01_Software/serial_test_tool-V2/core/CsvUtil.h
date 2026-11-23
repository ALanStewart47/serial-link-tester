#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// CSV 字段转义 / 解析（RFC 4180）：含逗号/引号/换行时用双引号包裹，内部引号翻倍。
namespace CsvUtil {

inline QString escape(const QString &s)
{
    if (s.contains(QLatin1Char(',')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))
        || s.contains(QLatin1Char('\r'))) {
        QString v = s;
        v.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + v + QLatin1Char('"');
    }
    return s;
}

inline QVector<QStringList> parseRecords(const QString &text)
{
    QVector<QStringList> rows;
    QStringList row;
    QString field;
    bool inQuotes = false;
    const int n = text.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < n && text.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            row.append(field);
            field.clear();
        } else if (c == QLatin1Char('\n') || c == QLatin1Char('\r')) {
            if (c == QLatin1Char('\r') && i + 1 < n && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            row.append(field);
            field.clear();
            bool empty = true;
            for (const QString &cell : row) {
                if (!cell.isEmpty()) {
                    empty = false;
                    break;
                }
            }
            if (!empty) {
                rows.append(row);
            }
            row.clear();
        } else {
            field += c;
        }
    }
    if (inQuotes || !field.isEmpty() || !row.isEmpty()) {
        row.append(field);
        bool empty = true;
        for (const QString &cell : row) {
            if (!cell.isEmpty()) {
                empty = false;
                break;
            }
        }
        if (!empty) {
            rows.append(row);
        }
    }
    return rows;
}

} // namespace CsvUtil
