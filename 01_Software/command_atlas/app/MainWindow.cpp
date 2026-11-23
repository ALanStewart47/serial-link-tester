#include "MainWindow.h"

#include "../ui/CommandDraftDialog.h"

#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {

QString protocolLabel(const QString &protocolId)
{
    if (protocolId == QStringLiteral("new"))
        return QStringLiteral("新协议 · CA");
    if (protocolId == QStringLiteral("old"))
        return QStringLiteral("旧协议 · ASCII");
    if (protocolId == QStringLiteral("old_ext"))
        return QStringLiteral("旧扩展 · $ / @");
    return QStringLiteral("全部协议");
}

QLabel *makeSectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionLabel"));
    return label;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();

    loadCatalog();
    resize(1040, 680);
    setMinimumSize(900, 580);
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Command Atlas · 控制器指令助手"));

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("centralPanel"));
    auto *pageLayout = new QVBoxLayout(central);
    pageLayout->setContentsMargins(16, 12, 16, 12);
    pageLayout->setSpacing(10);

    auto *headerLayout = new QHBoxLayout;
    auto *brandLayout = new QVBoxLayout;
    auto *brand = new QLabel(QStringLiteral("Command Atlas"), central);
    brand->setObjectName(QStringLiteral("brandTitle"));
    auto *subtitle = new QLabel(QStringLiteral("控制器指令助手 · 查询、填参与组帧"), central);
    subtitle->setObjectName(QStringLiteral("mutedText"));
    brandLayout->setSpacing(2);
    brandLayout->addWidget(brand);
    brandLayout->addWidget(subtitle);
    headerLayout->addLayout(brandLayout);
    headerLayout->addStretch(1);
    auto *offlineBadge = new QLabel(QStringLiteral("本地示例库  ·  离线可用"), central);
    offlineBadge->setObjectName(QStringLiteral("statusBadge"));
    headerLayout->addWidget(offlineBadge, 0, Qt::AlignVCenter);
    auto *draftButton = new QPushButton(QStringLiteral("研发录入"), central);
    draftButton->setObjectName(QStringLiteral("secondaryButton"));
    headerLayout->addWidget(draftButton, 0, Qt::AlignVCenter);
    pageLayout->addLayout(headerLayout);

    auto *filterPanel = new QWidget(central);
    filterPanel->setObjectName(QStringLiteral("filterPanel"));
    auto *filterLayout = new QHBoxLayout(filterPanel);
    filterLayout->setContentsMargins(8, 6, 8, 6);
    filterLayout->setSpacing(8);

    m_productSelector = new QComboBox(filterPanel);
    m_productSelector->setObjectName(QStringLiteral("productSelector"));
    m_productSelector->setMinimumWidth(185);
    m_protocolSelector = new QComboBox(filterPanel);
    m_protocolSelector->setObjectName(QStringLiteral("protocolSelector"));
    m_protocolSelector->setMinimumWidth(130);
    m_searchInput = new QLineEdit(filterPanel);
    m_searchInput->setObjectName(QStringLiteral("searchInput"));
    m_searchInput->setPlaceholderText(QStringLiteral("搜索用途、名称、别名或关键词，例如：调亮、脉冲宽度"));
    m_searchInput->setClearButtonEnabled(true);
    filterLayout->addWidget(m_productSelector);
    filterLayout->addWidget(m_protocolSelector);
    filterLayout->addWidget(m_searchInput, 1);
    pageLayout->addWidget(filterPanel);

    auto *bodyLayout = new QHBoxLayout;
    bodyLayout->setSpacing(10);

    auto *resultsPanel = new QWidget(central);
    resultsPanel->setObjectName(QStringLiteral("resultsPanel"));
    resultsPanel->setMinimumWidth(240);
    resultsPanel->setMaximumWidth(300);
    auto *resultsLayout = new QVBoxLayout(resultsPanel);
    resultsLayout->setContentsMargins(10, 10, 10, 10);
    resultsLayout->setSpacing(6);
    auto *resultsHeader = new QHBoxLayout;
    resultsHeader->addWidget(makeSectionLabel(QStringLiteral("指令结果"), resultsPanel));
    resultsHeader->addStretch(1);
    m_resultsCount = new QLabel(resultsPanel);
    m_resultsCount->setObjectName(QStringLiteral("mutedText"));
    resultsHeader->addWidget(m_resultsCount);
    resultsLayout->addLayout(resultsHeader);
    m_commandList = new QListWidget(resultsPanel);
    m_commandList->setObjectName(QStringLiteral("commandList"));
    m_commandList->setSpacing(2);
    resultsLayout->addWidget(m_commandList, 1);
    bodyLayout->addWidget(resultsPanel);

    auto *detailPanel = new QWidget(central);
    detailPanel->setObjectName(QStringLiteral("detailPanel"));
    auto *detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(14, 12, 14, 12);
    detailLayout->setSpacing(8);

    m_commandTitle = new QLabel(QStringLiteral("请选择一条指令"), detailPanel);
    m_commandTitle->setObjectName(QStringLiteral("commandTitle"));
    m_commandTitle->setWordWrap(true);
    m_commandMeta = new QLabel(detailPanel);
    m_commandMeta->setObjectName(QStringLiteral("mutedText"));
    m_commandDescription = new QLabel(detailPanel);
    m_commandDescription->setObjectName(QStringLiteral("descriptionText"));
    m_commandDescription->setWordWrap(true);
    detailLayout->addWidget(m_commandTitle);
    detailLayout->addWidget(m_commandMeta);
    detailLayout->addWidget(m_commandDescription);

    m_sampleWarning = new QLabel(QStringLiteral("示例数据仅用于验证查询与组帧流程；请先核对实际控制器型号、固件与协议能力。此软件不会连接或发送指令。"), detailPanel);
    m_sampleWarning->setObjectName(QStringLiteral("warningBanner"));
    m_sampleWarning->setWordWrap(true);
    detailLayout->addWidget(m_sampleWarning);

    detailLayout->addWidget(makeSectionLabel(QStringLiteral("参数"), detailPanel));
    auto *parameterScroll = new QScrollArea(detailPanel);
    parameterScroll->setObjectName(QStringLiteral("parameterScroll"));
    parameterScroll->setWidgetResizable(true);
    parameterScroll->setFrameShape(QFrame::NoFrame);
    parameterScroll->viewport()->setObjectName(QStringLiteral("parameterViewport"));
    auto *parameterContent = new QWidget(parameterScroll);
    parameterContent->setObjectName(QStringLiteral("parameterContent"));
    m_parameterForm = new QFormLayout(parameterContent);
    m_parameterForm->setContentsMargins(0, 0, 8, 0);
    m_parameterForm->setHorizontalSpacing(12);
    m_parameterForm->setVerticalSpacing(6);
    m_parameterForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    parameterScroll->setWidget(parameterContent);
    detailLayout->addWidget(parameterScroll, 0);

    detailLayout->addWidget(makeSectionLabel(QStringLiteral("完整指令预览"), detailPanel));
    m_framePreview = new QPlainTextEdit(detailPanel);
    m_framePreview->setObjectName(QStringLiteral("framePreview"));
    m_framePreview->setReadOnly(true);
    m_framePreview->setMaximumBlockCount(8);
    m_framePreview->setMinimumHeight(92);
    detailLayout->addWidget(m_framePreview, 1);
    m_frameStatus = new QLabel(detailPanel);
    m_frameStatus->setObjectName(QStringLiteral("mutedText"));
    detailLayout->addWidget(m_frameStatus);

    auto *actionLayout = new QHBoxLayout;
    actionLayout->addStretch(1);
    m_copyHexButton = new QPushButton(QStringLiteral("复制 HEX"), detailPanel);
    m_copyHexButton->setObjectName(QStringLiteral("secondaryButton"));
    m_copyAsciiButton = new QPushButton(QStringLiteral("复制 ASCII"), detailPanel);
    m_copyAsciiButton->setObjectName(QStringLiteral("secondaryButton"));
    m_exportButton = new QPushButton(QStringLiteral("导出文本…"), detailPanel);
    m_exportButton->setObjectName(QStringLiteral("primaryButton"));
    actionLayout->addWidget(m_copyHexButton);
    actionLayout->addWidget(m_copyAsciiButton);
    actionLayout->addWidget(m_exportButton);
    detailLayout->addLayout(actionLayout);
    bodyLayout->addWidget(detailPanel, 1);
    pageLayout->addLayout(bodyLayout, 1);

    auto *footer = new QLabel(QStringLiteral("S2 本地研发草稿  ·  导出标准 JSON  ·  不连接控制器"), central);
    footer->setObjectName(QStringLiteral("mutedText"));
    pageLayout->addWidget(footer);
    setCentralWidget(central);

    connect(m_productSelector, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { refreshResults(); });
    connect(m_protocolSelector, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { refreshResults(); });
    connect(m_searchInput, &QLineEdit::textChanged, this,
            [this](const QString &) { refreshResults(); });
    connect(m_commandList, &QListWidget::currentRowChanged, this,
            [this](int) { showSelectedCommand(); });
    connect(m_copyHexButton, &QPushButton::clicked, this, &MainWindow::copyHex);
    connect(m_copyAsciiButton, &QPushButton::clicked, this, &MainWindow::copyAscii);
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::exportFrame);
    connect(draftButton, &QPushButton::clicked, this, [this]() {
        CommandDraftDialog dialog(m_catalog, this);
        dialog.exec();
    });
}

void MainWindow::loadCatalog()
{
    QString errorMessage;
    if (!m_catalog.load(&errorMessage)) {
        QMessageBox::critical(this, QStringLiteral("无法载入指令库"), errorMessage);
        statusBar()->showMessage(errorMessage);
        m_copyHexButton->setEnabled(false);
        m_copyAsciiButton->setEnabled(false);
        m_exportButton->setEnabled(false);
        return;
    }

    for (const ProductProfile &product : m_catalog.products())
        m_productSelector->addItem(product.name, product.id);
    m_protocolSelector->addItem(protocolLabel(QStringLiteral("all")), QStringLiteral("all"));
    m_protocolSelector->addItem(protocolLabel(QStringLiteral("new")), QStringLiteral("new"));
    m_protocolSelector->addItem(protocolLabel(QStringLiteral("old")), QStringLiteral("old"));
    m_protocolSelector->addItem(protocolLabel(QStringLiteral("old_ext")), QStringLiteral("old_ext"));
    refreshResults();
    statusBar()->showMessage(QStringLiteral("已载入 %1 条示例指令，可离线查询。")
                                 .arg(m_catalog.commands().size()));
}

void MainWindow::refreshResults()
{
    if (!m_commandList)
        return;

    const QString productId = m_productSelector->currentData().toString();
    const QString protocolId = m_protocolSelector->currentData().toString();
    const QVector<const CommandTemplate *> matches = m_catalog.search(m_searchInput->text(), productId, protocolId);
    m_commandList->clear();
    for (const CommandTemplate *command : matches) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1\n%2  ·  %3").arg(command->title, command->category, protocolName(command->protocolId)),
            m_commandList);
        item->setData(Qt::UserRole, command->id);
        item->setToolTip(command->description);
        item->setSizeHint(QSize(0, 44));
    }
    m_resultsCount->setText(QStringLiteral("%1 条").arg(matches.size()));

    if (!matches.isEmpty()) {
        m_commandList->setCurrentRow(0);
    } else {
        m_currentCommand = nullptr;
        m_commandTitle->setText(QStringLiteral("没有找到匹配的指令"));
        m_commandMeta->clear();
        m_commandDescription->setText(QStringLiteral("试试更短的用途词，例如“亮度”或“脉宽”。"));
        clearParameterForm();
        m_framePreview->clear();
        m_frameStatus->clear();
        m_previewValid = false;
        m_copyHexButton->setEnabled(false);
        m_copyAsciiButton->setEnabled(false);
        m_exportButton->setEnabled(false);
    }
}

void MainWindow::showSelectedCommand()
{
    const QListWidgetItem *item = m_commandList->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    m_currentCommand = nullptr;
    for (const CommandTemplate &command : m_catalog.commands()) {
        if (command.id == id) {
            m_currentCommand = &command;
            break;
        }
    }
    if (!m_currentCommand)
        return;

    m_commandTitle->setText(m_currentCommand->title);
    const ProductProfile *product = m_catalog.productById(m_currentCommand->productIds.value(0));
    m_commandMeta->setText(QStringLiteral("%1  ·  %2  ·  %3")
                               .arg(product ? product->name : QStringLiteral("未知产品"),
                                    protocolName(m_currentCommand->protocolId), m_currentCommand->category));
    m_commandDescription->setText(m_currentCommand->description);
    clearParameterForm();

    for (const QJsonValue &parameterValue : m_currentCommand->parameters) {
        const QJsonObject definition = parameterValue.toObject();
        const QString parameterId = definition.value(QStringLiteral("id")).toString();
        const QString type = definition.value(QStringLiteral("type")).toString();
        const QString label = definition.value(QStringLiteral("label")).toString();
        QWidget *editor = nullptr;

        if (type == QStringLiteral("integer") || type == QStringLiteral("ascii_number")) {
            auto *spinBox = new QSpinBox(this);
            spinBox->setRange(definition.value(QStringLiteral("min")).toInt(),
                              definition.value(QStringLiteral("max")).toInt());
            spinBox->setValue(definition.value(QStringLiteral("default")).toInt());
            spinBox->setKeyboardTracking(false);
            editor = spinBox;
            connect(spinBox, qOverload<int>(&QSpinBox::valueChanged), this,
                    [this](int) { updatePreview(); });
        } else if (type == QStringLiteral("enum")) {
            auto *comboBox = new QComboBox(this);
            for (const QJsonValue &optionValue : definition.value(QStringLiteral("options")).toArray()) {
                const QJsonObject option = optionValue.toObject();
                comboBox->addItem(option.value(QStringLiteral("label")).toString(),
                                  option.value(QStringLiteral("value")).toVariant());
            }
            const QVariant defaultValue = definition.value(QStringLiteral("default")).toVariant();
            comboBox->setCurrentIndex(comboBox->findData(defaultValue));
            editor = comboBox;
            connect(comboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this](int) { updatePreview(); });
        } else {
            auto *lineEdit = new QLineEdit(this);
            const int width = definition.value(QStringLiteral("width")).toInt();
            if (width > 0)
                lineEdit->setMaxLength(width);
            lineEdit->setText(definition.value(QStringLiteral("default")).toString());
            editor = lineEdit;
            connect(lineEdit, &QLineEdit::textChanged, this,
                    [this](const QString &) { updatePreview(); });
        }

        QString fieldLabel = label;
        const QString unit = definition.value(QStringLiteral("unit")).toString();
        if (!unit.isEmpty())
            fieldLabel += QStringLiteral("（%1）").arg(unit);
        auto *labelWidget = new QLabel(fieldLabel, this);
        labelWidget->setObjectName(QStringLiteral("parameterLabel"));
        labelWidget->setBuddy(editor);
        m_parameterForm->addRow(labelWidget, editor);
        const QString help = definition.value(QStringLiteral("help")).toString();
        if (!help.isEmpty()) {
            auto *helpLabel = new QLabel(help, this);
            helpLabel->setObjectName(QStringLiteral("mutedText"));
            helpLabel->setWordWrap(true);
            m_parameterForm->addRow(QString(), helpLabel);
        }
        m_parameterInputs.insert(parameterId, editor);
    }
    updatePreview();
}

void MainWindow::clearParameterForm()
{
    m_parameterInputs.clear();
    while (QLayoutItem *item = m_parameterForm->takeAt(0)) {
        if (QWidget *widget = item->widget())
            delete widget;
        delete item;
    }
}

QVariantMap MainWindow::readParameters(bool *valid, QString *errorMessage) const
{
    QVariantMap values;
    *valid = true;
    if (!m_currentCommand)
        return values;

    for (const QJsonValue &parameterValue : m_currentCommand->parameters) {
        const QJsonObject definition = parameterValue.toObject();
        const QString id = definition.value(QStringLiteral("id")).toString();
        QWidget *editor = m_parameterInputs.value(id, nullptr);
        if (!editor) {
            *valid = false;
            *errorMessage = QStringLiteral("参数控件缺失：%1").arg(definition.value(QStringLiteral("label")).toString());
            return {};
        }
        if (auto *spinBox = qobject_cast<QSpinBox *>(editor))
            values.insert(id, spinBox->value());
        else if (auto *comboBox = qobject_cast<QComboBox *>(editor))
            values.insert(id, comboBox->currentData());
        else if (auto *lineEdit = qobject_cast<QLineEdit *>(editor))
            values.insert(id, lineEdit->text());
    }
    return values;
}

void MainWindow::updatePreview()
{
    if (!m_currentCommand)
        return;
    bool parametersValid = false;
    QString errorMessage;
    const QVariantMap parameters = readParameters(&parametersValid, &errorMessage);
    if (!parametersValid) {
        m_previewValid = false;
        m_framePreview->setPlainText(errorMessage);
        m_frameStatus->setText(QStringLiteral("请修正参数后再复制或导出。"));
        m_copyHexButton->setEnabled(false);
        m_copyAsciiButton->setEnabled(false);
        m_exportButton->setEnabled(false);
        return;
    }

    const FrameBuildResult result = FrameBuilder::build(*m_currentCommand, parameters);
    if (!result.ok) {
        m_previewValid = false;
        m_framePreview->setPlainText(result.errorMessage);
        m_frameStatus->setText(QStringLiteral("此参数组合无法组帧。"));
        m_copyHexButton->setEnabled(false);
        m_copyAsciiButton->setEnabled(false);
        m_exportButton->setEnabled(false);
        return;
    }

    m_currentFrame = result.bytes;
    m_previewValid = true;
    const QString hex = FrameBuilder::hexText(result.bytes);
    const QString ascii = FrameBuilder::escapedAscii(result.bytes);
    m_framePreview->setPlainText(QStringLiteral("HEX    %1\nASCII  %2").arg(hex, ascii));
    if (m_currentCommand->frameFormat == QStringLiteral("hex") && !result.bytes.isEmpty()) {
        const quint8 bcc = static_cast<quint8>(static_cast<unsigned char>(result.bytes.back()));
        m_frameStatus->setText(QStringLiteral("XOR BCC = 0x%1（对前 %2 字节逐字节异或）")
                                   .arg(bcc, 2, 16, QLatin1Char('0')).toUpper()
                                   .arg(result.bytes.size() - 1));
    } else {
        m_frameStatus->setText(QStringLiteral("ASCII 文本命令；此模板不使用 BCC。"));
    }
    m_copyHexButton->setEnabled(true);
    m_copyAsciiButton->setEnabled(true);
    m_exportButton->setEnabled(true);
}

void MainWindow::copyHex()
{
    if (!m_previewValid)
        return;
    QGuiApplication::clipboard()->setText(FrameBuilder::hexText(m_currentFrame));
    statusBar()->showMessage(QStringLiteral("HEX 指令已复制到剪贴板"), 3000);
}

void MainWindow::copyAscii()
{
    if (!m_previewValid || !m_currentCommand)
        return;
    const QString text = m_currentCommand->frameFormat == QStringLiteral("ascii")
        ? QString::fromLatin1(m_currentFrame) : FrameBuilder::escapedAscii(m_currentFrame);
    QGuiApplication::clipboard()->setText(text);
    statusBar()->showMessage(QStringLiteral("ASCII / 转义文本已复制到剪贴板"), 3000);
}

QString MainWindow::parameterSummary(const QVariantMap &parameters) const
{
    if (!m_currentCommand || m_currentCommand->parameters.isEmpty())
        return QStringLiteral("无可调参数");
    QStringList entries;
    for (const QJsonValue &value : m_currentCommand->parameters) {
        const QJsonObject definition = value.toObject();
        const QString id = definition.value(QStringLiteral("id")).toString();
        QString displayValue = parameters.value(id).toString();
        if (definition.value(QStringLiteral("encoding")).toString() == QStringLiteral("ascii_channel")) {
            bool ok = false;
            const int channel = parameters.value(id).toInt(&ok);
            if (ok && channel >= 1 && channel <= 26)
                displayValue = QStringLiteral("%1（%2）").arg(channel).arg(QChar('A' + channel - 1));
        }
        entries.append(QStringLiteral("%1：%2")
                           .arg(definition.value(QStringLiteral("label")).toString(), displayValue));
    }
    return entries.join(QStringLiteral("；"));
}

void MainWindow::exportFrame()
{
    if (!m_previewValid || !m_currentCommand)
        return;
    const QString filePath = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出指令文本"),
        QStringLiteral("%1.txt").arg(m_currentCommand->id),
        QStringLiteral("文本文件 (*.txt)"));
    if (filePath.isEmpty())
        return;

    bool parametersValid = false;
    QString errorMessage;
    const QVariantMap parameters = readParameters(&parametersValid, &errorMessage);
    if (!parametersValid) {
        QMessageBox::warning(this, QStringLiteral("无法导出"), errorMessage);
        return;
    }
    const QString ascii = m_currentCommand->frameFormat == QStringLiteral("ascii")
        ? QString::fromLatin1(m_currentFrame) : FrameBuilder::escapedAscii(m_currentFrame);
    const QString document = QStringLiteral(
        "Command Atlas 指令导出\n"
        "指令：%1\n"
        "ID：%2\n"
        "协议：%3\n"
        "参数：%4\n"
        "HEX：%5\n"
        "ASCII：%6\n"
        "\n注意：请在使用前核对实际产品型号、固件版本与协议能力。\n")
        .arg(m_currentCommand->title, m_currentCommand->id, protocolName(m_currentCommand->protocolId),
             parameterSummary(parameters), FrameBuilder::hexText(m_currentFrame), ascii);
    QSaveFile file(filePath);
    const QByteArray bytes = document.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    statusBar()->showMessage(QStringLiteral("已导出：%1").arg(filePath), 4000);
}

QString MainWindow::protocolName(const QString &protocolId) const
{
    return protocolLabel(protocolId);
}
