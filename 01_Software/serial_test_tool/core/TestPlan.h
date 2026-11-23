#pragma once

#include "core/CommandItem.h"
#include <QList>
#include <QStringList>

namespace TestPlan {
constexpr int MaxSteps = 4096;
constexpr int MaxFileBytes = 4 * 1024 * 1024;
constexpr quint64 MaxSends = 10000000;

struct Parameters
{
    QString protocol = QStringLiteral("new");
    QString model = QStringLiteral("自定义设备");
    int channelCount = 8;
    int brightnessMax = 255;
    QString pulseUnit = QStringLiteral("原始值（单位未确认）");
    QString function = QStringLiteral("brightness");
    QString channels = QStringLiteral("1-8");
    int value = 255;
    bool readback = true;
};

struct Document
{
    QString name = QStringLiteral("多通道测试计划");
    QList<CommandItem> steps;
    QJsonObject settings;
};

QString protocolName(const QString &protocol);
int channelLimit(const QString &protocol);
QStringList functions(const QString &protocol = QStringLiteral("new"));
QString functionName(const QString &id);
int maximum(const Parameters &p);
bool parseChannels(const QString &text, int maximum, QList<int> *channels, QString *error);
bool generate(const Parameters &p, QList<CommandItem> *steps, QString *error);
bool validate(const QList<CommandItem> &steps, QString *error);
QByteArray serialize(const Document &plan);
bool deserialize(const QByteArray &json, Document *plan, QString *error);
bool save(const QString &path, const Document &plan, QString *error);
bool load(const QString &path, Document *plan, QString *error);
bool totalSends(int stepCount, quint64 repetitions, quint64 *total, QString *error);
} // namespace TestPlan
