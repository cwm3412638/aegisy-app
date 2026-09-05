#ifndef EXTENSION_STAGING_SNAPSHOT_H
#define EXTENSION_STAGING_SNAPSHOT_H

#include "configuration_backup_store.h"
#include "extension_tree_capture.h"

#include <QDateTime>
#include <QString>
#include <QVector>

// 扩展暂存快照契约：一棵 `ExtensionTreeCapture` 捕获的树如何变成暂存域的一份备份，以及读回
// 时如何验证。存储的载荷是按槽位编号且无路径的（`ConfigurationBackupFile{slot, existed,
// content}`），而恢复一棵树必须知道每个槽位对应哪条相对路径，因此槽 0 固定承载一份路径清单
// 文档（`aegisy-extension-staging-snapshot-manifest/0.1`，规范化 JSON），槽 1..N 按清单中
// 文件条目的顺序承载文件内容。目录只占清单条目，不占槽位。
//
// 上限对账是这一层的核心契约。捕获层允许 4096 条目、单文件 2 MiB、总量 16 MiB；暂存域允许
// 256 槽、单槽 4 MiB、载荷 64 MiB、清单 32 MiB。树先过捕获层、再进暂存域，因此更紧的那一侧
// 必须在产出任何字节之前就以独立诊断拒绝——例如 300 个文件的树捕获层放行，但槽位只有 256
// 个（槽 0 已被清单占用，文件至多 255 个），超出的树被拒绝而不是被截断。清单文档本身占用
// 槽 0，因此它的实际上限是单槽上限与域清单上限中更紧的那个（4 MiB），而不是 32 MiB。
//
// 清单是严格的：未知字段、重复路径、非规范化 JSON、NUL 或非法 UTF-8、遍历形状的路径
// （`..`、绝对路径、分隔符误用）、槽位/计数/字节不符，全部以各自独立的
// `extension-staging-snapshot-*` 诊断失败关闭，绝不静默跳过。验证器在读回时重算每个文件槽的
// 散列与字节数，并用捕获域重算整树身份；任何不符都是完整性失败。
//
// 这一层不安装、不启用、不执行任何东西，也不写盘：`build` 只产出内存中的快照对象，是否
// 交给 `ConfigurationBackupStore` 持久化由调用方决定。当前没有任何产品调用方。
class ExtensionStagingSnapshot
{
public:
    // 清单文档的格式标识。一旦随暂存备份发布即不可更改：读回侧按它拒绝格式不符的槽 0。
    static QString manifestFormat();

    // 由捕获结果构造暂存域快照。`subject` 是注册表风格主体（`kind:id`），在任何捕获结果被
    // 触碰之前就按暂存域的主体语法校验；`backupId` 与 `createdAt` 属于存储元数据，按暂存域
    // 的 backup-id 语法校验。失败时不产出任何部分快照。
    static bool build(const ExtensionTreeCaptureDomain &captureDomain,
                      const QVector<ExtensionTreeCaptureEntry> &tree,
                      const QString &subject,
                      const QString &backupId,
                      const QDateTime &createdAt,
                      ConfigurationBackupSnapshot *snapshot,
                      QString *error);

    // 验证一份从暂存域读回（已解密）的快照。`captureDomain` 必须就是构建时所用的捕获域：
    // 树身份按域摘要，用错域会以 `extension-staging-snapshot-identity-mismatch` 失败关闭，
    // 而不是退回某个默认域。`expectedSubject` 同时与清单主体和快照主体逐字节相等才通过。
    static bool verify(const ExtensionTreeCaptureDomain &captureDomain,
                       const QString &expectedSubject,
                       const ConfigurationBackupSnapshot &snapshot,
                       QString *error);

    // 与四参 `verify` 完全相同的校验；通过时额外把按清单顺序重建的树（目录条目带空字节，
    // 文件条目带槽内容）写入 `rebuiltTree`。校验失败时 `rebuiltTree` 保持为空：恢复计划层
    // 只被允许消费这棵树，绝不自行重解析清单——两份解析会各自漂移。
    static bool verify(const ExtensionTreeCaptureDomain &captureDomain,
                       const QString &expectedSubject,
                       const ConfigurationBackupSnapshot &snapshot,
                       QVector<ExtensionTreeCaptureEntry> *rebuiltTree,
                       QString *error);
};

#endif // EXTENSION_STAGING_SNAPSHOT_H
