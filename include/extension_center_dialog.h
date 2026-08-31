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
    QList<QPushButton *> m_reviewButtons;
    QList<QPushButton *> m_enablementButtons;
    QList<QPushButton *> m_removalButtons;
    QList<QPushButton *> m_updateButtons;
    ExtensionReviewLedgerStoreResult m_ledger;
    ExtensionEnablementLedgerStoreResult m_grants;
    QList<ReviewRow> m_rows;
    bool m_reviewBusy = false;
    bool m_enablementBusy = false;
    bool m_removalBusy = false;
    bool m_importBusy = false;
    bool m_updateBusy = false;
};

#endif // EXTENSION_CENTER_DIALOG_H
