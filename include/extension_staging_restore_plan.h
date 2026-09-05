#ifndef EXTENSION_STAGING_RESTORE_PLAN_H
#define EXTENSION_STAGING_RESTORE_PLAN_H

#include "configuration_backup_store.h"
#include "extension_staging_snapshot.h"
#include "extension_tree_capture.h"

#include <QString>
#include <QVector>

// 扩展暂存恢复计划契约：一份【已通过验证】的暂存快照如何变成一份精确、有界、有序的恢复计划。
// 这一层只规划，绝不执行：不创建目录、不写文件、不创建符号链接、不接触存储、不接触 UI。
//
// 信任边界有三条，每条都不能由省掉：
//
// 1. 快照必须在计划之前验证。`plan` 内部调用 `ExtensionStagingSnapshot::verify`（五参重载）
//    并只消费它重建出的树；调用方传入的未验证字节绝不会被直接解析。验证失败时计划层原样
//    透传 `extension-staging-snapshot-*` 诊断：篡改过的快照在计划开始前就失败关闭。
//
// 2. 目标根的权威在调用方。这一层从不发明位置：`destinationRoot` 由调用方给出，计划层只
//    校验它非空、绝对、且与注入观察接口报告的规范化形式逐字节一致。目标根的当前状态只能
//    通过注入的 `ExtensionStagingRestoreObservation` 观察（只读：类型查询与已有文件内容
//    读取），因此测试可以模拟任意磁盘状态而不触碰真实产品目录；观察不可用一律拒绝，绝不
//    盲计划。
//
// 3. 纵深防御重查。清单虽然已被验证，计划层仍逐条重查：路径形状（含 `..`、绝对路径、空
//    段的任何路径都以 `extension-staging-restore-path-escapes-destination` 拒绝）、暂存域
//    上限（文件数、单文件字节、聚合字节，超出即 `bounds-exceeded`——验证层不单独守聚合
//    上限，而存储层在写入时才守，计划必须在产出任何决定之前自己守一次）、目标现状（任何
//    计划路径组件上存在符号链接、目标根本身是符号链接、已有内容与计划内容不符，各自独立
//    拒绝）。
//
// 冲突语义：目标树上已有路径的内容与计划内容不同，是 `destination-conflict` 拒绝，绝不
// 静默覆盖——恢复到一棵脏树必须是人显式处理过的决定，而不是计划层替人做的决定。已有内容
// 与计划逐字节一致的操作保留在计划里并标记 `alreadyInPlace`：它是显式的"无需写入"语义，
// 而不是跳过；执行侧（尚不存在）仍必须按条目携带的期望摘要复核每一个 already-in-place
// 文件，验证永不被静默跳过。
//
// 计划身份是目标根与全部计划操作的纯函数：规范化目标根、主体、树身份与每一条操作（类型、
// 路径、字节数、期望摘要、来源槽位、是否 already-in-place）经共享树捕获层的长度分帧摘要
// 绑定成 `planIdentity`。同一份快照对两个不同目标根产生两个不同身份；任何一条操作变动
// 也产生不同身份。计划因此只对一个树状态与一个目标成立。
class ExtensionStagingRestoreObservation
{
public:
    enum class NodeKind {
        // 路径不存在。
        Missing,
        File,
        Directory,
        // 符号链接在任何计划位置都是拒绝理由，而不是被跟随。
        Symlink,
        // 其他特殊文件（套接字、设备、管道等）。
        Other,
        // 状态读不出来，当前内容未知。
        Unavailable,
    };

    virtual ~ExtensionStagingRestoreObservation() = default;

    // 目标根的规范化绝对路径。空串表示无法确定（观察不可用）。调用方给出的目标根必须与
    // 它逐字节相等，否则按非规范化目标拒绝。
    virtual QString canonicalRoot() = 0;

    // 目标根内一条相对路径的节点类型；空串查询目标根本身。只读。
    virtual NodeKind nodeKind(const QString &relativePath) = 0;

    // 只读读取目标根内一个已有文件的内容，用于与计划内容比对。读不出来返回 false。
    // 读到的字节绝不写回任何地方。
    virtual bool fileContent(const QString &relativePath, QByteArray *content) = 0;
};

struct ExtensionStagingRestoreOperation {
    // true 表示创建目录，false 表示写入文件。
    bool directory = false;
    // 目标根内的相对路径。
    QString relativePath;
    // 仅文件：期望字节数、期望 SHA-256（小写十六进制）、来源快照槽位。
    qint64 byteCount = 0;
    QString sha256;
    int sourceSlot = 0;
    // 目标现状与计划内容逐字节一致。显式语义而非跳过：执行侧仍必须复核摘要。
    bool alreadyInPlace = false;
};

struct ExtensionStagingRestorePlan {
    // 规范化后的目标根（与观察接口报告的形式一致）。
    QString destinationRoot;
    QString subject;
    // 被验证快照的树身份（捕获域摘要，原样来自清单）。
    QString treeIdentity;
    // 计划身份：`extension-staging-restore-plan:sha256:` 前缀的分帧摘要。
    QString planIdentity;
    // 目录创建在前、文件写入在后，各自保持清单顺序。
    QVector<ExtensionStagingRestoreOperation> operations;
};

class ExtensionStagingRestorePlanBuilder
{
public:
    // 由一份暂存快照与调用方给出的目标根构造恢复计划。任何失败都不产出部分计划，
    // 并以独立的 `extension-staging-restore-*` 诊断失败关闭；内部验证失败时透传
    // `extension-staging-snapshot-*` 诊断。`observation` 必须非空。
    static bool plan(const ExtensionTreeCaptureDomain &captureDomain,
                     const QString &expectedSubject,
                     const ConfigurationBackupSnapshot &snapshot,
                     const QString &destinationRoot,
                     ExtensionStagingRestoreObservation *observation,
                     ExtensionStagingRestorePlan *plan,
                     QString *error);
};

#endif // EXTENSION_STAGING_RESTORE_PLAN_H
