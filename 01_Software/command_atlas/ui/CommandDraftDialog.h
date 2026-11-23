#pragma once

#include "../core/CommandCatalog.h"
#include "../core/CommandDraftStore.h"

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QVector>

class QComboBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QTimer;

class CommandDraftDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit CommandDraftDialog(const CommandCatalog &catalog, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    void loadDrafts();
    void newDraft();
    void duplicateCurrentDraft();
    void selectDraft(int row);
    void loadDraftIntoForm(const QJsonObject &draft);
    QJsonObject collectDraft() const;
    void updateCurrentDraft();
    void updateCurrentDraft(bool scheduleSave);
    void persistDrafts();
    void addParameter();
    void editSelectedParameter();
    void duplicateSelectedParameter();
    void removeSelectedParameter();
    void addFixedSegment();
    void addParameterSegment();
    void addBccSegment();
    void editSelectedSegment();
    void removeSelectedSegment();
    void moveSelectedSegment(int direction);
    void refreshPreview();
    void exportCurrentDraft();
    void importDrafts();
    void updateDraftList();
    QJsonArray parametersFromTable(QString *errorMessage) const;
    QJsonArray segmentsFromTable(QString *errorMessage) const;

    const CommandCatalog &m_catalog;
    CommandDraftStore m_store;
    QVector<QJsonObject> m_drafts;
    QString m_currentId;
    QJsonObject m_loadedDraft;
    QHash<QString, QString> m_loadedText;
    bool m_loading = false;

    QListWidget *m_draftList = nullptr;
    QComboBox *m_productInput = nullptr;
    QComboBox *m_protocolInput = nullptr;
    QLineEdit *m_categoryInput = nullptr;
    QLineEdit *m_titleInput = nullptr;
    QPlainTextEdit *m_descriptionInput = nullptr;
    QPlainTextEdit *m_aliasesInput = nullptr;
    QPlainTextEdit *m_keywordsInput = nullptr;
    QTableWidget *m_parameterTable = nullptr;
    QTableWidget *m_segmentTable = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QTextEdit *m_validationMessage = nullptr;
    QLabel *m_saveStatus = nullptr;
    QPushButton *m_exportButton = nullptr;
    QTimer *m_saveTimer = nullptr;
};
