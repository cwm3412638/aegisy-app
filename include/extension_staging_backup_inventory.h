#ifndef EXTENSION_STAGING_BACKUP_INVENTORY_H
#define EXTENSION_STAGING_BACKUP_INVENTORY_H

#include "configuration_backup_store.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// 扩展暂存备份的清点、验证删除与保留期规划层。暂存域钉住了 maxBackups（32），但在此之前
// 没有任何东西管理它：捕获只往里写，于是一份份备份无界堆积，而且没有任何有界的、诚实的
// 方式回答"这里到底有哪些备份"。本组件就是那个管理层。它的唯一写入目标是应用私有的加密
// 暂存备份存储（与 ToolManager 工具配置备份同一类写入），且写入只有一条路径——存储的
// 身份绑定验证删除；它不修改扩展来源树，不安装、不启用、不执行任何东西。当前没有任何产品
// 调用方。
//
// 清点不是证明。清单条目携带一个验证状态，而这个状态必须诚实：`ListedIntact` 的含义是
// **清单身份级**验证在读出时通过——清单字节有界读出、结构完整（格式/版本/算法/主体/备份 id
// 与目录名逐字节绑定/规范化时间戳/规范化 base64 字段齐备）、清单身份从读到的字节重算；
// 它**不**解密载荷，载荷字节的 GCM 认证留给恢复路径（`ConfigurationBackupStore::read` 加
// `ExtensionStagingSnapshot::verify`）。这个取舍是刻意的且固定不变：清点路径因此完全不触碰
// 密钥——密钥不可用时清点照常工作，而那正是最需要在场的时候。`ListedCorrupt` 是独立状态，
// 损坏条目永远留在清单里、绝不静默丢弃：一份被藏起来的损坏备份恰恰是回滚能力悄悄消失的
// 方式。存储本身的退化（根形状违例、锁不可用）传播为各自独立的结果状态（Invalid /
// Unavailable），绝不伪装成一份空清单。
//
// 一处与存储清点刻意的分歧：存储的 `inventory` 在所查主体作用域内的完整备份数超过
// maxBackups 时判定该主体 Invalid（混合主体根里别人主体的完整备份经完整验证后越出作用域，
// 既不占额度也不是错误；无法通过该验证的条目如实退化）。本层不继承这条——超限正是保留期
// 规划要修复的现实，看不到它就永远规划不了裁剪。
// 因此本层的扫描上限是 maxBackups 的 4 倍（128）：足够从任意超限状态规划回界内，同时保持
// 每一次清点读的总量有界。除此之外的根形状纪律（非备份 id 条目、符号链接、锁文件形状）
// 与存储逐字一致，违例即整体 Invalid。
//
// 验证删除只按精确的备份 id 走存储的 `removeVerified`：id 先按暂存域语法校验（畸形 id 在
// 任何存储工作之前拒绝），再经一次全主体清点取得该条目的主体与清单身份。id 畸形、id 不
// 存在、条目损坏（验证路径无法认证损坏清单，删除被拒绝且证据原地保留）、清点退化、存储
// 拒绝——五种结果各自独立，存储层的诊断逐字透传。删除只动指定 id 的那一份，其他主体的
// 备份绝不被触碰。
//
// 保留期规划只产出计划，绝不自动执行。语义固定如下：keep 集是该主体最新的 maxBackups 份
// ListedIntact 备份（新到旧，时间戳相同按 id 升序）；该主体最近一份 ListedIntact 备份
// **无条件保留**并记入 `newestVerifiedKept` 字段——保留它不是隐含的名单成员资格，而是一
// 条显式的、单独报告的决策；在这一构造下 keep 集永不超限，因此"即使超限也保留"不会把
// 主体推出界外，而若计算出的 keep 集竟不含它，规划失败关闭而不是产出一份会裁掉它的计划。
// 超出 keep 集的完整备份逐条列入 prune（原因 OverLimit）；该主体的每一份损坏备份也逐条
// 列入 prune（原因 Corrupt）——没有静默丢弃，但注意验证删除路径无法认证损坏清单，执行
// 时它们会被如实报告为拒绝删除，它们的物理清除需要一个尚未存在的证据处理决策。计划是纯
// 数据对象：规划期间存储零写入。`applyRetention` 只是逐条组合验证删除的便利入口，逐条
// 报告结果、绝不整体静默成败；损坏条目在 apply 里得到各自的 CorruptRefused 结果。
enum class ExtensionStagingBackupEntryVerification {
    // 清单身份级验证通过：结构完整且清单身份从读到的字节重算一致。不含载荷解密。
    ListedIntact,
    // 结构级损坏：目录形状违例、清单不可读或结构校验失败。条目保留在清单里。
    ListedCorrupt,
};

struct ExtensionStagingBackupListEntry {
    QString backupId;
    // 清单声称的主体。ListedIntact 时必然语法合法；ListedCorrupt 时仅在清单 JSON 可解析
    // 且主体字段语法合法时填写（未经认证，仅供归类），否则为空——为空者只出现在全主体
    // 清点里，绝不被悄悄丢掉。
    QString subject;
    // 仅在 ListedIntact 时有效：清单的规范化创建时间。
    QDateTime createdAt;
    // 从读到的清单字节重算的清单身份；清单字节不可读时为空。
    QString manifestIdentity;
    ExtensionStagingBackupEntryVerification verification =
        ExtensionStagingBackupEntryVerification::ListedCorrupt;
    // ListedCorrupt 时的结构级诊断（本层代号）。
    QString verificationIssue;
};

enum class ExtensionStagingBackupListState {
    Empty,
    Ready,
    Unavailable,
    Invalid,
};

struct ExtensionStagingBackupListResult {
    ExtensionStagingBackupListState state = ExtensionStagingBackupListState::Invalid;
    QList<ExtensionStagingBackupListEntry> entries;
    // Unavailable / Invalid 时的本层诊断。
    QString issue;
};

enum class ExtensionStagingBackupRemovalOutcome {
    Removed,
    // 参数本身不可用（空根、空输出、无密钥来源），存储未被触碰。
    RequestInvalid,
    // 备份 id 不合暂存域语法，在任何存储工作之前拒绝。
    IdMalformed,
    // 清点成功但该 id 不存在。
    NotFound,
    // 清点退化（Invalid/Unavailable），无法安全确认"是哪一份"，拒绝删除。
    ListingDegraded,
    // 条目存在但结构级损坏：验证删除路径无法认证它，拒绝删除并原地保留证据。
    CorruptRefused,
    // 存储的验证删除拒绝；diagnostic 逐字携带存储层代号。
    StoreFailed,
};

struct ExtensionStagingBackupRemovalResult {
    ExtensionStagingBackupRemovalOutcome outcome =
        ExtensionStagingBackupRemovalOutcome::StoreFailed;
    QString backupId;
    QString subject;
    QString diagnostic;
};

enum class ExtensionStagingPruneReason {
    // 完整备份超出 newest-first 保留集。
    OverLimit,
    // 结构级损坏的备份；每条单独列出，绝不静默丢弃。
    Corrupt,
};

struct ExtensionStagingRetentionPruneEntry {
    QString backupId;
    ExtensionStagingPruneReason reason = ExtensionStagingPruneReason::Corrupt;
    // 清单身份（已知时）：审计按它确认计划要裁的是哪一份。
    QString manifestIdentity;
};

struct ExtensionStagingRetentionPlan {
    QString subject;
    // 计划所对照的域上限（暂存域 maxBackups）。
    int maxBackups = 0;
    // 保留集，新到旧。
    QStringList keepBackupIds;
    QList<ExtensionStagingRetentionPruneEntry> prune;
    // 该主体最近一份 ListedIntact 备份：无条件保留的显式、单独报告的决策；该主体没有
    // 完整备份时为空。
    QString newestVerifiedKept;
};

struct ExtensionStagingRetentionApplyEntry {
    QString backupId;
    ExtensionStagingBackupRemovalOutcome outcome =
        ExtensionStagingBackupRemovalOutcome::StoreFailed;
    QString diagnostic;
};

class ExtensionStagingBackupInventory
{
public:
    // 清点暂存备份。`subject` 为空时全主体清点；非空时按暂存域主体语法先校验（畸形主体在
    // 任何存储工作之前以 `extension-staging-inventory-subject-invalid` 拒绝，结果对象保持
    // 为空）。作用域清点包含该主体的损坏条目（主体可从损坏清单里归类时）；主体无法归类的
    // 损坏条目只出现在全主体清点里。条目排序：时间戳有效的在前（新到旧，同刻按 id 升序），
    // 无有效时间戳的在后（按 id 升序）。返回 false 只表示请求本身无效；存储真相永远在
    // result 的 state 里。
    static bool list(const QString &backupRoot, const QString &subject,
                     ExtensionStagingBackupListResult *result, QString *error);

    // 按精确备份 id 经存储的验证删除路径移除一份暂存备份。id 语法先于任何存储工作；删除
    // 只动这一份，其他主体的备份绝不被触碰。
    static ExtensionStagingBackupRemovalResult removeVerified(
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        const QString &backupId);

    // 产出把一个主体裁回域上限之内的保留计划。纯数据、零写入：先按主体语法校验，再做一次
    // 该主体的只读清点；清点退化时以清点的原诊断失败关闭，绝不产出基于退化输入的计划。
    static bool planRetention(const QString &backupRoot, const QString &subject,
                              ExtensionStagingRetentionPlan *plan, QString *error);

    // 逐条执行计划：每条 prune 条目组合一次验证删除并逐条报告结果，绝不整体静默成败。
    static QList<ExtensionStagingRetentionApplyEntry> applyRetention(
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        const ExtensionStagingRetentionPlan &plan);
};

#endif // EXTENSION_STAGING_BACKUP_INVENTORY_H
