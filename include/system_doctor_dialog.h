#ifndef SYSTEM_DOCTOR_DIALOG_H
#define SYSTEM_DOCTOR_DIALOG_H

#include <QDialog>
#include <QList>
#include <QMap>

#include "tool_manager.h"

class QLabel;
class QPushButton;
class QTableWidget;
class StatusBadge;

class SystemDoctorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SystemDoctorDialog(ToolManager *toolManager, QWidget *parent = nullptr);

private slots:
    void refreshReport();
    void onInstallFinished(AiTool tool, int requestId, bool success);
    void onInstallOutput(AiTool tool, const QString &line);

private:
    void setupUi();
    void applyReport(const QList<RuntimeStatus> &runtimes,
                     const QMap<int, ToolStatus> &tools,
                     bool secureStorageAvailable,
                     const QString &dataDirectory,
                     bool dataDirectoryWritable);
    void addRow(const QString &category,
                const QString &name,
                const QString &status,
                const QString &detail,
                const QString &tone,
                AiTool *actionTool = nullptr,
                bool installed = false,
                const QString &passiveAction = QString(),
                bool repair = false);
    void installOrUpdate(AiTool tool, bool installed, bool repair = false);

    ToolManager *m_toolManager;
    QTableWidget *m_table = nullptr;
    StatusBadge *m_summaryLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    bool m_scanning = false;
};

#endif // SYSTEM_DOCTOR_DIALOG_H
