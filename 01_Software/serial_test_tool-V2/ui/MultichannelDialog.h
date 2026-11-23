#pragma once
#include "core/TestPlan.h"
#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QTableWidget;
class QLabel;
class QPushButton;

class MultichannelDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MultichannelDialog(QWidget *parent = nullptr);

    QList<CommandItem> steps() const { return m_steps; }

private:
    void preview();
    TestPlan::Parameters parameters() const;
    QLineEdit *m_model, *m_channels;
    QSpinBox *m_channelCount, *m_value;
    QComboBox *m_protocol, *m_function, *m_brightnessMax, *m_unit;
    QCheckBox *m_readback;
    QTableWidget *m_preview;
    QLabel *m_summary, *m_protocolNote;
    QPushButton *m_add;
    QList<CommandItem> m_steps;
};
