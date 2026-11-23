#include "CommandDraftDialog.h"

#include "../core/FrameBuilder.h"

#include <QAbstractTextDocumentLayout>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextOption>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtMath>
#include <utility>

namespace {

const QStringList ParameterTypes {
    QStringLiteral("integer"), QStringLiteral("enum"),
    QStringLiteral("fixed_ascii"), QStringLiteral("ascii_number")
};

const QStringList ParameterEncodings {
    QStringLiteral("u8"), QStringLiteral("u16be"), QStringLiteral("ascii_channel"),
    QStringLiteral("ascii_decimal"), QStringLiteral("ascii_fixed")
};

bool numericEncoding(const QString &encoding)
{
    return encoding == QStringLiteral("u8") || encoding == QStringLiteral("u16be")
        || encoding == QStringLiteral("ascii_channel") || encoding == QStringLiteral("ascii_decimal");
}

QStringList splitWords(const QString &text)
{
    QStringList words = text.split(QRegularExpression(QStringLiteral("[\\r\\n,，、;；]+")),
                                   Qt::SkipEmptyParts);
    for (QString &word : words)
        word = word.trimmed();
    words.removeAll(QString());
    words.removeDuplicates();
    return words;
}

QJsonArray stringArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

QString optionText(const QJsonArray &options)
{
    QStringList entries;
    for (const QJsonValue &value : options) {
        const QJsonObject option = value.toObject();
        entries.append(QStringLiteral("%1=%2")
                           .arg(option.value(QStringLiteral("value")).toVariant().toString(),
                                option.value(QStringLiteral("label")).toString()));
    }
    return entries.join(QStringLiteral("; "));
}

QJsonObject tableObject(const QTableWidget *table, int row)
{
    const QTableWidgetItem *item = table->item(row, 0);
    return item ? item->data(Qt::UserRole).toJsonObject() : QJsonObject();
}

void setReadOnlyCell(QTableWidget *table, int row, int column, const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (column == 0)
        item->setData(Qt::UserRole, QVariant::fromValue(tableObject(table, row)));
    table->setItem(row, column, item);
}

QString protocolTitle(const QString &protocol)
{
    if (protocol == QStringLiteral("new"))
        return QStringLiteral("新协议 · CA");
    if (protocol == QStringLiteral("old"))
        return QStringLiteral("旧协议 · ASCII");
    return QStringLiteral("旧扩展 · $ / @");
}

QString commandId()
{
    QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    suffix.remove(QLatin1Char('-'));
    return QStringLiteral("DRAFT_%1").arg(suffix.toUpper());
}

class ParameterDefinitionDialog final : public QDialog
{
public:
    explicit ParameterDefinitionDialog(const QJsonObject &initial, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(initial.isEmpty() ? QStringLiteral("添加参数") : QStringLiteral("编辑参数"));
        setMinimumWidth(400);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(6);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(6);
        m_id = new QLineEdit(this);
        m_label = new QLineEdit(this);
        m_type = new QComboBox(this);
        m_type->addItems(ParameterTypes);
        m_encoding = new QComboBox(this);
        m_min = new QLineEdit(this);
        m_max = new QLineEdit(this);
        m_default = new QLineEdit(this);
        m_width = new QLineEdit(this);
        m_unit = new QLineEdit(this);
        m_options = new QLineEdit(this);
        m_options->setPlaceholderText(QStringLiteral("数值=显示名; 另一个值=显示名"));
        m_min->setPlaceholderText(QStringLiteral("整数编码时必填"));
        m_max->setPlaceholderText(QStringLiteral("整数编码时必填"));
        m_width->setPlaceholderText(QStringLiteral("ASCII 固定/数字编码时必填"));
        form->addRow(QStringLiteral("参数 ID"), m_id);
        form->addRow(QStringLiteral("显示名称"), m_label);
        form->addRow(QStringLiteral("参数类型"), m_type);
        form->addRow(QStringLiteral("字段编码"), m_encoding);
        form->addRow(QStringLiteral("最小值"), m_min);
        form->addRow(QStringLiteral("最大值"), m_max);
        form->addRow(QStringLiteral("默认值"), m_default);
        form->addRow(QStringLiteral("位宽"), m_width);
        form->addRow(QStringLiteral("单位"), m_unit);
        form->addRow(QStringLiteral("枚举选项"), m_options);
        layout->addLayout(form);

        auto *hint = new QLabel(QStringLiteral("ASCII 数字按指定位数左侧补零；ASCII 通道把 1~26 映射为 A~Z。枚举项填写“编码值=显示名称”，用分号分隔。"), this);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#5B6676;"));
        layout->addWidget(hint);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            QString error;
            if (!definition(&error).isEmpty())
                accept();
            else
                QMessageBox::warning(this, QStringLiteral("参数定义无效"), error);
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_type, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this](int) { updateEncodingOptions(); });

        m_id->setText(initial.value(QStringLiteral("id")).toString());
        m_label->setText(initial.value(QStringLiteral("label")).toString());
        m_type->setCurrentText(initial.value(QStringLiteral("type")).toString(QStringLiteral("integer")));
        updateEncodingOptions();
        m_encoding->setCurrentText(initial.value(QStringLiteral("encoding")).toString(QStringLiteral("u8")));
        if (initial.contains(QStringLiteral("min")))
            m_min->setText(initial.value(QStringLiteral("min")).toVariant().toString());
        if (initial.contains(QStringLiteral("max")))
            m_max->setText(initial.value(QStringLiteral("max")).toVariant().toString());
        m_default->setText(initial.value(QStringLiteral("default")).toVariant().toString());
        if (initial.contains(QStringLiteral("width")))
            m_width->setText(initial.value(QStringLiteral("width")).toInt() == 0
                                 ? QString() : QString::number(initial.value(QStringLiteral("width")).toInt()));
        m_unit->setText(initial.value(QStringLiteral("unit")).toString());
        m_options->setText(optionText(initial.value(QStringLiteral("options")).toArray()));
    }

    QJsonObject definition(QString *errorMessage) const
    {
        const QString id = m_id->text().trimmed();
        const QString label = m_label->text().trimmed();
        const QString type = m_type->currentText();
        const QString encoding = m_encoding->currentText();
        static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
        if (!idPattern.match(id).hasMatch()) {
            *errorMessage = QStringLiteral("参数 ID 请使用英文字母开头，并只包含字母、数字和下划线。");
            return {};
        }
        if (label.isEmpty() || encoding.isEmpty()) {
            *errorMessage = QStringLiteral("显示名称和字段编码不能为空。");
            return {};
        }

        QJsonObject result {
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), label},
            {QStringLiteral("type"), type},
            {QStringLiteral("encoding"), encoding},
            {QStringLiteral("unit"), m_unit->text().trimmed()}
        };

        if (numericEncoding(encoding)) {
            bool minOk = false;
            bool maxOk = false;
            bool defaultOk = false;
            const qlonglong minimum = m_min->text().trimmed().toLongLong(&minOk);
            const qlonglong maximum = m_max->text().trimmed().toLongLong(&maxOk);
            const qlonglong defaultValue = m_default->text().trimmed().toLongLong(&defaultOk);
            if (!minOk || !maxOk || !defaultOk || minimum > maximum
                || defaultValue < minimum || defaultValue > maximum) {
                *errorMessage = QStringLiteral("数值编码需要有效的整数最小值、最大值和范围内默认值。");
                return {};
            }
            result.insert(QStringLiteral("min"), static_cast<double>(minimum));
            result.insert(QStringLiteral("max"), static_cast<double>(maximum));
            result.insert(QStringLiteral("default"), static_cast<double>(defaultValue));
        } else {
            const QString defaultValue = m_default->text();
            if (defaultValue.isEmpty()) {
                *errorMessage = QStringLiteral("ASCII 参数需要填写默认值。");
                return {};
            }
            result.insert(QStringLiteral("default"), defaultValue);
        }

        if (encoding == QStringLiteral("ascii_decimal") || encoding == QStringLiteral("ascii_fixed")) {
            bool widthOk = false;
            const int width = m_width->text().trimmed().toInt(&widthOk);
            if (!widthOk || width < 1 || width > (encoding == QStringLiteral("ascii_decimal") ? 15 : 4096)) {
                *errorMessage = encoding == QStringLiteral("ascii_decimal")
                    ? QStringLiteral("ASCII 数字位宽需为 1~15。")
                    : QStringLiteral("固定 ASCII 位宽需为 1~4096。");
                return {};
            }
            result.insert(QStringLiteral("width"), width);
        }

        if (type == QStringLiteral("enum")) {
            QJsonArray options;
            QSet<QString> seenValues;
            const QStringList entries = m_options->text().split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString &entry : entries) {
                const qsizetype separator = entry.indexOf(QLatin1Char('='));
                if (separator <= 0) {
                    *errorMessage = QStringLiteral("枚举项格式应为“值=显示名称”，多项之间用分号分隔。");
                    return {};
                }
                const QString rawValue = entry.left(separator).trimmed();
                const QString optionLabel = entry.mid(separator + 1).trimmed();
                if (rawValue.isEmpty() || optionLabel.isEmpty() || seenValues.contains(rawValue)) {
                    *errorMessage = QStringLiteral("枚举项不能空白或重复。");
                    return {};
                }
                seenValues.insert(rawValue);
                QJsonValue optionValue(rawValue);
                if (numericEncoding(encoding)) {
                    bool ok = false;
                    const qlonglong number = rawValue.toLongLong(&ok);
                    if (!ok) {
                        *errorMessage = QStringLiteral("当前编码的枚举值必须是整数。");
                        return {};
                    }
                    optionValue = static_cast<double>(number);
                }
                options.append(QJsonObject {
                    {QStringLiteral("value"), optionValue},
                    {QStringLiteral("label"), optionLabel}
                });
            }
            if (options.isEmpty()) {
                *errorMessage = QStringLiteral("枚举参数至少需要一个选项。");
                return {};
            }
            result.insert(QStringLiteral("options"), options);
        }

        if (errorMessage)
            errorMessage->clear();
        return result;
    }

private:
    void updateEncodingOptions()
    {
        const QString type = m_type->currentText();
        QStringList allowed;
        if (type == QStringLiteral("integer"))
            allowed = {QStringLiteral("u8"), QStringLiteral("u16be"), QStringLiteral("ascii_channel"), QStringLiteral("ascii_decimal")};
        else if (type == QStringLiteral("enum"))
            allowed = ParameterEncodings;
        else if (type == QStringLiteral("fixed_ascii"))
            allowed = {QStringLiteral("ascii_fixed")};
        else
            allowed = {QStringLiteral("ascii_decimal")};

        const QString previous = m_encoding->currentText();
        m_encoding->clear();
        m_encoding->addItems(allowed);
        if (allowed.contains(previous))
            m_encoding->setCurrentText(previous);
        else
            m_encoding->setCurrentIndex(0);
    }

    QLineEdit *m_id = nullptr;
    QLineEdit *m_label = nullptr;
    QComboBox *m_type = nullptr;
    QComboBox *m_encoding = nullptr;
    QLineEdit *m_min = nullptr;
    QLineEdit *m_max = nullptr;
    QLineEdit *m_default = nullptr;
    QLineEdit *m_width = nullptr;
    QLineEdit *m_unit = nullptr;
    QLineEdit *m_options = nullptr;
};

class FixedSegmentDialog final : public QDialog
{
public:
    FixedSegmentDialog(const QString &frameFormat, const QJsonObject &initial, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(initial.isEmpty() ? QStringLiteral("添加固定段") : QStringLiteral("编辑固定段"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(6);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(6);
        m_encoding = new QComboBox(this);
        m_encoding->addItem(QStringLiteral("HEX"), QStringLiteral("hex"));
        m_encoding->addItem(QStringLiteral("ASCII"), QStringLiteral("ascii"));
        m_encoding->setEnabled(false);
        if (frameFormat == QStringLiteral("hex"))
            m_encoding->setCurrentIndex(0);
        else
            m_encoding->setCurrentIndex(1);
        m_value = new QLineEdit(this);
        m_value->setPlaceholderText(frameFormat == QStringLiteral("hex")
                                        ? QStringLiteral("例如 CA 01")
                                        : QStringLiteral("例如 SA 或 #"));
        form->addRow(QStringLiteral("编码"), m_encoding);
        form->addRow(QStringLiteral("固定内容"), m_value);
        layout->addLayout(form);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        if (!initial.isEmpty()
            && initial.value(QStringLiteral("encoding")).toString() == frameFormat) {
            m_encoding->setCurrentIndex(m_encoding->findData(frameFormat));
            m_value->setText(initial.value(QStringLiteral("value")).toString());
        }
    }

    QJsonObject segment() const
    {
        return {
            {QStringLiteral("kind"), QStringLiteral("fixed")},
            {QStringLiteral("encoding"), m_encoding->currentData().toString()},
            {QStringLiteral("value"), m_value->text()}
        };
    }

private:
    QComboBox *m_encoding = nullptr;
    QLineEdit *m_value = nullptr;
};

class ParameterSegmentDialog final : public QDialog
{
public:
    ParameterSegmentDialog(const QJsonArray &parameters, const QJsonObject &initial, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(initial.isEmpty() ? QStringLiteral("插入参数段") : QStringLiteral("编辑参数段"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(6);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(6);
        m_parameter = new QComboBox(this);
        for (const QJsonValue &value : parameters) {
            const QJsonObject parameter = value.toObject();
            const QString id = parameter.value(QStringLiteral("id")).toString();
            m_parameter->addItem(QStringLiteral("%1（%2）").arg(parameter.value(QStringLiteral("label")).toString(), id), id);
        }
        m_encoding = new QLabel(this);
        m_width = new QLabel(this);
        form->addRow(QStringLiteral("使用参数"), m_parameter);
        form->addRow(QStringLiteral("字段编码"), m_encoding);
        form->addRow(QStringLiteral("位宽"), m_width);
        layout->addLayout(form);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确定"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_parameter, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, parameters](int) { updateDescription(parameters); });
        if (!initial.isEmpty())
            m_parameter->setCurrentIndex(m_parameter->findData(initial.value(QStringLiteral("parameter")).toString()));
        updateDescription(parameters);
    }

    QJsonObject segment(const QJsonArray &parameters) const
    {
        const QString parameterId = m_parameter->currentData().toString();
        for (const QJsonValue &value : parameters) {
            const QJsonObject parameter = value.toObject();
            if (parameter.value(QStringLiteral("id")).toString() != parameterId)
                continue;
            QJsonObject result {
                {QStringLiteral("kind"), QStringLiteral("parameter")},
                {QStringLiteral("parameter"), parameterId},
                {QStringLiteral("encoding"), parameter.value(QStringLiteral("encoding")).toString()}
            };
            const QString encoding = parameter.value(QStringLiteral("encoding")).toString();
            if (encoding == QStringLiteral("ascii_decimal") || encoding == QStringLiteral("ascii_fixed"))
                result.insert(QStringLiteral("width"), parameter.value(QStringLiteral("width")).toInt());
            return result;
        }
        return {};
    }

private:
    void updateDescription(const QJsonArray &parameters)
    {
        const QString parameterId = m_parameter->currentData().toString();
        for (const QJsonValue &value : parameters) {
            const QJsonObject parameter = value.toObject();
            if (parameter.value(QStringLiteral("id")).toString() == parameterId) {
                const QString encoding = parameter.value(QStringLiteral("encoding")).toString();
                m_encoding->setText(encoding);
                m_width->setText((encoding == QStringLiteral("ascii_decimal")
                                  || encoding == QStringLiteral("ascii_fixed"))
                                     ? QString::number(parameter.value(QStringLiteral("width")).toInt())
                                     : QStringLiteral("由编码决定"));
                return;
            }
        }
        m_encoding->clear();
        m_width->clear();
    }

    QComboBox *m_parameter = nullptr;
    QLabel *m_encoding = nullptr;
    QLabel *m_width = nullptr;
};

} // namespace

CommandDraftDialog::CommandDraftDialog(const CommandCatalog &catalog, QWidget *parent)
    : QDialog(parent), m_catalog(catalog)
{
    setObjectName(QStringLiteral("draftDialog"));
    buildUi();
    loadDrafts();
    if (m_drafts.isEmpty())
        newDraft();
    else
        m_draftList->setCurrentRow(0);
}

void CommandDraftDialog::buildUi()
{
    setWindowTitle(QStringLiteral("Command Atlas · 研发草稿录入"));
    resize(1120, 700);
    setMinimumSize(980, 600);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("本地指令草稿"), this);
    title->setObjectName(QStringLiteral("commandTitle"));
    m_saveStatus = new QLabel(QStringLiteral("草稿仅保存在本机，不会修改内置指令库。"), this);
    m_saveStatus->setObjectName(QStringLiteral("mutedText"));
    heading->addWidget(title);
    heading->addStretch(1);
    heading->addWidget(m_saveStatus);
    root->addLayout(heading);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 8, 0);
    leftLayout->setSpacing(6);
    leftLayout->addWidget(new QLabel(QStringLiteral("草稿列表"), left));
    m_draftList = new QListWidget(left);
    m_draftList->setObjectName(QStringLiteral("commandList"));
    leftLayout->addWidget(m_draftList, 1);
    auto *newButton = new QPushButton(QStringLiteral("＋ 新建草稿"), left);
    auto *duplicateButton = new QPushButton(QStringLiteral("复制所选条目"), left);
    auto *importButton = new QPushButton(QStringLiteral("导入 JSON…"), left);
    importButton->setToolTip(QStringLiteral("导入标准指令文件到本地草稿；同 ID 自动另建，不覆盖已有草稿。"));
    leftLayout->addWidget(newButton);
    leftLayout->addWidget(duplicateButton);
    leftLayout->addWidget(importButton);

    auto *tabs = new QTabWidget(splitter);
    auto *identityPage = new QWidget(tabs);
    identityPage->setObjectName(QStringLiteral("editorPage"));
    auto *identityForm = new QFormLayout(identityPage);
    identityForm->setContentsMargins(10, 10, 10, 10);
    identityForm->setHorizontalSpacing(10);
    identityForm->setVerticalSpacing(6);
    identityForm->setFormAlignment(Qt::AlignTop);
    m_productInput = new QComboBox(identityPage);
    for (const ProductProfile &product : m_catalog.products())
        m_productInput->addItem(product.name, product.id);
    m_protocolInput = new QComboBox(identityPage);
    m_protocolInput->addItem(protocolTitle(QStringLiteral("new")), QStringLiteral("new"));
    m_protocolInput->addItem(protocolTitle(QStringLiteral("old")), QStringLiteral("old"));
    m_protocolInput->addItem(protocolTitle(QStringLiteral("old_ext")), QStringLiteral("old_ext"));
    m_categoryInput = new QLineEdit(identityPage);
    m_titleInput = new QLineEdit(identityPage);
    m_descriptionInput = new QPlainTextEdit(identityPage);
    m_descriptionInput->setMaximumHeight(76);
    m_aliasesInput = new QPlainTextEdit(identityPage);
    m_aliasesInput->setMaximumHeight(60);
    m_aliasesInput->setPlaceholderText(QStringLiteral("一行一条，也可用逗号分隔"));
    m_keywordsInput = new QPlainTextEdit(identityPage);
    m_keywordsInput->setMaximumHeight(60);
    m_keywordsInput->setPlaceholderText(QStringLiteral("一行一个关键词"));
    identityForm->addRow(QStringLiteral("产品"), m_productInput);
    identityForm->addRow(QStringLiteral("协议"), m_protocolInput);
    identityForm->addRow(QStringLiteral("功能分组"), m_categoryInput);
    identityForm->addRow(QStringLiteral("中文名称"), m_titleInput);
    identityForm->addRow(QStringLiteral("用途说明"), m_descriptionInput);
    identityForm->addRow(QStringLiteral("别名"), m_aliasesInput);
    identityForm->addRow(QStringLiteral("关键词"), m_keywordsInput);
    auto *identityHelp = new QLabel(QStringLiteral("条目 ID 自动生成；协议选择决定帧格式。正式库数据仍由 GitLab 主分支管理。"), identityPage);
    identityHelp->setObjectName(QStringLiteral("mutedText"));
    identityHelp->setWordWrap(true);
    identityForm->addRow(QString(), identityHelp);
    tabs->addTab(identityPage, QStringLiteral("指令信息"));

    auto *parameterPage = new QWidget(tabs);
    parameterPage->setObjectName(QStringLiteral("editorPage"));
    auto *parameterLayout = new QVBoxLayout(parameterPage);
    parameterLayout->setContentsMargins(10, 10, 10, 10);
    parameterLayout->setSpacing(6);
    m_parameterTable = new QTableWidget(0, 6, parameterPage);
    m_parameterTable->setHorizontalHeaderLabels({QStringLiteral("参数 ID"), QStringLiteral("显示名"),
                                                  QStringLiteral("类型"), QStringLiteral("编码"),
                                                  QStringLiteral("范围 / 位宽"), QStringLiteral("默认值")});
    m_parameterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_parameterTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_parameterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_parameterTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_parameterTable->horizontalHeader()->setStretchLastSection(true);
    m_parameterTable->verticalHeader()->setMinimumSectionSize(22);
    m_parameterTable->verticalHeader()->setDefaultSectionSize(26);
    parameterLayout->addWidget(m_parameterTable, 1);
    auto *parameterButtons = new QHBoxLayout;
    parameterButtons->setSpacing(6);
    auto *addParameterButton = new QPushButton(QStringLiteral("添加参数"), parameterPage);
    auto *editParameterButton = new QPushButton(QStringLiteral("编辑"), parameterPage);
    auto *copyParameterButton = new QPushButton(QStringLiteral("复制参数"), parameterPage);
    auto *removeParameterButton = new QPushButton(QStringLiteral("移除"), parameterPage);
    parameterButtons->addWidget(addParameterButton);
    parameterButtons->addWidget(editParameterButton);
    parameterButtons->addWidget(copyParameterButton);
    parameterButtons->addWidget(removeParameterButton);
    parameterButtons->addStretch(1);
    parameterLayout->addLayout(parameterButtons);
    auto *parameterHint = new QLabel(QStringLiteral("类型支持整数、枚举、定长 ASCII 和 ASCII 数字。取值范围、编码、默认值会在预览与导出时校验。"), parameterPage);
    parameterHint->setObjectName(QStringLiteral("mutedText"));
    parameterHint->setWordWrap(true);
    parameterLayout->addWidget(parameterHint);
    tabs->addTab(parameterPage, QStringLiteral("参数定义"));

    auto *segmentPage = new QWidget(tabs);
    segmentPage->setObjectName(QStringLiteral("editorPage"));
    auto *segmentLayout = new QVBoxLayout(segmentPage);
    segmentLayout->setContentsMargins(10, 10, 10, 10);
    segmentLayout->setSpacing(6);
    m_segmentTable = new QTableWidget(0, 4, segmentPage);
    m_segmentTable->setHorizontalHeaderLabels({QStringLiteral("段类型"), QStringLiteral("内容 / 参数"),
                                                QStringLiteral("编码 / 算法"), QStringLiteral("位宽")});
    m_segmentTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_segmentTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_segmentTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_segmentTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_segmentTable->horizontalHeader()->setStretchLastSection(true);
    m_segmentTable->verticalHeader()->setMinimumSectionSize(22);
    m_segmentTable->verticalHeader()->setDefaultSectionSize(26);
    segmentLayout->addWidget(m_segmentTable, 1);
    auto *segmentButtons = new QHBoxLayout;
    segmentButtons->setSpacing(6);
    auto *segmentEditButtons = new QHBoxLayout;
    segmentEditButtons->setSpacing(6);
    auto *addFixedButton = new QPushButton(QStringLiteral("添加固定段"), segmentPage);
    auto *addParameterSegmentButton = new QPushButton(QStringLiteral("插入参数段"), segmentPage);
    auto *addBccButton = new QPushButton(QStringLiteral("追加 XOR BCC"), segmentPage);
    auto *editSegmentButton = new QPushButton(QStringLiteral("编辑"), segmentPage);
    auto *removeSegmentButton = new QPushButton(QStringLiteral("移除"), segmentPage);
    auto *upSegmentButton = new QPushButton(QStringLiteral("上移"), segmentPage);
    auto *downSegmentButton = new QPushButton(QStringLiteral("下移"), segmentPage);
    for (QPushButton *button : {addFixedButton, addParameterSegmentButton, addBccButton})
        segmentButtons->addWidget(button);
    segmentButtons->addStretch(1);
    for (QPushButton *button : {editSegmentButton, removeSegmentButton, upSegmentButton, downSegmentButton})
        segmentEditButtons->addWidget(button);
    segmentEditButtons->addStretch(1);
    segmentLayout->addLayout(segmentButtons);
    segmentLayout->addLayout(segmentEditButtons);
    auto *segmentHint = new QLabel(QStringLiteral("按设备帧顺序排列各段。固定段填写原始内容；参数段选择已定义字段。新协议可在末尾追加 XOR BCC。"), segmentPage);
    segmentHint->setObjectName(QStringLiteral("mutedText"));
    segmentHint->setWordWrap(true);
    segmentLayout->addWidget(segmentHint);
    tabs->addTab(segmentPage, QStringLiteral("帧段编辑"));

    auto *right = new QWidget(splitter);
    right->setMinimumWidth(220);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->setSpacing(6);
    rightLayout->addWidget(new QLabel(QStringLiteral("默认参数预览"), right));
    m_validationMessage = new QTextEdit(right);
    m_validationMessage->setReadOnly(true);
    m_validationMessage->setTabChangesFocus(true);
    m_validationMessage->setFrameShape(QFrame::NoFrame);
    m_validationMessage->setLineWrapMode(QTextEdit::WidgetWidth);
    m_validationMessage->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_validationMessage->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_validationMessage->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_validationMessage->document()->setDocumentMargin(0);
    m_validationMessage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_validationMessage->setFixedHeight(m_validationMessage->fontMetrics().lineSpacing() + 4);
    // Reflow at the actual splitter width; long IDs may wrap within a word.
    // Normal messages grow to their full height; unusually long ones scroll.
    connect(m_validationMessage->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF &size) {
                const int lineHeight = m_validationMessage->fontMetrics().lineSpacing();
                const int height = qBound(lineHeight + 4, qCeil(size.height()) + 4,
                                          6 * lineHeight + 4);
                if (height != m_validationMessage->height())
                    m_validationMessage->setFixedHeight(height);
            });
    rightLayout->addWidget(m_validationMessage);
    m_preview = new QPlainTextEdit(right);
    m_preview->setObjectName(QStringLiteral("framePreview"));
    m_preview->setReadOnly(true);
    rightLayout->addWidget(m_preview, 1);
    auto *saveButton = new QPushButton(QStringLiteral("保存本地草稿"), right);
    saveButton->setObjectName(QStringLiteral("secondaryButton"));
    m_exportButton = new QPushButton(QStringLiteral("导出标准 JSON…"), right);
    m_exportButton->setObjectName(QStringLiteral("primaryButton"));
    rightLayout->addWidget(saveButton);
    rightLayout->addWidget(m_exportButton);

    splitter->addWidget(left);
    splitter->addWidget(tabs);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({180, 650, 250});
    root->addWidget(splitter, 1);

    connect(newButton, &QPushButton::clicked, this, &CommandDraftDialog::newDraft);
    connect(duplicateButton, &QPushButton::clicked, this, &CommandDraftDialog::duplicateCurrentDraft);
    connect(importButton, &QPushButton::clicked, this, &CommandDraftDialog::importDrafts);
    connect(m_draftList, &QListWidget::currentRowChanged, this, &CommandDraftDialog::selectDraft);
    connect(addParameterButton, &QPushButton::clicked, this, &CommandDraftDialog::addParameter);
    connect(editParameterButton, &QPushButton::clicked, this, &CommandDraftDialog::editSelectedParameter);
    connect(m_parameterTable, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { editSelectedParameter(); });
    connect(copyParameterButton, &QPushButton::clicked, this, &CommandDraftDialog::duplicateSelectedParameter);
    connect(removeParameterButton, &QPushButton::clicked, this, &CommandDraftDialog::removeSelectedParameter);
    connect(addFixedButton, &QPushButton::clicked, this, &CommandDraftDialog::addFixedSegment);
    connect(addParameterSegmentButton, &QPushButton::clicked, this, &CommandDraftDialog::addParameterSegment);
    connect(addBccButton, &QPushButton::clicked, this, &CommandDraftDialog::addBccSegment);
    connect(editSegmentButton, &QPushButton::clicked, this, &CommandDraftDialog::editSelectedSegment);
    connect(m_segmentTable, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { editSelectedSegment(); });
    connect(removeSegmentButton, &QPushButton::clicked, this, &CommandDraftDialog::removeSelectedSegment);
    connect(upSegmentButton, &QPushButton::clicked, this, [this]() { moveSelectedSegment(-1); });
    connect(downSegmentButton, &QPushButton::clicked, this, [this]() { moveSelectedSegment(1); });
    connect(saveButton, &QPushButton::clicked, this, &CommandDraftDialog::persistDrafts);
    connect(m_exportButton, &QPushButton::clicked, this, &CommandDraftDialog::exportCurrentDraft);

    const auto changed = [this]() { updateCurrentDraft(); };
    connect(m_productInput, qOverload<int>(&QComboBox::currentIndexChanged), this, [changed](int) { changed(); });
    connect(m_protocolInput, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_loading)
            return;
        const bool newProtocol = m_protocolInput->currentData().toString() == QStringLiteral("new");
        m_protocolInput->setToolTip(newProtocol ? QStringLiteral("新协议帧使用 HEX") : QStringLiteral("旧协议帧使用 ASCII"));
        updateCurrentDraft();
    });
    for (QLineEdit *edit : {m_categoryInput, m_titleInput})
        connect(edit, &QLineEdit::textChanged, this, changed);
    for (QPlainTextEdit *edit : {m_descriptionInput, m_aliasesInput, m_keywordsInput})
        connect(edit, &QPlainTextEdit::textChanged, this, changed);
    connect(m_parameterTable, &QTableWidget::itemSelectionChanged, this,
            [this]() { refreshPreview(); });
    connect(m_segmentTable, &QTableWidget::itemSelectionChanged, this,
            [this]() { refreshPreview(); });

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(650);
    connect(m_saveTimer, &QTimer::timeout, this, &CommandDraftDialog::persistDrafts);
}

void CommandDraftDialog::loadDrafts()
{
    QString error;
    if (!m_store.load(&m_drafts, &error)) {
        m_drafts.clear();
        m_saveStatus->setText(error);
        m_saveStatus->setStyleSheet(QStringLiteral("color:#DC2626;"));
        QMessageBox::warning(this, QStringLiteral("本地草稿无法读取"), error);
        return;
    }
    updateDraftList();
}

void CommandDraftDialog::loadDraftIntoForm(const QJsonObject &draft)
{
    m_loading = true;
    m_loadedDraft = draft;
    m_productInput->setCurrentIndex(m_productInput->findData(
        draft.value(QStringLiteral("product_ids")).toArray().at(0).toString()));
    const bool multipleProducts = draft.value(QStringLiteral("product_ids")).toArray().size() > 1;
    m_productInput->setEnabled(!multipleProducts);
    m_productInput->setToolTip(multipleProducts
        ? QStringLiteral("此模板适用多个产品：%1。当前显示首个产品，完整适用范围保留；本版暂不编辑多产品范围。")
              .arg(QString::fromUtf8(QJsonDocument(draft.value(QStringLiteral("product_ids")).toArray())
                                        .toJson(QJsonDocument::Compact)))
        : QString());
    const QString protocol = draft.value(QStringLiteral("protocol")).toString(QStringLiteral("new"));
    m_protocolInput->setCurrentIndex(m_protocolInput->findData(protocol));
    m_categoryInput->setText(draft.value(QStringLiteral("category")).toString());
    m_titleInput->setText(draft.value(QStringLiteral("title")).toString());
    m_descriptionInput->setPlainText(draft.value(QStringLiteral("description")).toString());
    QStringList aliases;
    for (const QJsonValue &value : draft.value(QStringLiteral("aliases")).toArray())
        aliases.append(value.toString());
    m_aliasesInput->setPlainText(aliases.join(QLatin1Char('\n')));
    QStringList keywords;
    for (const QJsonValue &value : draft.value(QStringLiteral("keywords")).toArray())
        keywords.append(value.toString());
    m_keywordsInput->setPlainText(keywords.join(QLatin1Char('\n')));
    // Compare edits against the rendered text, since text widgets normalize CR/LF.
    m_loadedText = {
        {QStringLiteral("category"), m_categoryInput->text()},
        {QStringLiteral("title"), m_titleInput->text()},
        {QStringLiteral("description"), m_descriptionInput->toPlainText()},
        {QStringLiteral("aliases"), m_aliasesInput->toPlainText()},
        {QStringLiteral("keywords"), m_keywordsInput->toPlainText()}
    };

    const QJsonArray parameters = draft.value(QStringLiteral("parameters")).toArray();
    m_parameterTable->setRowCount(parameters.size());
    for (qsizetype row = 0; row < parameters.size(); ++row) {
        const QJsonObject parameter = parameters.at(row).toObject();
        auto *idItem = new QTableWidgetItem(parameter.value(QStringLiteral("id")).toString());
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        idItem->setData(Qt::UserRole, QVariant::fromValue(parameter));
        m_parameterTable->setItem(static_cast<int>(row), 0, idItem);
        m_parameterTable->setItem(static_cast<int>(row), 1, new QTableWidgetItem(parameter.value(QStringLiteral("label")).toString()));
        m_parameterTable->setItem(static_cast<int>(row), 2, new QTableWidgetItem(parameter.value(QStringLiteral("type")).toString()));
        m_parameterTable->setItem(static_cast<int>(row), 3, new QTableWidgetItem(parameter.value(QStringLiteral("encoding")).toString()));
        QString range;
        if (parameter.contains(QStringLiteral("min")))
            range = QStringLiteral("%1~%2").arg(parameter.value(QStringLiteral("min")).toVariant().toString(),
                                                parameter.value(QStringLiteral("max")).toVariant().toString());
        if (parameter.contains(QStringLiteral("width")))
            range += (range.isEmpty() ? QString() : QStringLiteral(" · "))
                + QStringLiteral("%1 位").arg(parameter.value(QStringLiteral("width")).toInt());
        m_parameterTable->setItem(static_cast<int>(row), 4, new QTableWidgetItem(range));
        m_parameterTable->setItem(static_cast<int>(row), 5,
                                  new QTableWidgetItem(parameter.value(QStringLiteral("default")).toVariant().toString()));
    }

    const QJsonArray segments = draft.value(QStringLiteral("segments")).toArray();
    m_segmentTable->setRowCount(segments.size());
    for (qsizetype row = 0; row < segments.size(); ++row) {
        const QJsonObject segment = segments.at(row).toObject();
        const QString kind = segment.value(QStringLiteral("kind")).toString();
        QString typeLabel = kind == QStringLiteral("fixed") ? QStringLiteral("固定段")
            : kind == QStringLiteral("parameter") ? QStringLiteral("参数段") : QStringLiteral("计算字段");
        QString content;
        QString encoding;
        if (kind == QStringLiteral("fixed")) {
            content = segment.value(QStringLiteral("value")).toString();
            encoding = segment.value(QStringLiteral("encoding")).toString();
        } else if (kind == QStringLiteral("parameter")) {
            const QString id = segment.value(QStringLiteral("parameter")).toString();
            content = id;
            encoding = segment.value(QStringLiteral("encoding")).toString();
        } else {
            content = segment.value(QStringLiteral("algorithm")).toString();
            encoding = QStringLiteral("XOR BCC");
        }
        const int tableRow = static_cast<int>(row);
        auto *kindItem = new QTableWidgetItem(typeLabel);
        kindItem->setData(Qt::UserRole, QVariant::fromValue(segment));
        kindItem->setFlags(kindItem->flags() & ~Qt::ItemIsEditable);
        m_segmentTable->setItem(tableRow, 0, kindItem);
        m_segmentTable->setItem(tableRow, 1, new QTableWidgetItem(content));
        m_segmentTable->setItem(tableRow, 2, new QTableWidgetItem(encoding));
        m_segmentTable->setItem(tableRow, 3,
                                new QTableWidgetItem(segment.contains(QStringLiteral("width"))
                                                         ? QString::number(segment.value(QStringLiteral("width")).toInt())
                                                         : QString()));
    }
    m_loading = false;
    m_saveStatus->setStyleSheet(QString());
    m_saveStatus->setText(QStringLiteral("本地草稿 · %1 条").arg(m_drafts.size()));
    refreshPreview();
}

QJsonArray CommandDraftDialog::parametersFromTable(QString *errorMessage) const
{
    QJsonArray parameters;
    for (int row = 0; row < m_parameterTable->rowCount(); ++row) {
        const QJsonObject parameter = tableObject(m_parameterTable, row);
        if (parameter.isEmpty()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("参数表第 %1 行数据无效。").arg(row + 1);
            return {};
        }
        parameters.append(parameter);
    }
    if (errorMessage)
        errorMessage->clear();
    return parameters;
}

QJsonArray CommandDraftDialog::segmentsFromTable(QString *errorMessage) const
{
    QJsonArray segments;
    for (int row = 0; row < m_segmentTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_segmentTable->item(row, 0);
        const QJsonObject segment = item ? item->data(Qt::UserRole).toJsonObject() : QJsonObject();
        if (segment.isEmpty()) {
            if (errorMessage)
                *errorMessage = QStringLiteral("帧段表第 %1 行数据无效。").arg(row + 1);
            return {};
        }
        segments.append(segment);
    }
    if (errorMessage)
        errorMessage->clear();
    return segments;
}

QJsonObject CommandDraftDialog::collectDraft() const
{
    QString ignored;
    // Preserve fields not edited by this form, including extension metadata and product scope.
    QJsonObject draft = m_loadedDraft;
    const auto setText = [this, &draft](const QString &key, const QString &text) {
        if (text != m_loadedText.value(key))
            draft.insert(key, text.trimmed());
    };
    const auto setWords = [this, &draft](const QString &key, const QString &text) {
        if (text != m_loadedText.value(key))
            draft.insert(key, stringArray(splitWords(text)));
    };
    draft.insert(QStringLiteral("id"), m_currentId);
    if (m_productInput->currentData().toString()
        != draft.value(QStringLiteral("product_ids")).toArray().at(0).toString()) {
        draft.insert(QStringLiteral("product_ids"), QJsonArray {m_productInput->currentData().toString()});
    }
    draft.insert(QStringLiteral("protocol"), m_protocolInput->currentData().toString());
    setText(QStringLiteral("category"), m_categoryInput->text());
    setText(QStringLiteral("title"), m_titleInput->text());
    setText(QStringLiteral("description"), m_descriptionInput->toPlainText());
    setWords(QStringLiteral("aliases"), m_aliasesInput->toPlainText());
    setWords(QStringLiteral("keywords"), m_keywordsInput->toPlainText());
    draft.insert(QStringLiteral("frame_format"), m_protocolInput->currentData().toString() == QStringLiteral("new")
                     ? QStringLiteral("hex") : QStringLiteral("ascii"));
    draft.insert(QStringLiteral("parameters"), parametersFromTable(&ignored));
    draft.insert(QStringLiteral("segments"), segmentsFromTable(&ignored));
    return draft;
}

void CommandDraftDialog::updateCurrentDraft()
{
    updateCurrentDraft(true);
}

void CommandDraftDialog::updateCurrentDraft(bool scheduleSave)
{
    if (m_loading || m_currentId.isEmpty())
        return;
    const QJsonObject current = collectDraft();
    for (QJsonObject &draft : m_drafts) {
        if (draft.value(QStringLiteral("id")).toString() == m_currentId) {
            draft = current;
            break;
        }
    }
    for (int row = 0; row < m_draftList->count(); ++row) {
        QListWidgetItem *item = m_draftList->item(row);
        if (item->data(Qt::UserRole).toString() != m_currentId)
            continue;
        const QString title = current.value(QStringLiteral("title")).toString();
        item->setText(QStringLiteral("%1\n%2 · %3")
                          .arg(title.isEmpty() ? QStringLiteral("未命名草稿") : title,
                               current.value(QStringLiteral("category")).toString(),
                               protocolTitle(current.value(QStringLiteral("protocol")).toString())));
        break;
    }
    refreshPreview();
    m_saveStatus->setStyleSheet(QStringLiteral("color:#956000;"));
    m_saveStatus->setText(QStringLiteral("草稿有未保存更改"));
    if (scheduleSave && m_saveTimer)
        m_saveTimer->start();
}

void CommandDraftDialog::persistDrafts()
{
    if (m_saveTimer)
        m_saveTimer->stop();
    updateCurrentDraft(false);
    QString error;
    if (!m_store.save(m_drafts, &error)) {
        m_saveStatus->setStyleSheet(QStringLiteral("color:#DC2626;"));
        m_saveStatus->setText(error);
        return;
    }
    m_saveStatus->setStyleSheet(QStringLiteral("color:#16834A;"));
    m_saveStatus->setText(QStringLiteral("本地草稿已保存 · %1 条").arg(m_drafts.size()));
    updateDraftList();
    const int row = -1;
    Q_UNUSED(row);
    for (int index = 0; index < m_draftList->count(); ++index) {
        if (m_draftList->item(index)->data(Qt::UserRole).toString() == m_currentId) {
            m_draftList->setCurrentRow(index);
            break;
        }
    }
}

void CommandDraftDialog::updateDraftList()
{
    const QString selectedId = m_currentId;
    m_loading = true;
    m_draftList->clear();
    int selectedRow = -1;
    for (qsizetype index = 0; index < m_drafts.size(); ++index) {
        const QJsonObject &draft = m_drafts.at(index);
        const QString id = draft.value(QStringLiteral("id")).toString();
        const QString title = draft.value(QStringLiteral("title")).toString();
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2 · %3")
                .arg(title.isEmpty() ? QStringLiteral("未命名草稿") : title,
                     draft.value(QStringLiteral("category")).toString(),
                     protocolTitle(draft.value(QStringLiteral("protocol")).toString())), m_draftList);
        item->setData(Qt::UserRole, id);
        if (id == selectedId)
            selectedRow = static_cast<int>(index);
    }
    if (selectedRow >= 0)
        m_draftList->setCurrentRow(selectedRow);
    m_loading = false;
}

void CommandDraftDialog::newDraft()
{
    updateCurrentDraft(false);
    QJsonObject draft {
        {QStringLiteral("id"), commandId()},
        {QStringLiteral("product_ids"), QJsonArray {m_productInput->currentData().toString()}},
        {QStringLiteral("protocol"), QStringLiteral("new")},
        {QStringLiteral("category"), QString()},
        {QStringLiteral("title"), QString()},
        {QStringLiteral("description"), QString()},
        {QStringLiteral("aliases"), QJsonArray()},
        {QStringLiteral("keywords"), QJsonArray()},
        {QStringLiteral("frame_format"), QStringLiteral("hex")},
        {QStringLiteral("parameters"), QJsonArray()},
        {QStringLiteral("segments"), QJsonArray()}
    };
    m_drafts.append(draft);
    m_currentId = draft.value(QStringLiteral("id")).toString();
    updateDraftList();
    loadDraftIntoForm(draft);
    m_titleInput->setFocus();
}

void CommandDraftDialog::duplicateCurrentDraft()
{
    updateCurrentDraft(false);
    QJsonObject duplicate;
    for (const QJsonObject &draft : std::as_const(m_drafts)) {
        if (draft.value(QStringLiteral("id")).toString() == m_currentId) {
            duplicate = draft;
            break;
        }
    }
    if (duplicate.isEmpty())
        return;
    duplicate.insert(QStringLiteral("id"), commandId());
    const QString title = duplicate.value(QStringLiteral("title")).toString();
    duplicate.insert(QStringLiteral("title"), title.isEmpty() ? QStringLiteral("新指令副本")
                                                               : title + QStringLiteral("（副本）"));
    m_drafts.append(duplicate);
    m_currentId = duplicate.value(QStringLiteral("id")).toString();
    updateDraftList();
    loadDraftIntoForm(duplicate);
}

void CommandDraftDialog::selectDraft(int row)
{
    if (m_loading || row < 0 || row >= m_drafts.size())
        return;
    updateCurrentDraft(false);
    const QJsonObject draft = m_drafts.at(row);
    m_currentId = draft.value(QStringLiteral("id")).toString();
    loadDraftIntoForm(draft);
}

void CommandDraftDialog::addParameter()
{
    ParameterDefinitionDialog dialog({}, this);
    dialog.setWindowModality(Qt::WindowModal);
    QString error;
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject definition = dialog.definition(&error);
    for (int row = 0; row < m_parameterTable->rowCount(); ++row) {
        if (tableObject(m_parameterTable, row).value(QStringLiteral("id")).toString()
            == definition.value(QStringLiteral("id")).toString()) {
            QMessageBox::warning(this, QStringLiteral("参数 ID 重复"), QStringLiteral("请使用不同的参数 ID。"));
            return;
        }
    }
    const int row = m_parameterTable->rowCount();
    m_parameterTable->insertRow(row);
    auto *idItem = new QTableWidgetItem(definition.value(QStringLiteral("id")).toString());
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    idItem->setData(Qt::UserRole, QVariant::fromValue(definition));
    m_parameterTable->setItem(row, 0, idItem);
    m_parameterTable->setItem(row, 1, new QTableWidgetItem(definition.value(QStringLiteral("label")).toString()));
    m_parameterTable->setItem(row, 2, new QTableWidgetItem(definition.value(QStringLiteral("type")).toString()));
    m_parameterTable->setItem(row, 3, new QTableWidgetItem(definition.value(QStringLiteral("encoding")).toString()));
    QString range;
    if (definition.contains(QStringLiteral("min")))
        range = QStringLiteral("%1~%2").arg(definition.value(QStringLiteral("min")).toVariant().toString(),
                                            definition.value(QStringLiteral("max")).toVariant().toString());
    if (definition.contains(QStringLiteral("width")))
        range += (range.isEmpty() ? QString() : QStringLiteral(" · "))
            + QStringLiteral("%1 位").arg(definition.value(QStringLiteral("width")).toInt());
    m_parameterTable->setItem(row, 4, new QTableWidgetItem(range));
    m_parameterTable->setItem(row, 5, new QTableWidgetItem(definition.value(QStringLiteral("default")).toVariant().toString()));
    m_parameterTable->setCurrentCell(row, 0);
    updateCurrentDraft();
}

void CommandDraftDialog::editSelectedParameter()
{
    const int row = m_parameterTable->currentRow();
    if (row < 0)
        return;
    const QJsonObject oldDefinition = tableObject(m_parameterTable, row);
    ParameterDefinitionDialog dialog(oldDefinition, this);
    QString error;
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject definition = dialog.definition(&error);
    for (int other = 0; other < m_parameterTable->rowCount(); ++other) {
        if (other != row && tableObject(m_parameterTable, other).value(QStringLiteral("id")).toString()
            == definition.value(QStringLiteral("id")).toString()) {
            QMessageBox::warning(this, QStringLiteral("参数 ID 重复"), QStringLiteral("请使用不同的参数 ID。"));
            return;
        }
    }
    auto *idItem = m_parameterTable->item(row, 0);
    idItem->setText(definition.value(QStringLiteral("id")).toString());
    idItem->setData(Qt::UserRole, QVariant::fromValue(definition));
    m_parameterTable->item(row, 1)->setText(definition.value(QStringLiteral("label")).toString());
    m_parameterTable->item(row, 2)->setText(definition.value(QStringLiteral("type")).toString());
    m_parameterTable->item(row, 3)->setText(definition.value(QStringLiteral("encoding")).toString());
    QString range;
    if (definition.contains(QStringLiteral("min")))
        range = QStringLiteral("%1~%2").arg(definition.value(QStringLiteral("min")).toVariant().toString(),
                                            definition.value(QStringLiteral("max")).toVariant().toString());
    if (definition.contains(QStringLiteral("width")))
        range += (range.isEmpty() ? QString() : QStringLiteral(" · "))
            + QStringLiteral("%1 位").arg(definition.value(QStringLiteral("width")).toInt());
    m_parameterTable->item(row, 4)->setText(range);
    m_parameterTable->item(row, 5)->setText(definition.value(QStringLiteral("default")).toVariant().toString());

    const QString oldId = oldDefinition.value(QStringLiteral("id")).toString();
    const QString newId = definition.value(QStringLiteral("id")).toString();
    if (oldId != newId) {
        for (int segmentRow = 0; segmentRow < m_segmentTable->rowCount(); ++segmentRow) {
            QTableWidgetItem *segmentItem = m_segmentTable->item(segmentRow, 0);
            QJsonObject segment = segmentItem->data(Qt::UserRole).toJsonObject();
            if (segment.value(QStringLiteral("kind")).toString() == QStringLiteral("parameter")
                && segment.value(QStringLiteral("parameter")).toString() == oldId) {
                segment.insert(QStringLiteral("parameter"), newId);
                segment.insert(QStringLiteral("encoding"), definition.value(QStringLiteral("encoding")).toString());
                const QString encoding = definition.value(QStringLiteral("encoding")).toString();
                if (encoding == QStringLiteral("ascii_decimal") || encoding == QStringLiteral("ascii_fixed"))
                    segment.insert(QStringLiteral("width"), definition.value(QStringLiteral("width")).toInt());
                else
                    segment.remove(QStringLiteral("width"));
                segmentItem->setData(Qt::UserRole, QVariant::fromValue(segment));
                m_segmentTable->item(segmentRow, 1)->setText(newId);
                m_segmentTable->item(segmentRow, 2)->setText(definition.value(QStringLiteral("encoding")).toString());
                m_segmentTable->item(segmentRow, 3)->setText(segment.contains(QStringLiteral("width"))
                                                                  ? QString::number(segment.value(QStringLiteral("width")).toInt())
                                                                  : QString());
            }
        }
    } else {
        for (int segmentRow = 0; segmentRow < m_segmentTable->rowCount(); ++segmentRow) {
            QTableWidgetItem *segmentItem = m_segmentTable->item(segmentRow, 0);
            QJsonObject segment = segmentItem->data(Qt::UserRole).toJsonObject();
            if (segment.value(QStringLiteral("kind")).toString() == QStringLiteral("parameter")
                && segment.value(QStringLiteral("parameter")).toString() == oldId) {
                segment.insert(QStringLiteral("encoding"), definition.value(QStringLiteral("encoding")).toString());
                const QString encoding = definition.value(QStringLiteral("encoding")).toString();
                if (encoding == QStringLiteral("ascii_decimal") || encoding == QStringLiteral("ascii_fixed"))
                    segment.insert(QStringLiteral("width"), definition.value(QStringLiteral("width")).toInt());
                else
                    segment.remove(QStringLiteral("width"));
                segmentItem->setData(Qt::UserRole, QVariant::fromValue(segment));
                m_segmentTable->item(segmentRow, 2)->setText(definition.value(QStringLiteral("encoding")).toString());
                m_segmentTable->item(segmentRow, 3)->setText(segment.contains(QStringLiteral("width"))
                                                                  ? QString::number(segment.value(QStringLiteral("width")).toInt())
                                                                  : QString());
            }
        }
    }
    updateCurrentDraft();
}

void CommandDraftDialog::duplicateSelectedParameter()
{
    const int row = m_parameterTable->currentRow();
    if (row < 0)
        return;
    QJsonObject copy = tableObject(m_parameterTable, row);
    copy.insert(QStringLiteral("id"), copy.value(QStringLiteral("id")).toString() + QStringLiteral("_copy"));
    copy.insert(QStringLiteral("label"), copy.value(QStringLiteral("label")).toString() + QStringLiteral("（副本）"));
    ParameterDefinitionDialog dialog(copy, this);
    QString error;
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject definition = dialog.definition(&error);
    for (int other = 0; other < m_parameterTable->rowCount(); ++other) {
        if (tableObject(m_parameterTable, other).value(QStringLiteral("id")).toString()
            == definition.value(QStringLiteral("id")).toString()) {
            QMessageBox::warning(this, QStringLiteral("参数 ID 重复"), QStringLiteral("请使用不同的参数 ID。"));
            return;
        }
    }
    const int newRow = m_parameterTable->rowCount();
    m_parameterTable->insertRow(newRow);
    auto *idItem = new QTableWidgetItem(definition.value(QStringLiteral("id")).toString());
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    idItem->setData(Qt::UserRole, QVariant::fromValue(definition));
    m_parameterTable->setItem(newRow, 0, idItem);
    m_parameterTable->setItem(newRow, 1, new QTableWidgetItem(definition.value(QStringLiteral("label")).toString()));
    m_parameterTable->setItem(newRow, 2, new QTableWidgetItem(definition.value(QStringLiteral("type")).toString()));
    m_parameterTable->setItem(newRow, 3, new QTableWidgetItem(definition.value(QStringLiteral("encoding")).toString()));
    const QString range = definition.contains(QStringLiteral("min"))
        ? QStringLiteral("%1~%2").arg(definition.value(QStringLiteral("min")).toVariant().toString(),
                                      definition.value(QStringLiteral("max")).toVariant().toString()) : QString();
    m_parameterTable->setItem(newRow, 4, new QTableWidgetItem(range));
    m_parameterTable->setItem(newRow, 5, new QTableWidgetItem(definition.value(QStringLiteral("default")).toVariant().toString()));
    m_parameterTable->setCurrentCell(newRow, 0);
    updateCurrentDraft();
}

void CommandDraftDialog::removeSelectedParameter()
{
    const int row = m_parameterTable->currentRow();
    if (row < 0)
        return;
    const QString id = tableObject(m_parameterTable, row).value(QStringLiteral("id")).toString();
    for (int segmentRow = 0; segmentRow < m_segmentTable->rowCount(); ++segmentRow) {
        const QJsonObject segment = m_segmentTable->item(segmentRow, 0)->data(Qt::UserRole).toJsonObject();
        if (segment.value(QStringLiteral("kind")).toString() == QStringLiteral("parameter")
            && segment.value(QStringLiteral("parameter")).toString() == id) {
            QMessageBox::information(this, QStringLiteral("参数仍在使用"),
                                     QStringLiteral("请先移除引用此参数的帧段，再移除参数定义。"));
            return;
        }
    }
    m_parameterTable->removeRow(row);
    updateCurrentDraft();
}

void CommandDraftDialog::addFixedSegment()
{
    const QString protocol = m_protocolInput->currentData().toString();
    FixedSegmentDialog dialog(protocol == QStringLiteral("new") ? QStringLiteral("hex") : QStringLiteral("ascii"), {}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject segment = dialog.segment();
    int row = m_segmentTable->currentRow();
    row = row < 0 ? m_segmentTable->rowCount() : row + 1;
    if (m_segmentTable->rowCount() > 0
        && m_segmentTable->item(m_segmentTable->rowCount() - 1, 0)->data(Qt::UserRole)
               .toJsonObject().value(QStringLiteral("kind")).toString() == QStringLiteral("computed"))
        row = qMin(row, m_segmentTable->rowCount() - 1);
    m_segmentTable->insertRow(row);
    auto *item = new QTableWidgetItem(QStringLiteral("固定段"));
    item->setData(Qt::UserRole, QVariant::fromValue(segment));
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    m_segmentTable->setItem(row, 0, item);
    m_segmentTable->setItem(row, 1, new QTableWidgetItem(segment.value(QStringLiteral("value")).toString()));
    m_segmentTable->setItem(row, 2, new QTableWidgetItem(segment.value(QStringLiteral("encoding")).toString()));
    m_segmentTable->setItem(row, 3, new QTableWidgetItem(QString()));
    m_segmentTable->setCurrentCell(row, 0);
    updateCurrentDraft();
}

void CommandDraftDialog::addParameterSegment()
{
    QString ignored;
    const QJsonArray parameters = parametersFromTable(&ignored);
    if (parameters.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚无参数"), QStringLiteral("请先添加参数定义。"));
        return;
    }
    ParameterSegmentDialog dialog(parameters, {}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject segment = dialog.segment(parameters);
    int row = m_segmentTable->currentRow();
    row = row < 0 ? m_segmentTable->rowCount() : row + 1;
    if (m_segmentTable->rowCount() > 0
        && m_segmentTable->item(m_segmentTable->rowCount() - 1, 0)->data(Qt::UserRole)
               .toJsonObject().value(QStringLiteral("kind")).toString() == QStringLiteral("computed"))
        row = qMin(row, m_segmentTable->rowCount() - 1);
    m_segmentTable->insertRow(row);
    auto *item = new QTableWidgetItem(QStringLiteral("参数段"));
    item->setData(Qt::UserRole, QVariant::fromValue(segment));
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    m_segmentTable->setItem(row, 0, item);
    m_segmentTable->setItem(row, 1, new QTableWidgetItem(segment.value(QStringLiteral("parameter")).toString()));
    m_segmentTable->setItem(row, 2, new QTableWidgetItem(segment.value(QStringLiteral("encoding")).toString()));
    m_segmentTable->setItem(row, 3, new QTableWidgetItem(segment.contains(QStringLiteral("width"))
                                                              ? QString::number(segment.value(QStringLiteral("width")).toInt())
                                                              : QString()));
    m_segmentTable->setCurrentCell(row, 0);
    updateCurrentDraft();
}

void CommandDraftDialog::addBccSegment()
{
    if (m_protocolInput->currentData().toString() != QStringLiteral("new")) {
        QMessageBox::information(this, QStringLiteral("此協議不使用 BCC"), QStringLiteral("只有 HEX 新協議可添加 XOR BCC。"));
        return;
    }
    for (int row = 0; row < m_segmentTable->rowCount(); ++row) {
        if (m_segmentTable->item(row, 0)->data(Qt::UserRole).toJsonObject()
                .value(QStringLiteral("kind")).toString() == QStringLiteral("computed")) {
            QMessageBox::information(this, QStringLiteral("BCC 已存在"), QStringLiteral("每條模板只能有一個末尾 XOR BCC。"));
            return;
        }
    }
    const int row = m_segmentTable->rowCount();
    m_segmentTable->insertRow(row);
    const QJsonObject segment {{QStringLiteral("kind"), QStringLiteral("computed")},
                               {QStringLiteral("algorithm"), QStringLiteral("xor_bcc")}};
    auto *item = new QTableWidgetItem(QStringLiteral("计算字段"));
    item->setData(Qt::UserRole, QVariant::fromValue(segment));
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    m_segmentTable->setItem(row, 0, item);
    m_segmentTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("xor_bcc")));
    m_segmentTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("XOR BCC")));
    m_segmentTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("1")));
    m_segmentTable->setCurrentCell(row, 0);
    updateCurrentDraft();
}

void CommandDraftDialog::editSelectedSegment()
{
    const int row = m_segmentTable->currentRow();
    if (row < 0)
        return;
    const QJsonObject current = m_segmentTable->item(row, 0)->data(Qt::UserRole).toJsonObject();
    QJsonObject edited;
    const QString kind = current.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("fixed")) {
        const QString frameFormat = m_protocolInput->currentData().toString() == QStringLiteral("new")
            ? QStringLiteral("hex") : QStringLiteral("ascii");
        FixedSegmentDialog dialog(frameFormat, current, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        edited = dialog.segment();
    } else if (kind == QStringLiteral("parameter")) {
        QString ignored;
        ParameterSegmentDialog dialog(parametersFromTable(&ignored), current, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        edited = dialog.segment(parametersFromTable(&ignored));
    } else {
        return;
    }
    QTableWidgetItem *item = m_segmentTable->item(row, 0);
    item->setData(Qt::UserRole, QVariant::fromValue(edited));
    m_segmentTable->item(row, 1)->setText(kind == QStringLiteral("fixed")
                                               ? edited.value(QStringLiteral("value")).toString()
                                               : edited.value(QStringLiteral("parameter")).toString());
    m_segmentTable->item(row, 2)->setText(edited.value(QStringLiteral("encoding")).toString());
    m_segmentTable->item(row, 3)->setText(edited.contains(QStringLiteral("width"))
                                              ? QString::number(edited.value(QStringLiteral("width")).toInt())
                                              : QString());
    updateCurrentDraft();
}

void CommandDraftDialog::removeSelectedSegment()
{
    const int row = m_segmentTable->currentRow();
    if (row >= 0) {
        m_segmentTable->removeRow(row);
        updateCurrentDraft();
    }
}

void CommandDraftDialog::moveSelectedSegment(int direction)
{
    const int row = m_segmentTable->currentRow();
    const int target = row + direction;
    if (row < 0 || target < 0 || target >= m_segmentTable->rowCount())
        return;
    const QJsonObject current = tableObject(m_segmentTable, row);
    const QJsonObject other = tableObject(m_segmentTable, target);
    if (current.value(QStringLiteral("kind")).toString() == QStringLiteral("computed")
        || other.value(QStringLiteral("kind")).toString() == QStringLiteral("computed"))
        return;
    m_loading = true;
    auto swapRows = [this](int first, int second) {
        const int columns = m_segmentTable->columnCount();
        QVector<QString> firstTexts(columns);
        QVector<QString> secondTexts(columns);
        QJsonObject firstObject = tableObject(m_segmentTable, first);
        QJsonObject secondObject = tableObject(m_segmentTable, second);
        for (int column = 0; column < columns; ++column) {
            firstTexts[column] = m_segmentTable->item(first, column)->text();
            secondTexts[column] = m_segmentTable->item(second, column)->text();
        }
        for (int column = 0; column < columns; ++column) {
            m_segmentTable->item(first, column)->setText(secondTexts.at(column));
            m_segmentTable->item(second, column)->setText(firstTexts.at(column));
        }
        m_segmentTable->item(first, 0)->setData(Qt::UserRole, QVariant::fromValue(secondObject));
        m_segmentTable->item(second, 0)->setData(Qt::UserRole, QVariant::fromValue(firstObject));
    };
    swapRows(row, target);
    m_segmentTable->setCurrentCell(target, 0);
    m_loading = false;
    updateCurrentDraft();
}

void CommandDraftDialog::refreshPreview()
{
    if (!m_preview || m_loading)
        return;
    const QJsonObject draft = collectDraft();
    CommandTemplate command;
    QString error;
    if (!m_catalog.validateTemplate(draft, &command, &error)) {
        m_validationMessage->setStyleSheet(QStringLiteral("color:#956000; background:transparent; border:0; padding:0;"));
        m_validationMessage->setPlainText(error);
        m_preview->setPlainText(QStringLiteral("默认帧暂不可生成。\n\n请先填写名称、说明与适用产品，再补齐参数和有序帧段。"));
        m_exportButton->setEnabled(false);
        return;
    }
    const FrameBuildResult result = FrameBuilder::build(command, FrameBuilder::defaultParameters(command));
    if (!result.ok) {
        m_validationMessage->setStyleSheet(QStringLiteral("color:#DC2626; background:transparent; border:0; padding:0;"));
        m_validationMessage->setPlainText(result.errorMessage);
        m_preview->clear();
        m_exportButton->setEnabled(false);
        return;
    }
    m_validationMessage->setStyleSheet(QStringLiteral("color:#16834A; background:transparent; border:0; padding:0;"));
    m_validationMessage->setPlainText(QStringLiteral("模板字段、引用与默认帧校验通过"));
    m_preview->setPlainText(QStringLiteral("HEX    %1\nASCII  %2\n\n参数数：%3\n帧段数：%4")
                                 .arg(FrameBuilder::hexText(result.bytes), FrameBuilder::escapedAscii(result.bytes))
                                 .arg(command.parameters.size())
                                 .arg(command.segments.size()));
    m_exportButton->setEnabled(true);
}

void CommandDraftDialog::importDrafts()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入指令 JSON 到本地草稿"),
                                                     QString(), QStringLiteral("Command Atlas 指令库 (*.json)"));
    if (path.isEmpty())
        return;

    QVector<QJsonObject> imported;
    int renamedIds = 0;
    QString error;
    if (!m_store.readImport(path, m_catalog, m_drafts, &imported, &renamedIds, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    QVector<QJsonObject> candidate = m_drafts;
    for (QJsonObject &draft : candidate) {
        if (draft.value(QStringLiteral("id")).toString() == m_currentId)
            draft = collectDraft();
    }
    candidate += imported;
    // Commit to disk first; a failed save must not activate a partial import in the editor.
    if (!m_store.save(candidate, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入未保存"),
                             error + QStringLiteral("\n本次导入未生效，原草稿保留。"));
        return;
    }
    m_saveTimer->stop();
    m_drafts = candidate;
    m_currentId = imported.first().value(QStringLiteral("id")).toString();
    updateDraftList();
    loadDraftIntoForm(imported.first());
    m_saveStatus->setText(QStringLiteral("已导入并保存 %1 条 · 本地共 %2 条").arg(imported.size()).arg(m_drafts.size()));
    m_saveStatus->setStyleSheet(QStringLiteral("color:#16834A;"));
    QMessageBox::information(this, QStringLiteral("导入完成"),
                             QStringLiteral("已导入 %1 条并保存为本地草稿。\n%2 条因 ID 已存在而分配了新 ID，其余字段不变。\n已有草稿和正式指令库未被覆盖。")
                                 .arg(imported.size()).arg(renamedIds));
}

void CommandDraftDialog::exportCurrentDraft()
{
    updateCurrentDraft(false);
    const QJsonObject draft = collectDraft();
    CommandTemplate command;
    QString error;
    if (!m_catalog.validateTemplate(draft, &command, &error)) {
        QMessageBox::warning(this, QStringLiteral("草稿尚未通過校驗"), error);
        return;
    }

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出指令库 JSON"),
                                                     QStringLiteral("commands.json"),
                                                     QStringLiteral("Command Atlas 指令庫 (*.json)"));
    if (path.isEmpty())
        return;
    QJsonObject root {
        {QStringLiteral("format"), QStringLiteral("command-atlas-commands")},
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("commands"), QJsonArray {draft}}
    };
    QSaveFile file(path);
    const QByteArray serialized = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(serialized) != serialized.size()
        || !file.commit()) {
        QMessageBox::critical(this, QStringLiteral("导出失败"),
                              QStringLiteral("无法写入 JSON 文件：%1").arg(file.errorString()));
        return;
    }
    m_saveStatus->setStyleSheet(QStringLiteral("color:#16834A;"));
    m_saveStatus->setText(QStringLiteral("已导出标准 commands.json"));
    QMessageBox::information(this, QStringLiteral("导出完成"),
                             QStringLiteral("指令模板已按 command-atlas-commands 格式导出。\n%1").arg(path));
}

void CommandDraftDialog::closeEvent(QCloseEvent *event)
{
    m_saveTimer->stop();
    updateCurrentDraft(false);
    QString error;
    if (!m_store.save(m_drafts, &error)) {
        QMessageBox::warning(this, QStringLiteral("草稿未能保存"),
                             QStringLiteral("关闭前保存失败，请确认磁盘可写后重试。\n%1").arg(error));
        event->ignore();
        return;
    }
    event->accept();
}
