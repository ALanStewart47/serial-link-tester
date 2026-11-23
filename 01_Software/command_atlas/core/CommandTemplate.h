#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>

struct ProductProfile
{
    QString id;
    QString name;
    QString description;
    QStringList supportedProtocols;
};

struct CommandTemplate
{
    QString id;
    QStringList productIds;
    QString protocolId;
    QString category;
    QString title;
    QString description;
    QString frameFormat;
    QStringList aliases;
    QStringList keywords;
    QJsonArray parameters;
    QJsonArray segments;
};
