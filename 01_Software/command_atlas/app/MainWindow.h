#pragma once

#include "../core/CommandCatalog.h"
#include "../core/FrameBuilder.h"

#include <QHash>
#include <QMainWindow>
#include <QVariantMap>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QFormLayout;
class QPushButton;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void buildUi();
    void loadCatalog();
    void refreshResults();
    void showSelectedCommand();
    void clearParameterForm();
    void updatePreview();
    QVariantMap readParameters(bool *valid, QString *errorMessage) const;
    void copyHex();
    void copyAscii();
    void exportFrame();
    QString protocolName(const QString &protocolId) const;
    QString parameterSummary(const QVariantMap &parameters) const;

    CommandCatalog m_catalog;
    const CommandTemplate *m_currentCommand = nullptr;
    QHash<QString, QWidget *> m_parameterInputs;
    QByteArray m_currentFrame;
    bool m_previewValid = false;

    QComboBox *m_productSelector = nullptr;
    QComboBox *m_protocolSelector = nullptr;
    QLineEdit *m_searchInput = nullptr;
    QLabel *m_resultsCount = nullptr;
    QListWidget *m_commandList = nullptr;
    QLabel *m_commandTitle = nullptr;
    QLabel *m_commandMeta = nullptr;
    QLabel *m_commandDescription = nullptr;
    QLabel *m_sampleWarning = nullptr;
    QFormLayout *m_parameterForm = nullptr;
    QPlainTextEdit *m_framePreview = nullptr;
    QLabel *m_frameStatus = nullptr;
    QPushButton *m_copyHexButton = nullptr;
    QPushButton *m_copyAsciiButton = nullptr;
    QPushButton *m_exportButton = nullptr;
};
