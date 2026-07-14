#ifndef MCP_CONFIG_DIALOG_H
#define MCP_CONFIG_DIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>

class StatusBadge;

// MCP 服务器共享配置面板
// 读写 ~/.claude/settings.json 中的 mcpServers 字段
// 切换档案时该字段会被 Aegisy 的 merge-write 逻辑自动保留
class McpConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit McpConfigDialog(QWidget *parent = nullptr);

private slots:
    void onAddServer();
    void onEditServer();
    void onRemoveServer();
    void onSave();
    void onSelectionChanged();

private:
    void setupUi();
    void loadFromSettings();
    bool saveToSettings();
    void rebuildTable();

    static QString settingsFilePath();
    static QJsonObject readSettingsFile();
    static bool writeSettingsFile(const QJsonObject &root);

    QTableWidget *m_table = nullptr;
    QPushButton  *m_addButton = nullptr;
    QPushButton  *m_editButton = nullptr;
    QPushButton  *m_removeButton = nullptr;
    QPushButton  *m_saveButton = nullptr;
    StatusBadge  *m_statusLabel = nullptr;

    QJsonObject  m_mcpServers;   // 当前编辑中的 mcpServers 对象
};

#endif // MCP_CONFIG_DIALOG_H
