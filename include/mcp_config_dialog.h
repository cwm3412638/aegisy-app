#ifndef MCP_CONFIG_DIALOG_H
#define MCP_CONFIG_DIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>

#include <functional>

class ConfigurationBackupKeyProvider;
class StatusBadge;

// MCP 服务器共享配置面板
// 读写 ~/.claude/settings.json 中的 mcpServers 字段
// 切换档案时该字段会被 Aegisy 的 merge-write 逻辑自动保留
class McpConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit McpConfigDialog(QWidget *parent = nullptr);

    // 生产接线：注入暂存备份密钥来源与备份根后，保存（对共享设置文件的整文档重写）
    // 先把当前文件字节捕获进应用私有的加密暂存备份域；备份失败即拒绝保存并给出原因，
    // 与激活路径"备份失败阻挡写入"的 fail-closed 先例一致。文件不存在（空来源）时
    // 没有可丢失的字节，诚实跳过捕获。未注入时保持接线前的保存行为（仅既有测试与
    // 未接线构造使用；产品调用方必须注入）。
    McpConfigDialog(ConfigurationBackupKeyProvider *stagingBackupKeyProvider,
                    const QString &stagingBackupRoot, QWidget *parent = nullptr);

private slots:
    void onAddServer();
    void onEditServer();
    void onRemoveServer();
    void onSave();
    void onSelectionChanged();

private:
    friend class McpConfigDialogTestAccess;

    void setupUi();
    void loadFromSettings();
    bool saveToSettings();
    void rebuildTable();

    static QString settingsFilePath();
    static bool writeSettingsFile(const QJsonObject &root);

    QTableWidget *m_table = nullptr;
    QPushButton  *m_addButton = nullptr;
    QPushButton  *m_editButton = nullptr;
    QPushButton  *m_removeButton = nullptr;
    QPushButton  *m_saveButton = nullptr;
    StatusBadge  *m_statusLabel = nullptr;

    QJsonObject  m_mcpServers;   // 当前编辑中的 mcpServers 对象
    QString      m_sourceIdentity;
    bool         m_sourceValid = false;

    // 暂存备份接线（生产路径由 MainWindow 注入）。保存前把当前设置文件字节捕获进
    // 加密暂存备份域；两者任一缺席即视为未接线，保持接线前行为。
    ConfigurationBackupKeyProvider *m_stagingBackupKeyProvider = nullptr;
    QString      m_stagingBackupRoot;
    // 最近一次保存失败的如实原因，供状态条与提示框展示。
    QString      m_lastSaveError;
    // 最近一次保存成功后的保留期修剪备注（空 = 未修剪：未接线或空来源诚实跳过捕获）。
    // 修剪失败绝不翻转保存结果，只在这里如实可见。
    QString      m_lastRetentionNote;
    // 测试钩子：捕获完成后、写入前身份复查之前调用，用于确定性构造"捕获与写入之间
    // 文件被换掉"的漂移场景。生产路径恒为空。
    std::function<void()> m_afterBackupCaptureHook;
};

#endif // MCP_CONFIG_DIALOG_H
