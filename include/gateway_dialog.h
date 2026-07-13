#ifndef GATEWAY_DIALOG_H
#define GATEWAY_DIALOG_H

#include <QDialog>

class GatewayManager;
class QLabel;
class QPushButton;
class QTableWidget;

class GatewayDialog : public QDialog
{
    Q_OBJECT

public:
    explicit GatewayDialog(GatewayManager *manager, QWidget *parent = nullptr);

private slots:
    void toggleGateway();
    void refreshState();
    void refreshLogs();
    void onGatewayError(const QString &error);

private:
    void setupUi();

    GatewayManager *m_manager;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_endpointLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_toggleButton = nullptr;
    QTableWidget *m_logTable = nullptr;
};

#endif // GATEWAY_DIALOG_H
