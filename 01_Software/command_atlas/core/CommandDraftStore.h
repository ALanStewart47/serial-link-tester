#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

class CommandCatalog;

class CommandDraftStore
{
public:
    bool load(QVector<QJsonObject> *drafts, QString *errorMessage) const;
    bool save(const QVector<QJsonObject> &drafts, QString *errorMessage) const;
    // Prepares an append-only batch. Does not change existing drafts or write files.
    bool readImport(const QString &path, const CommandCatalog &catalog,
                    const QVector<QJsonObject> &existing, QVector<QJsonObject> *imported,
                    int *renamedIds, QString *errorMessage) const;
    QString filePath() const;
};
