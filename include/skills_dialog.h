#ifndef SKILLS_DIALOG_H
#define SKILLS_DIALOG_H

#include <QDialog>
#include <QString>

#include <functional>

class ConfigurationBackupKeyProvider;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class SkillManager;

class SkillsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SkillsDialog(SkillManager *manager, QWidget *parent = nullptr);

    // 生产接线：注入暂存备份密钥来源与备份根后，删除非内置 Skill 前先把该 Skill 目录
    // （`SkillManager::skillsRoot()` 下的调用方权威目标根，与扩展清点同一来源）以
    // `skill:<id>` 为主体捕获进应用私有的加密暂存备份域；备份失败即拒绝删除并给出
    // 原因（fail-closed，与 MCP 保存先例逐字一致，内容仍在磁盘上）。内置 Skill 与
    // 不存在 Skill 的守卫先于一切备份工作——这两种情况本就不该产生备份。捕获与删除
    // 都成功后经共享唯一入口 `ExtensionStagingBackupRetention::pruneAfterCapture`
    // 修剪该主体；修剪失败绝不翻转删除结果，只如实记入备注。本对话框只产生备份，
    // 不提供任何恢复动作（skill 恢复资格尚未接线）。未注入时保持接线前行为（仅既有
    // 测试与未接线构造使用；产品调用方必须注入）。
    SkillsDialog(SkillManager *manager,
                 ConfigurationBackupKeyProvider *stagingBackupKeyProvider,
                 const QString &stagingBackupRoot, QWidget *parent = nullptr);

private slots:
    void rebuildTable();
    void rebuildCatalog();
    void onItemChanged(int row, int column);
    void onInstallUrl();
    void onImportDirectory();
    void onInstallCatalogSelected();
    void onDeleteSelected();
    void onOpenFolder();
    void onInstallPresentationRuntime();
    void updateSelection();

private:
    friend class SkillsDialogTestAccess;

    QString selectedSkillId() const;
    QString selectedCatalogSkillId() const;
    void filterCatalog(const QString &text);
    // 删除的内核（确认框之外的全部决定）：内置/不存在守卫先于备份，删除前捕获
    // fail-closed，删除成功后修剪。结果与如实原因落在 m_lastDeleteError /
    // m_lastDeleteBackupId / m_lastRetentionNote 上，由 onDeleteSelected 上屏。
    bool removeSkillWithBackup(const QString &id);

    SkillManager *m_manager = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_table = nullptr;
    QTableWidget *m_catalogTable = nullptr;
    QLineEdit *m_catalogSearch = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_details = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_installCatalogButton = nullptr;
    QPushButton *m_runtimeButton = nullptr;
    bool m_rebuilding = false;

    // 暂存备份接线（生产路径由 MainWindow 注入）。删除前把该 Skill 目录捕获进加密暂存
    // 备份域；两者任一缺席即视为未接线，保持接线前行为。
    ConfigurationBackupKeyProvider *m_stagingBackupKeyProvider = nullptr;
    QString m_stagingBackupRoot;
    // 最近一次删除失败的如实原因（捕获失败/删除失败各自如实）。
    QString m_lastDeleteError;
    // 最近一次删除前捕获的暂存备份 id（空 = 未捕获：未接线、内置或不存在守卫）。
    QString m_lastDeleteBackupId;
    // 最近一次删除成功后的保留期修剪备注（空 = 未修剪：未接线或未捕获）。
    // 修剪失败绝不翻转删除结果，只在这里如实可见。
    QString m_lastRetentionNote;
    // 测试钩子：捕获完成后、删除之前调用，用于确定性构造"捕获后修剪退化"场景。
    // 生产路径恒为空。
    std::function<void()> m_afterBackupCaptureHook;
};

#endif // SKILLS_DIALOG_H
