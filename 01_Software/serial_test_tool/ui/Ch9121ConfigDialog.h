#pragma once
#include "core/Ch9121ConfigClient.h"
#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

class Ch9121ConfigDialog : public QDialog
{
    Q_OBJECT
public:
    Ch9121ConfigDialog(Ch9121ConfigClient *client, const Ch9121Config::DeviceSummary &device,
                       QWidget *parent = nullptr);

protected:
    void reject() override;

private:
    void load(const Ch9121Config::DeviceConfig &config);
    void read();
    void write();
    void updateControls();
    Ch9121ConfigClient *m_client;
    QByteArray m_mac;
    std::optional<Ch9121Config::DeviceConfig> m_baseline;
    QWidget *m_form;
    QLineEdit *m_name, *m_ip, *m_mask, *m_gateway;
    QCheckBox *m_dhcp;
    QComboBox *m_baud, *m_data, *m_stop, *m_parity;
    QLabel *m_version, *m_status;
    QPushButton *m_read, *m_write, *m_close;
};
