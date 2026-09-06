#ifndef EXTENSION_CENTER_DIALOG_H
#define EXTENSION_CENTER_DIALOG_H

#include "extension_enablement_ledger_store.h"
#include "extension_enablement_presentation.h"
#include "extension_enablement_workflow.h"
#include "extension_import_presentation.h"
#include "extension_lifecycle_presentation.h"
#include "extension_registry.h"
#include "extension_review_ledger_store.h"
#include "extension_review_presentation.h"
#include "extension_review_workflow.h"
#include "extension_staging_backup_inventory.h"
#include "extension_staging_restore_audit_ledger_store.h"
#include "extension_staging_restore_flow.h"
#include "extension_update_presentation.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QAbstractButton;
class QPushButton;

class ExtensionCenterDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExtensionCenterDialog(
        const QList<ExtensionRegistryRecord> &records,
        const QStringList &sourceIssueCodes = {},
        const ExtensionReviewLedgerStoreResult &ledger = {},
        const ExtensionEnablementLedgerStoreResult &grants = {},
        QWidget *parent = nullptr);

    void setReviewSnapshot(const QList<ExtensionRegistryRecord> &records,
                           const QStringList &sourceIssueCodes,
                           const ExtensionReviewLedgerStoreResult &ledger);
    void setReviewBusy(bool busy);
    void showReviewError(const QString &errorCode);

    void setEnablementSnapshot(const QList<ExtensionRegistryRecord> &records,
                               const QStringList &sourceIssueCodes,
                               const ExtensionEnablementLedgerStoreResult &grants);
    void setEnablementBusy(bool busy);
    void showEnablementError(const QString &errorCode);

    // 移除同时改动两份账本，因此它的快照同时替换两者。这与复核/授权刷新只带自己那一半
    // 相反，而那正是因为移除确实读过并写过两份账本。
    void setRemovalSnapshot(const QList<ExtensionRegistryRecord> &records,
                            const QStringList &sourceIssueCodes,
                            const ExtensionReviewLedgerStoreResult &ledger,
                            const ExtensionEnablementLedgerStoreResult &grants);
    void setRemovalBusy(bool busy);
    void showRemovalError(const QString &errorCode);

    // 披露一个扩展包的内容。这里没有对应的提交入口：没有任何东西可以提交，因为披露不
    // 导入。每一次披露都完整替换上一次的组件列表，绝不保留上一次的行——留着上一次的组件
    // 会让一次失败的读取看起来在描述这一次选的那个包。
    void setImportDisclosure(const ExtensionImportDisclosure &disclosure);
    void setImportBusy(bool busy);

    // 披露一次更新能不能成立。这里同样没有对应的提交入口：当前没有任何一次更新可以成立，
    // 而即使有一天可以，暂存也仍然要经过人工复核与重新授权，因此界面不提供一个"就在这里
    // 点一下完成更新"的动作。
    void setUpdatePlan(const ExtensionUpdatePlan &plan);
    void setUpdateBusy(bool busy);

    // 暂存备份浏览区。动作入口是封闭的、按资格缺席的：只有
    // `ExtensionStagingRestoreFlow::isRestoreOffered` 判定合格（清单身份级验证通过且主体
    // 是 mcp:claude-settings）且调用方声明恢复目标可解析（restoreDestinationResolved）
    // 的行才出现"恢复"按钮；其余行连按钮都不渲染——在场但灰着的按钮暗示"本来可以"，而
    // 真相是那些行不存在恢复入口（授权按钮先例）。这里没有删除、没有裁剪、没有立即捕获
    // 按钮：那些触发器仍然不存在。渲染规则与上面各区相同：每一次清单完整替换上一次，损坏
    // 条目永远可见并标注，存储退化冻结成一条明确的非空消息，绝不伪装成空清单。
    void setBackupListing(const ExtensionStagingBackupListResult &listing,
                          bool restoreDestinationResolved);
    void setBackupBusy(bool busy);
    void showBackupError(const QString &errorCode);

    // 恢复工作流的界面半边。准备与提交都在 MainWindow 的 tracked worker 里跑，本对话框
    // 只渲染、提问与如实报告：
    // - setRestoreBusy 冻结/解冻全部恢复入口（一次只进行一个恢复）；
    // - showRestoreError 按准备阶段给出各自的诚实文案（捕获失败、清单退化、备份消失、
    //   读回失败、目标不可解析……诊断逐字透传，固定代码正则门控后才上屏）；
    // - showRestoreRefusal 呈现计划层的拒绝（含目标冲突：此时当前内容已被捕获为新备份，
    //   文案如实说出这一点）；
    // - askRestoreDecision 把准备结果渲染成完整披露（主体、备份 id、目标目录、完整计划
    //   身份与树身份、统计、有界清单、绑定声明、警告、共享文件覆盖说明、执行前备份行、
    //   固定执行披露），PlainText、复选框默认未勾选门控 OK；取消与关窗都算 Decline
    //   （同样会被记录），返回 true 仅当明确确认；
    // - showRestoreResult 如实报告结果：declined 已记录、记录失败冻结、Complete /
    //   Partial（必须说"混合状态"并指名恢复前备份为回退路径）/ Refused / NotStarted。
    void setRestoreBusy(bool busy);
    void showRestoreError(const QString &stage, const QString &errorCode);
    void showRestoreRefusal(const QString &refusalCode);
    bool askRestoreDecision(
        const ExtensionStagingRestorePreparation &preparation,
        ExtensionStagingRestoreApprovalAcknowledgement *acknowledgement);
    void showRestoreResult(
        const ExtensionStagingRestoreOutcome &outcome,
        const ExtensionStagingRestorePreparation &preparation);

    // 恢复审计轨迹只读视图。它回答的是另一个问题："恢复的决定与执行结果被记录了什么"。
    // 渲染规则与备份浏览区相同，且更严：这里连一个按钮都没有——它是轨迹，不是控制台。
    // - 每一次读取完整替换上一次，绝不保留上一次的行；
    // - 退化（Invalid/Unavailable/OutcomeUnknown）冻结成明确的非空消息，绝不伪装成
    //   "没有记录"；MAC 认证失败落到 Invalid，而不是被过滤掉的行；
    // - "没有记录"只在两个已区分的状态说出：从未建立（Empty）与已认证的空（Ready 且
    //   零条目），两者措辞不同；
    // - 已批准但尚无结果条目的决定如实显示"批准已记录，尚无执行记录"，绝不暗示执行
    //   已成功；Partial 必须说出混合状态并指名恢复前备份 id 作为回退路径；
    // - 渲染有界：最多上屏固定数量的最近决定，超出以显式截断标记交代，审计链本身
    //   完整保留。
    void setRestoreAuditTrail(const ExtensionStagingRestoreAuditStoreResult &result);
    void setRestoreAuditBusy(bool busy);

signals:
    void reviewRequested(const ExtensionReviewRequest &request);
    void enablementRequested(const ExtensionEnablementRequest &request);
    // 移除只需要 (kind, id)：被移除的内容摘要可能已经不可读，而移除必须仍然能收回它
    // 留下的授权。
    void removalRequested(ExtensionKind kind, const QString &id);
    // 只请求一次披露。这里没有任何"请求导入"的对应信号：在权限、审批、沙箱与恢复门禁完成
    // 之前没有任何东西可以被导入，而一个发不出去的请求比一个能发出去的请求安全。
    void bundleDisclosureRequested();
    // 请求为某一个已在列的扩展检查一份候选包。带上 (kind, id) 是因为候选必须描述同一个
    // 扩展，而那件事只能由产出层用磁盘上的清单去核对。
    void updatePlanRequested(ExtensionKind kind, const QString &id);
    // 请求为某一份已在列的备份发起恢复。只带 (backupId, subject)：资格判定在编排器里
    // 复核，按钮在场只是渲染结果，绝不构成信任输入。
    void restoreRequested(const QString &backupId, const QString &subject);

private slots:
    void applyFilter();
    void reviewRow(int row);
    void enablementRow(int row);
    void removalRow(int row);
    void updateRow(int row);

private:
    void populate(const QList<ExtensionRegistryRecord> &records,
                  const QStringList &sourceIssueCodes,
                  const ExtensionReviewLedgerStoreResult &ledger,
                  const ExtensionEnablementLedgerStoreResult &grants);
    bool confirmPrompt(const ExtensionReviewPrompt &prompt,
                       ExtensionReviewAction action);
    bool confirmRevoke(const ExtensionReviewPin &pin);
    // 授权确认与复核确认问的是两个不同的问题：复核问"有人看过这份内容吗"，授权问"你要让
    // 这份内容运行吗"。因此它有独立的确认文本，并且必须把"这次授权当前不会让任何东西
    // 运行"写在人能看到的地方。
    bool confirmEnablementPrompt(const ExtensionEnablementPrompt &prompt);
    bool confirmEnablementRevocation(const ExtensionRevocationPrompt &prompt);
    // 授权动作的可点击性只能来自呈现层的判定，不能由这里另算一遍：另算一遍必然会与那
    // 一层漂移，而漂移的方向是给一份没人复核过的内容提供授权按钮，那份授权会以已认证的
    // 形式留在账本里，等复核出现的那一刻自动生效。
    ExtensionEnablementPrompt enablementPromptFor(int row) const;
    // 移除确认必须说清楚这次收回了哪几半，并且必须说明磁盘上的内容没有被删除：写"删除
    // 扩展"会让人以为内容已经消失而停止清理，而内容还在原处。
    bool confirmRemoval(const ExtensionRemovalPlan &plan);
    ExtensionRemovalPlan removalPlanFor(int row) const;
    void rebuildRows();

    struct ReviewRow {
        ExtensionRegistryRecord record;
        ExtensionReviewPin pin;
        ExtensionEnablementGrant grant;
        bool hasRecord = false;
        bool hasPin = false;
        bool hasGrant = false;
    };

    QList<ExtensionRegistryRecord> m_records;
    QLineEdit *m_search = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_reviewStatus = nullptr;
    QLabel *m_enablementStatus = nullptr;
    QLabel *m_removalStatus = nullptr;
    QLabel *m_importStatus = nullptr;
    QTableWidget *m_importTable = nullptr;
    QPushButton *m_importButton = nullptr;
    QLabel *m_updateStatus = nullptr;
    QTableWidget *m_updateTable = nullptr;
    QLabel *m_backupStatus = nullptr;
    QTableWidget *m_backupTable = nullptr;
    QLabel *m_restoreStatus = nullptr;
    QLabel *m_restoreAuditStatus = nullptr;
    QTableWidget *m_restoreAuditTable = nullptr;
    QList<QPushButton *> m_reviewButtons;
    QList<QPushButton *> m_enablementButtons;
    QList<QPushButton *> m_removalButtons;
    QList<QPushButton *> m_updateButtons;
    QList<QPushButton *> m_restoreButtons;
    // 当前渲染的备份清单条目（与 m_backupTable 行一一对应）：恢复按钮点击时按行号取回
    // (backupId, subject)，绝不从单元格文本反解。
    QList<ExtensionStagingBackupListEntry> m_backupEntries;
    ExtensionReviewLedgerStoreResult m_ledger;
    ExtensionEnablementLedgerStoreResult m_grants;
    QList<ReviewRow> m_rows;
    bool m_reviewBusy = false;
    bool m_enablementBusy = false;
    bool m_removalBusy = false;
    bool m_importBusy = false;
    bool m_updateBusy = false;
    bool m_backupBusy = false;
    bool m_restoreBusy = false;
    bool m_restoreAuditBusy = false;
    // 调用方声明的恢复目标可解析性：设置路径非空且其父目录存在。为假时连合格行也不渲染
    // 恢复按钮——恢复不可能生效的地方不得出现恢复入口。
    bool m_restoreDestinationResolved = false;
};

#endif // EXTENSION_CENTER_DIALOG_H
