#pragma once

#include "CommandTemplate.h"

#include <QHash>
#include <QJsonObject>
#include <QVector>

class CommandCatalog
{
public:
    bool load(QString *errorMessage);

    const QVector<ProductProfile> &products() const;
    const QVector<CommandTemplate> &commands() const;
    QVector<const CommandTemplate *> search(const QString &query,
                                            const QString &productId,
                                            const QString &protocolId) const;
    const ProductProfile *productById(const QString &id) const;
    bool validateTemplate(const QJsonObject &object,
                          CommandTemplate *command,
                          QString *errorMessage) const;

private:
    bool validateCommand(const QJsonObject &object,
                         const QHash<QString, ProductProfile> &productsById,
                         CommandTemplate *command,
                         QString *errorMessage) const;
    QStringList searchTerms(const QString &text) const;

    QVector<ProductProfile> m_products;
    QVector<CommandTemplate> m_commands;
    QHash<QString, QStringList> m_synonyms;
};
