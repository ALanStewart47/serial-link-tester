#pragma once

#include "CommandTemplate.h"

#include <QByteArray>
#include <QVariantMap>

struct FrameBuildResult
{
    bool ok = false;
    QByteArray bytes;
    QString errorMessage;
};

class FrameBuilder
{
public:
    static QVariantMap defaultParameters(const CommandTemplate &command);
    static FrameBuildResult build(const CommandTemplate &command, const QVariantMap &parameters);
    static QString hexText(const QByteArray &bytes);
    static QString escapedAscii(const QByteArray &bytes);

private:
    static bool appendParameter(QByteArray *output,
                                const QJsonObject &definition,
                                const QString &encoding,
                                int segmentWidth,
                                const QVariant &value,
                                QString *errorMessage);
};
