#pragma once
#include "core/DiagnosticCommands.h"
#include <QDialog>
class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QTableWidget;
class QLabel;
class QPushButton;

class CommandSelectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit CommandSelectionDialog(QWidget *parent=nullptr, const QString &libraryPath=Commands::Library::defaultPath());
    Commands::Profile profile() const;
    QList<Commands::Command> selectedCommands() const;
    QList<Commands::Command> fallbackCommands() const { return m_library.eligible(profile()); }
    QJsonObject preparedPlan() const;
    bool importPath(const QString &path, QString *error);
private:
    void rebuild();
    void updateSelection();
    void setAll(bool checked);
    Commands::Library m_library;
    QString m_libraryPath;
    QLineEdit *m_model;
    QSpinBox *m_channels;
    QComboBox *m_protocol;
    QCheckBox *m_confirmed;
    QList<QCheckBox *> m_groups;
    QTableWidget *m_table;
    QLabel *m_summary, *m_notice;
    QPushButton *m_accept;
};
