#include "CommandCatalog.h"
#include "FrameBuilder.h"

#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QtMath>
#include <utility>

namespace {

constexpr int CatalogSchemaVersion = 1;

bool fail(QString *message, const QString &reason)
{
    if (message)
        *message = reason;
    return false;
}

bool readObject(const QString &resourcePath, QJsonObject *object, QString *errorMessage)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(errorMessage, QStringLiteral("无法读取内置数据：%1").arg(resourcePath));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(errorMessage,
                    QStringLiteral("内置数据 JSON 格式错误：%1（%2）")
                        .arg(resourcePath, parseError.errorString()));
    *object = document.object();
    return true;
}

bool hasStringArray(const QJsonValue &value)
{
    if (!value.isArray())
        return false;
    for (const QJsonValue &entry : value.toArray()) {
        if (!entry.isString() || entry.toString().trimmed().isEmpty())
            return false;
    }
    return true;
}

QStringList toStringList(const QJsonArray &array)
{
    QStringList result;
    result.reserve(array.size());
    for (const QJsonValue &value : array)
        result.append(value.toString());
    return result;
}

bool isKnownEncoding(const QString &encoding)
{
    return encoding == QStringLiteral("u8") || encoding == QStringLiteral("u16be")
        || encoding == QStringLiteral("ascii_channel")
        || encoding == QStringLiteral("ascii_decimal")
        || encoding == QStringLiteral("ascii_fixed");
}

bool isAscii(const QString &text)
{
    for (const QChar character : text) {
        if (character.unicode() > 0x7F)
            return false;
    }
    return true;
}

bool isIntegralNumber(const QJsonValue &value)
{
    return value.isDouble() && qIsFinite(value.toDouble())
        && value.toDouble() == qFloor(value.toDouble());
}

bool isNumericEncoding(const QString &encoding)
{
    return encoding == QStringLiteral("u8") || encoding == QStringLiteral("u16be")
        || encoding == QStringLiteral("ascii_channel") || encoding == QStringLiteral("ascii_decimal");
}

QString enumValueKey(const QJsonValue &value)
{
    if (value.isString())
        return QStringLiteral("s:") + value.toString();
    if (value.isDouble())
        return QStringLiteral("n:") + QString::number(value.toDouble(), 'g', 17);
    return {};
}

bool validWidth(const QJsonValue &value)
{
    return isIntegralNumber(value) && value.toInt() > 0 && value.toInt() <= 4096;
}

} // namespace

bool CommandCatalog::load(QString *errorMessage)
{
    m_products.clear();
    m_commands.clear();
    m_synonyms.clear();

    QJsonObject manifest;
    if (!readObject(QStringLiteral(":/catalog/manifest.json"), &manifest, errorMessage))
        return false;
    if (manifest.value(QStringLiteral("format")).toString() != QStringLiteral("command-atlas-catalog")
        || manifest.value(QStringLiteral("schema_version")).toInt() != CatalogSchemaVersion
        || manifest.value(QStringLiteral("products_file")).toString() != QStringLiteral("products.json")
        || manifest.value(QStringLiteral("commands_file")).toString() != QStringLiteral("commands.json")
        || manifest.value(QStringLiteral("synonyms_file")).toString() != QStringLiteral("synonyms.json")) {
        return fail(errorMessage, QStringLiteral("指令库清单格式或版本不受支持"));
    }

    QJsonObject productsDocument;
    QJsonObject commandsDocument;
    QJsonObject synonymsDocument;
    if (!readObject(QStringLiteral(":/catalog/products.json"), &productsDocument, errorMessage)
        || !readObject(QStringLiteral(":/catalog/commands.json"), &commandsDocument, errorMessage)
        || !readObject(QStringLiteral(":/catalog/synonyms.json"), &synonymsDocument, errorMessage)) {
        return false;
    }

    const auto validDocument = [](const QJsonObject &object, const QString &format) {
        return object.value(QStringLiteral("format")).toString() == format
            && object.value(QStringLiteral("schema_version")).toInt() == CatalogSchemaVersion;
    };
    if (!validDocument(productsDocument, QStringLiteral("command-atlas-products"))
        || !validDocument(commandsDocument, QStringLiteral("command-atlas-commands"))
        || !validDocument(synonymsDocument, QStringLiteral("command-atlas-synonyms"))) {
        return fail(errorMessage, QStringLiteral("产品、指令或同义词数据的格式版本不受支持"));
    }

    const QJsonValue productValues = productsDocument.value(QStringLiteral("products"));
    if (!productValues.isArray() || productValues.toArray().isEmpty())
        return fail(errorMessage, QStringLiteral("产品档案不能为空"));

    QHash<QString, ProductProfile> productsById;
    for (const QJsonValue &value : productValues.toArray()) {
        if (!value.isObject())
            return fail(errorMessage, QStringLiteral("产品档案必须是对象"));
        const QJsonObject object = value.toObject();
        ProductProfile product;
        product.id = object.value(QStringLiteral("id")).toString().trimmed();
        product.name = object.value(QStringLiteral("name")).toString().trimmed();
        product.description = object.value(QStringLiteral("description")).toString().trimmed();
        const QJsonValue protocols = object.value(QStringLiteral("supported_protocols"));
        if (product.id.isEmpty() || product.name.isEmpty() || !hasStringArray(protocols)
            || protocols.toArray().isEmpty())
            return fail(errorMessage, QStringLiteral("产品档案缺少 ID、名称或有效协议列表"));
        product.supportedProtocols = toStringList(protocols.toArray());
        for (const QString &protocol : product.supportedProtocols) {
            if (protocol != QStringLiteral("new") && protocol != QStringLiteral("old")
                && protocol != QStringLiteral("old_ext")) {
                return fail(errorMessage, QStringLiteral("产品“%1”包含未知协议“%2”").arg(product.name, protocol));
            }
        }
        if (productsById.contains(product.id))
            return fail(errorMessage, QStringLiteral("产品 ID 重复：%1").arg(product.id));
        productsById.insert(product.id, product);
        m_products.append(product);
    }

    const QJsonValue commandValues = commandsDocument.value(QStringLiteral("commands"));
    if (!commandValues.isArray() || commandValues.toArray().isEmpty())
        return fail(errorMessage, QStringLiteral("指令模板不能为空"));
    QSet<QString> commandIds;
    for (const QJsonValue &value : commandValues.toArray()) {
        if (!value.isObject())
            return fail(errorMessage, QStringLiteral("指令模板必须是对象"));
        CommandTemplate command;
        if (!validateCommand(value.toObject(), productsById, &command, errorMessage))
            return false;
        if (commandIds.contains(command.id))
            return fail(errorMessage, QStringLiteral("指令 ID 重复：%1").arg(command.id));
        const FrameBuildResult defaultFrame = FrameBuilder::build(command,
                                                                   FrameBuilder::defaultParameters(command));
        if (!defaultFrame.ok)
            return fail(errorMessage, QStringLiteral("指令“%1”的默认帧无法生成：%2")
                                          .arg(command.title, defaultFrame.errorMessage));
        commandIds.insert(command.id);
        m_commands.append(command);
    }

    const QJsonValue synonymValues = synonymsDocument.value(QStringLiteral("synonyms"));
    if (!synonymValues.isArray())
        return fail(errorMessage, QStringLiteral("同义词表必须是数组"));
    for (const QJsonValue &value : synonymValues.toArray()) {
        if (!value.isObject())
            return fail(errorMessage, QStringLiteral("同义词条目必须是对象"));
        const QJsonObject object = value.toObject();
        const QString term = object.value(QStringLiteral("term")).toString().trimmed();
        const QJsonValue synonyms = object.value(QStringLiteral("values"));
        if (term.isEmpty() || !hasStringArray(synonyms))
            return fail(errorMessage, QStringLiteral("同义词条目缺少主词或有效同义词"));
        QStringList related = toStringList(synonyms.toArray());
        related.append(term);
        related.removeDuplicates();
        for (const QString &word : related)
            m_synonyms.insert(word.toCaseFolded(), related);
    }

    if (errorMessage)
        errorMessage->clear();
    return true;
}

bool CommandCatalog::validateCommand(const QJsonObject &object,
                                     const QHash<QString, ProductProfile> &productsById,
                                     CommandTemplate *command,
                                     QString *errorMessage) const
{
    command->id = object.value(QStringLiteral("id")).toString().trimmed();
    command->title = object.value(QStringLiteral("title")).toString().trimmed();
    command->description = object.value(QStringLiteral("description")).toString().trimmed();
    command->protocolId = object.value(QStringLiteral("protocol")).toString();
    command->category = object.value(QStringLiteral("category")).toString();
    command->frameFormat = object.value(QStringLiteral("frame_format")).toString();
    const QJsonValue aliases = object.value(QStringLiteral("aliases"));
    const QJsonValue keywords = object.value(QStringLiteral("keywords"));
    const QJsonValue parameterValues = object.value(QStringLiteral("parameters"));
    const QJsonValue segmentValues = object.value(QStringLiteral("segments"));
    command->aliases = toStringList(aliases.toArray());
    command->keywords = toStringList(keywords.toArray());
    command->parameters = parameterValues.toArray();
    command->segments = segmentValues.toArray();

    const QJsonValue productValues = object.value(QStringLiteral("product_ids"));
    if (command->id.isEmpty() || command->title.isEmpty() || command->description.isEmpty()
        || !hasStringArray(productValues) || productValues.toArray().isEmpty()
        || !aliases.isArray() || !hasStringArray(aliases)
        || !keywords.isArray() || !hasStringArray(keywords) || !parameterValues.isArray()
        || !segmentValues.isArray()
        || (command->protocolId != QStringLiteral("new") && command->protocolId != QStringLiteral("old")
            && command->protocolId != QStringLiteral("old_ext"))
        || (command->frameFormat != QStringLiteral("hex") && command->frameFormat != QStringLiteral("ascii"))
        || command->segments.isEmpty()) {
        return fail(errorMessage, QStringLiteral("指令模板缺少必填字段或包含未知协议/帧格式：%1").arg(command->id));
    }

    command->productIds = toStringList(productValues.toArray());
    for (const QString &productId : command->productIds) {
        const auto product = productsById.constFind(productId);
        if (product == productsById.cend())
            return fail(errorMessage, QStringLiteral("指令“%1”引用了不存在的产品 %2").arg(command->title, productId));
        if (!product->supportedProtocols.contains(command->protocolId))
            return fail(errorMessage, QStringLiteral("指令“%1”的协议不在产品“%2”的支持列表中")
                            .arg(command->title, product->name));
    }
    if ((command->protocolId == QStringLiteral("new")) != (command->frameFormat == QStringLiteral("hex")))
        return fail(errorMessage, QStringLiteral("指令“%1”的协议类型与帧格式不匹配").arg(command->title));

    QHash<QString, QJsonObject> parameterById;
    for (const QJsonValue &value : command->parameters) {
        if (!value.isObject())
            return fail(errorMessage, QStringLiteral("指令“%1”包含无效参数").arg(command->title));
        const QJsonObject parameter = value.toObject();
        const QString id = parameter.value(QStringLiteral("id")).toString().trimmed();
        const QString type = parameter.value(QStringLiteral("type")).toString();
        const QString encoding = parameter.value(QStringLiteral("encoding")).toString();
        const bool integerType = type == QStringLiteral("integer");
        const bool enumType = type == QStringLiteral("enum");
        const bool fixedAsciiType = type == QStringLiteral("fixed_ascii");
        const bool asciiNumberType = type == QStringLiteral("ascii_number");
        const bool supportedType = integerType || enumType || fixedAsciiType || asciiNumberType;
        const bool encodingMatchesType = (integerType && isNumericEncoding(encoding))
            || (enumType && (isNumericEncoding(encoding) || encoding == QStringLiteral("ascii_fixed")))
            || (fixedAsciiType && encoding == QStringLiteral("ascii_fixed"))
            || (asciiNumberType && encoding == QStringLiteral("ascii_decimal"));
        if (id.isEmpty() || parameter.value(QStringLiteral("label")).toString().trimmed().isEmpty()
            || !supportedType || !isKnownEncoding(encoding) || !encodingMatchesType
            || parameterById.contains(id)) {
            return fail(errorMessage, QStringLiteral("指令“%1”包含重复或不受支持的参数定义").arg(command->title));
        }

        if (isNumericEncoding(encoding)) {
            const QJsonValue minimum = parameter.value(QStringLiteral("min"));
            const QJsonValue maximum = parameter.value(QStringLiteral("max"));
            const QJsonValue defaultValue = parameter.value(QStringLiteral("default"));
            if (!isIntegralNumber(minimum) || !isIntegralNumber(maximum) || !isIntegralNumber(defaultValue)
                || minimum.toDouble() > defaultValue.toDouble()
                || defaultValue.toDouble() > maximum.toDouble()
                || minimum.toDouble() > maximum.toDouble()) {
                return fail(errorMessage, QStringLiteral("指令“%1”的数值参数范围或默认值无效").arg(command->title));
            }
            if ((encoding == QStringLiteral("u8")
                 && (minimum.toDouble() < 0 || maximum.toDouble() > 255))
                || (encoding == QStringLiteral("u16be")
                    && (minimum.toDouble() < 0 || maximum.toDouble() > 65535))
                || (encoding == QStringLiteral("ascii_channel")
                    && (minimum.toDouble() < 1 || maximum.toDouble() > 26))
                || (encoding == QStringLiteral("ascii_decimal") && minimum.toDouble() < 0)) {
                return fail(errorMessage, QStringLiteral("指令“%1”的数值范围超出编码能力").arg(command->title));
            }
            if (encoding == QStringLiteral("ascii_decimal")
                && (!validWidth(parameter.value(QStringLiteral("width")))
                    || parameter.value(QStringLiteral("width")).toInt() > 15
                    || maximum.toDouble() > 9007199254740991.0
                    || QString::number(maximum.toDouble(), 'f', 0).size()
                        > parameter.value(QStringLiteral("width")).toInt())) {
                return fail(errorMessage, QStringLiteral("指令“%1”的 ASCII 数字位宽不足或无效").arg(command->title));
            }
        }

        if (encoding == QStringLiteral("ascii_fixed")) {
            const QJsonValue width = parameter.value(QStringLiteral("width"));
            const QJsonValue defaultValue = parameter.value(QStringLiteral("default"));
            if (!validWidth(width) || !defaultValue.isString() || !isAscii(defaultValue.toString())
                || defaultValue.toString().size() != width.toInt()) {
                return fail(errorMessage, QStringLiteral("指令“%1”的定长 ASCII 默认值或位宽无效").arg(command->title));
            }
        }

        if (enumType) {
            const QJsonValue optionsValue = parameter.value(QStringLiteral("options"));
            if (!optionsValue.isArray() || optionsValue.toArray().isEmpty())
                return fail(errorMessage, QStringLiteral("指令“%1”的枚举参数缺少有效选项").arg(command->title));
            QSet<QString> optionKeys;
            bool defaultFound = false;
            for (const QJsonValue &optionValue : optionsValue.toArray()) {
                if (!optionValue.isObject())
                    return fail(errorMessage, QStringLiteral("指令“%1”包含无效枚举选项").arg(command->title));
                const QJsonObject option = optionValue.toObject();
                const QJsonValue optionCode = option.value(QStringLiteral("value"));
                const QString key = enumValueKey(optionCode);
                if (option.value(QStringLiteral("label")).toString().trimmed().isEmpty() || key.isEmpty()
                    || optionKeys.contains(key)) {
                    return fail(errorMessage, QStringLiteral("指令“%1”包含空白或重复枚举值").arg(command->title));
                }
                if (isNumericEncoding(encoding)) {
                    if (!optionCode.isDouble() || !isIntegralNumber(optionCode)
                        || optionCode.toDouble() < parameter.value(QStringLiteral("min")).toDouble()
                        || optionCode.toDouble() > parameter.value(QStringLiteral("max")).toDouble()) {
                        return fail(errorMessage, QStringLiteral("指令“%1”的枚举值与数值编码不匹配").arg(command->title));
                    }
                } else if (!optionCode.isString() || !isAscii(optionCode.toString())
                           || (encoding == QStringLiteral("ascii_fixed")
                               && optionCode.toString().size() != parameter.value(QStringLiteral("width")).toInt())) {
                    return fail(errorMessage, QStringLiteral("指令“%1”的枚举值与 ASCII 编码不匹配").arg(command->title));
                }
                optionKeys.insert(key);
                if (optionCode == parameter.value(QStringLiteral("default")))
                    defaultFound = true;
            }
            if (!defaultFound)
                return fail(errorMessage, QStringLiteral("指令“%1”的枚举默认值不在选项中").arg(command->title));
        }

        parameterById.insert(id, parameter);
    }

    int bccCount = 0;
    QSet<QString> referencedParameters;
    for (qsizetype index = 0; index < command->segments.size(); ++index) {
        const QJsonValue value = command->segments.at(index);
        if (!value.isObject())
            return fail(errorMessage, QStringLiteral("指令“%1”包含无效帧段").arg(command->title));
        const QJsonObject segment = value.toObject();
        const QString kind = segment.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("fixed")) {
            const QString encoding = segment.value(QStringLiteral("encoding")).toString();
            const QString fixedValue = segment.value(QStringLiteral("value")).toString();
            bool validValue = segment.value(QStringLiteral("value")).isString();
            if (encoding == QStringLiteral("hex")) {
                QString compact = fixedValue;
                compact.remove(QRegularExpression(QStringLiteral("\\s+")));
                validValue = validValue && !compact.isEmpty() && compact.size() % 2 == 0
                    && QRegularExpression(QStringLiteral("^[0-9A-Fa-f]+$")).match(compact).hasMatch();
            } else {
                validValue = validValue && isAscii(fixedValue);
            }
            if (encoding != (command->frameFormat == QStringLiteral("hex")
                                ? QStringLiteral("hex") : QStringLiteral("ascii")) || !validValue) {
                return fail(errorMessage, QStringLiteral("指令“%1”固定帧段的编码无效").arg(command->title));
            }
        } else if (kind == QStringLiteral("parameter")) {
            const QString parameterId = segment.value(QStringLiteral("parameter")).toString();
            const QString encoding = segment.value(QStringLiteral("encoding")).toString();
            if (!parameterById.contains(parameterId) || !isKnownEncoding(encoding)
                || parameterById.value(parameterId).value(QStringLiteral("encoding")).toString() != encoding) {
                return fail(errorMessage, QStringLiteral("指令“%1”帧段引用了未知参数或编码").arg(command->title));
            }
            const QJsonObject parameter = parameterById.value(parameterId);
            if (encoding == QStringLiteral("ascii_decimal") || encoding == QStringLiteral("ascii_fixed")) {
                const QJsonValue segmentWidth = segment.value(QStringLiteral("width"));
                if (!validWidth(segmentWidth)
                    || segmentWidth.toInt() != parameter.value(QStringLiteral("width")).toInt()) {
                    return fail(errorMessage, QStringLiteral("指令“%1”的参数帧段位宽与定义不一致").arg(command->title));
                }
            } else if (segment.contains(QStringLiteral("width"))) {
                return fail(errorMessage, QStringLiteral("指令“%1”的此类参数帧段不应设置位宽").arg(command->title));
            }
            referencedParameters.insert(parameterId);
        } else if (kind == QStringLiteral("computed")) {
            if (segment.value(QStringLiteral("algorithm")).toString() != QStringLiteral("xor_bcc")
                || command->frameFormat != QStringLiteral("hex") || index != command->segments.size() - 1) {
                return fail(errorMessage, QStringLiteral("指令“%1”包含不支持或位置错误的计算字段").arg(command->title));
            }
            ++bccCount;
        } else {
            return fail(errorMessage, QStringLiteral("指令“%1”包含未知帧段类型：%2").arg(command->title, kind));
        }
    }
    if (referencedParameters.size() != parameterById.size() || bccCount > 1
        || (command->frameFormat == QStringLiteral("hex") && bccCount != 1)) {
        return fail(errorMessage, QStringLiteral("指令“%1”的参数未全部使用或校验字段定义不正确").arg(command->title));
    }
    return true;
}

bool CommandCatalog::validateTemplate(const QJsonObject &object,
                                      CommandTemplate *command,
                                      QString *errorMessage) const
{
    if (m_products.isEmpty())
        return fail(errorMessage, QStringLiteral("产品档案尚未载入"));

    QHash<QString, ProductProfile> productsById;
    for (const ProductProfile &product : m_products)
        productsById.insert(product.id, product);

    if (!validateCommand(object, productsById, command, errorMessage))
        return false;
    const FrameBuildResult defaultFrame = FrameBuilder::build(*command,
                                                               FrameBuilder::defaultParameters(*command));
    if (!defaultFrame.ok)
        return fail(errorMessage, QStringLiteral("默认帧无法生成：%1").arg(defaultFrame.errorMessage));
    if (errorMessage)
        errorMessage->clear();
    return true;
}

const QVector<ProductProfile> &CommandCatalog::products() const
{
    return m_products;
}

const QVector<CommandTemplate> &CommandCatalog::commands() const
{
    return m_commands;
}

const ProductProfile *CommandCatalog::productById(const QString &id) const
{
    for (const ProductProfile &product : m_products) {
        if (product.id == id)
            return &product;
    }
    return nullptr;
}

QStringList CommandCatalog::searchTerms(const QString &text) const
{
    const QString normalized = text.trimmed().toCaseFolded();
    if (normalized.isEmpty())
        return {};

    QStringList vocabulary = m_synonyms.keys();
    std::sort(vocabulary.begin(), vocabulary.end(), [](const QString &left, const QString &right) {
        if (left.size() != right.size())
            return left.size() > right.size();
        return left < right;
    });

    QStringList terms;
    const QStringList tokens = normalized.split(QRegularExpression(QStringLiteral("[\\s,，、;；]+")), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        qsizetype offset = 0;
        while (offset < token.size()) {
            QString matched;
            for (const QString &word : vocabulary) {
                if (token.mid(offset).startsWith(word)) {
                    matched = word;
                    break;
                }
            }
            if (!matched.isEmpty()) {
                terms.append(matched);
                offset += matched.size();
                continue;
            }

            const QChar character = token.at(offset);
            if (character.isLetterOrNumber() && character.unicode() < 0x2E80) {
                QString latinToken;
                while (offset < token.size()) {
                    const QChar next = token.at(offset);
                    if (!next.isLetterOrNumber() || next.unicode() >= 0x2E80)
                        break;
                    latinToken += next;
                    ++offset;
                }
                terms.append(latinToken);
            } else {
                // Unrecognized Chinese filler words do not hide a known intent word later in the query.
                ++offset;
            }
        }
    }
    terms.removeDuplicates();
    if (terms.isEmpty())
        terms = tokens;
    return terms;
}

QVector<const CommandTemplate *> CommandCatalog::search(const QString &query,
                                                        const QString &productId,
                                                        const QString &protocolId) const
{
    QVector<const CommandTemplate *> matches;
    const QStringList queryTerms = searchTerms(query);
    const ProductProfile *product = productById(productId);

    for (const CommandTemplate &command : m_commands) {
        if (!productId.isEmpty() && !command.productIds.contains(productId))
            continue;
        if (!protocolId.isEmpty() && protocolId != QStringLiteral("all") && command.protocolId != protocolId)
            continue;

        QStringList fields {command.id, command.title, command.description, command.category,
                            command.protocolId};
        fields.append(command.aliases);
        fields.append(command.keywords);
        if (product)
            fields.append(product->name);
        for (const QJsonValue &parameterValue : command.parameters) {
            const QJsonObject parameter = parameterValue.toObject();
            fields.append(parameter.value(QStringLiteral("label")).toString());
            fields.append(parameter.value(QStringLiteral("unit")).toString());
        }
        QString searchableText = fields.join(QLatin1Char(' ')).toCaseFolded();
        for (const QString &field : std::as_const(fields)) {
            const auto related = m_synonyms.constFind(field.toCaseFolded());
            if (related != m_synonyms.cend())
                searchableText += QLatin1Char(' ') + related->join(QLatin1Char(' '));
        }

        bool matchesQuery = true;
        for (const QString &term : queryTerms) {
            bool termMatches = false;
            const auto related = m_synonyms.constFind(term);
            if (searchableText.contains(term))
                termMatches = true;
            if (!termMatches && related != m_synonyms.cend()) {
                for (const QString &synonym : *related) {
                    if (searchableText.contains(synonym.toCaseFolded())) {
                        termMatches = true;
                        break;
                    }
                }
            }
            if (!termMatches) {
                matchesQuery = false;
                break;
            }
        }
        if (matchesQuery)
            matches.append(&command);
    }
    return matches;
}
