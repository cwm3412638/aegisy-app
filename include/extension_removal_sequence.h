#ifndef EXTENSION_REMOVAL_SEQUENCE_H
#define EXTENSION_REMOVAL_SEQUENCE_H

#include "extension_enablement_workflow.h"
#include "extension_review_workflow.h"
#include "extension_scope_policy.h"
#include "extension_tree_capture.h"

#include <QString>
#include <QStringList>
#include <QVector>

// 扩展移除计划契约：一次"移除这个扩展"的人工请求，究竟会变成哪几步、每一步的精确数据
// 是什么、在哪些前提不成立时拒绝。这一层是**纯规划**：它组合既有组件（复核/启用工作流
// 的撤销语义、树捕获层的身份与分帧摘要、作用域策略的 Managed 强制判定）产出一份完整、
// 可判定的计划，但不删除任何文件、不写任何账本、不提交任何存储、不接触 UI。
//
// 步骤顺序是安全性质，并由类型形状在结构上固定，而不是由列表位置约定：
//
// 1. 备份（backup）。任何删除步骤之前必须先有对**确切当前内容**的一份经验证备份：备份
//    必须是被删内容的备份，因此计划绑定记录当前的 contentIdentity，执行侧的捕获必须
//    与之逐字节相等——内容已漂移的备份不是备份。备份失败的移除是拒绝删除的移除。
// 2. 收回启用授权（grantWithdrawal）。经 ExtensionEnablementWorkflow 的停用语义，只按
//    (kind, id) 键合：内容已漂移、甚至记录已消失时授权依然必须可收回，否则被篡改过的
//    扩展将永远无法撤销其授权。
// 3. 收回复核记录（reviewWithdrawal）。永远在授权之后：授权是真正运行内容的那一半，
//    任何中间失败都必须停在"没有授权、复核记录尚存"的安全一侧，而不是"有授权、无复核"
//    的更坏一侧。两个步骤是计划类型的两个独立字段，构造顺序只有构建器一条路径——这个
//    类型无法表达"先收回复核"。
// 4. 删除内容（contentRemoval）。待删文件是一个有界的显式列表，逐条携带相对路径、字节
//    数与期望 SHA-256，全部来自调用方在调用前完成的新鲜捕获；目录按深度逆序排在文件
//    之后。执行侧仍必须按条目复核摘要——验证永不被静默跳过。
// 5. 元数据保留声明（retention）。移除恰好是"这份内容曾被授权运行"的记录最要紧的时刻，
//    因此身份元数据（记录身份、最后内容身份、备份 id）必须保留。备份 id 只有捕获完成后
//    才存在，计划以 backupIdDeferred 显式声明：执行侧必须先把捕获产出的备份 id 记入
//    保留元数据，才允许执行删除步骤。这份声明是计划数据，执行侧不能借"清理"之名抹掉
//    审计痕迹。
//
// 搁浅授权语义：目标内容已经消失（记录缺席或未安装）但授权或复核记录仍存在时，移除
// 仍然可规划——只是计划退化为只有权威收回步骤：备份步骤以 possible=false 加独立原因
// 代号显式声明"内容已不存在，无从备份"，删除步骤同样标记不可能，但授权与复核的收回
// 照常出现在计划里。一份退化的计划也照常绑定身份与保留声明。目标与授权都不存在时，
// 移除无事可做，以独立代号拒绝而不是产出一份关于虚无的计划。
//
// 种类边界是封闭的。`mcp:` 移除是对共享 settings.json 的**文档编辑**而非文件删除：该
// 文件被所有 mcp 主体共享，删掉一个服务器条目需要合并语义，而一份有界显式步骤列表
// 不重新实现文档合并就无法诚实表达它——本切片以独立代号拒绝，绝不产出一份声称删除
// 整个共享文件的 dishonest 计划。`codex-plugin:` 只有观察：应用从未安装它，也不持有
// 任何处于自身权威内可删除的字节，同样以独立代号拒绝。
//
// 前提不满足即拒绝（各自独立代号）：授权或复核集合读不出来（对着读不出的权威集合规划
// 等于规划一份会静默搁浅权威的 Partial 收回）、目标 id 畸形、目标缺席或无授权、同一
// (kind, id) 多条记录（清单不可信）、记录身份畸形、Managed 强制启用（组织策略强制的
// 扩展不可被用户请求移除，拒绝携带阻挡层级）、缺少新鲜捕获、捕获与记录之间的内容漂移
// （计划只对一份内容身份构建）、捕获条目路径越界（纵深防御重查）、撤销集合无法规划
// （透传工作流代号于 errorDetail）。
//
// 计划身份是目标身份与全部有序步骤的纯函数：主体、两个权威路径、每一步的每个字段按
// 固定顺序经长度分帧摘要绑定成 `extension-removal-plan:sha256:` 全形。任何一步变动、
// 内容漂移、或授权/复核步骤被交换顺序，都产出不同身份——交换后的"计划"无法通过身份
// 校验，这正是顺序不可重排的证明方式。
struct ExtensionRemovalBackupStep {
    // 恒为 0：备份是第一步。顺序常量由构建器写入，调用方无法重排。
    int order = 0;
    // 为 false 时内容已不存在，无从备份；impossibleCode 携带独立原因代号。
    bool possible = false;
    QString impossibleCode;
    QString subject;
    // 调用方权威：扩展根目录与备份目的地。这一层从不发明位置。
    QString sourceRoot;
    QString backupRoot;
    // 捕获必须与之逐字节相等的内容身份。绑定记录的当前内容：漂移的备份不是备份。
    QString contentIdentity;
};

struct ExtensionRemovalGrantWithdrawalStep {
    // 恒为 1：授权收回在复核收回之前。
    int order = 1;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 提交后应当存在的完整授权集合（经 ExtensionEnablementWorkflow 的停用语义产出，
    // 只按 (kind, id) 键合），以及读取时的代号，交给存储做比较并交换。
    QList<ExtensionEnablementGrant> resultingGrants;
    qint64 expectedGeneration = 0;
    // 集合没有变化时不应提交：没有授权可收回是显式的无操作，不是跳过这一步。
    bool changed = false;
};

struct ExtensionRemovalReviewWithdrawalStep {
    // 恒为 2：复核收回在授权收回之后。类型形状使相反顺序无法表达。
    int order = 2;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    QList<ExtensionReviewPin> resultingPins;
    qint64 expectedGeneration = 0;
    bool changed = false;
};

struct ExtensionRemovalContentFile {
    QString relativePath;
    qint64 byteCount = 0;
    QString sha256;
};

struct ExtensionRemovalContentStep {
    // 恒为 3：内容删除在备份与两次权威收回之后。
    int order = 3;
    bool possible = false;
    QString sourceRoot;
    // 待删文件：有界显式列表，来自与记录身份逐字节相等的新鲜捕获，保持捕获顺序。
    QVector<ExtensionRemovalContentFile> files;
    // 待删目录：深度逆序（最深层先删），同深度按路径字典序。
    QStringList directories;
};

struct ExtensionRemovalRetention {
    // 恒为 4：保留声明是计划的最后一步，约束执行侧在删除后不得清理审计痕迹。
    int order = 4;
    bool mustRetain = true;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 被保留的不可变身份。记录缺席时从幸存的授权/复核记录取最后已知身份；
    // 两者都不可得时 contentIdentityKnown 为 false。
    QString sourceIdentity;
    QString contentIdentity;
    bool contentIdentityKnown = false;
    // 恒为真：备份 id 只有捕获完成后才存在，执行侧必须先把它记入保留元数据再删除。
    bool backupIdDeferred = true;
};

enum class ExtensionRemovalSequenceState {
    Ready,
    Refused,
};

struct ExtensionRemovalSequence {
    ExtensionRemovalSequenceState state = ExtensionRemovalSequenceState::Refused;
    // 五个步骤是五个独立字段：计划没有可重排的步骤列表，顺序由类型形状固定。
    ExtensionRemovalBackupStep backup;
    ExtensionRemovalGrantWithdrawalStep grantWithdrawal;
    ExtensionRemovalReviewWithdrawalStep reviewWithdrawal;
    ExtensionRemovalContentStep contentRemoval;
    ExtensionRemovalRetention retention;
    // 计划身份：`extension-removal-plan:sha256:` 前缀的分帧摘要，按固定顺序绑定
    // 目标身份与每一步的每个字段。
    QString planIdentity;
    // 内容已消失但授权尚存时的退化计划为 true：只有权威收回步骤可执行。
    bool degradedAuthorityOnly = false;
    QString errorCode;
    // 拒绝的下游细节：被阻挡的层级标签或被透传的工作流代号。
    QString errorDetail;
};

struct ExtensionRemovalSequenceRequest {
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 清单、复核集合与授权集合都作为数据注入（含读出状态与代号）。这一层不接触
    // 任何存储：读不出的集合是拒绝理由，而不是被当成空集合规划。
    QList<ExtensionRegistryRecord> records;
    ExtensionReviewLedgerStoreResult reviewLedger;
    ExtensionEnablementLedgerStoreResult grantLedger;
    // 适用的作用域规则。Managed 强制启用的扩展不可被用户请求移除。
    QList<ExtensionScopeRule> scopeRules;
    // 调用方权威：扩展根目录与备份目的地，必须非空且为绝对路径。
    QString sourceRoot;
    QString backupRoot;
    // 新鲜捕获所使用的捕获域，由调用方给出（与恢复计划构建器同一先例）：这一层
    // 不持有任何清单组件的域副本。
    ExtensionTreeCaptureDomain captureDomain;
    // skill 目标必需：调用方在调用前对当前树完成的新鲜捕获。计划只针对一份内容
    // 身份构建；捕获身份与记录身份不一致即内容漂移，拒绝。
    QVector<ExtensionTreeCaptureEntry> freshTree;
    bool freshTreeCaptured = false;
};

class ExtensionRemovalSequenceBuilder
{
public:
    // 构建移除计划。任何前提不满足都返回 Refused 与独立的
    // `extension-removal-plan-*` 诊断，绝不产出部分计划。
    static ExtensionRemovalSequence plan(const ExtensionRemovalSequenceRequest &request);
};

#endif // EXTENSION_REMOVAL_SEQUENCE_H
