#ifndef EXTENSION_STAGING_BACKUP_CAPTURE_H
#define EXTENSION_STAGING_BACKUP_CAPTURE_H

#include "configuration_backup_store.h"
#include "extension_tree_capture.h"

#include <QString>

// 扩展暂存备份捕获工作流：把一棵活着的扩展树变成暂存域里的一份加密备份。快照契约与恢复
// 计划都已经存在，但在本组件之前没有任何东西从真实扩展树产出备份——这条路径就是那个
// 生产者。本组件写入的唯一目标是应用私有的加密备份存储（与 ToolManager 的工具配置备份
// 同一类写入）；它不修改扩展来源树，不安装、不启用、不执行任何东西，也不做保留期裁剪、
// 清单管理或恢复执行。当前没有任何产品调用方。
//
// 位置权威在调用方：扩展住在哪里（sourceRoot）与备份写到哪里（backupRoot）都由调用方
// 给出，本组件从不发明位置。对来源根本身只施加捕获层同款的规范化/符号链接纪律：来源根
// 是符号链接时以独立诊断拒绝（规范化会静默解析它，因此必须在规范化之前检查），其余
// 包含性与漂移检查由捕获层在扫描内部完成。
//
// 种类到捕获域的映射是封闭的。`skill:` 主体经 `SkillExtensionInventory::treeCaptureDomain()`
// 的技能捕获域捕获——身份字节与技能清单路径逐字节一致，于是同一棵树在清单与备份里算出
// 同一个身份。`codex-plugin:` 与 `mcp:` 主体被各自独立的诊断拒绝：Codex 插件经 CLI 输出
// 捕获进入、MCP 经设置 JSON 进入，两者都不是这一层可以假装去捕获的树。语法之外或映射
// 之外的种类绝不落到某个默认域。
//
// 顺序是安全性质：主体在任何文件系统工作之前按暂存域语法校验；随后是种类映射、来源根
// 纪律、有界捕获（漂移复查由捕获层持有）、快照构建，最后才是存储写入。任何一步失败都
// 以独立诊断失败关闭且不留下半份状态：捕获层与快照层不写盘，而存储的 `create` 是
// 备份目录级原子的（加锁、原子写、写后重读重解析复核，任何失败都回收整个
// 备份目录），因此失败路径上没有需要本组件自己清理的残留。捕获层、快照层与存储层的
// 诊断逐字透传，不另造本地代号。
//
// 再捕获语义：同一棵未变化的树允许再捕获（存储分配新的备份 id），但结果报告新树的
// 内容身份与该主体最近一次既有备份是否一致，调用方据此避免无界churn。这个比对是对该
// 主体的只读清点加上对最近备份的完整读回验证（只消费验证器重建的树来重算身份，绝不
// 自行解析清单）。清点不可用或最近备份读不回/验不过时，比对结果是显式的 Unknown 加
// 独立降级诊断，而不是静默变成"没有既有备份"：清点只是建议性输入，存储坏了的时候
// 恰恰最不该丢备份，因此降级不阻断写入。写入成功后再清点一次取回清单身份（审计用它
// 把备份绑到确切的树）；这次清点同样可能退化，退化时清单身份留空并给出独立诊断，
// 备份本身仍完整在盘上且可按 id 直接读回。
enum class ExtensionStagingPriorIdentity {
    // 该主体没有任何既有备份。
    NoPriorBackup,
    // 新树的内容身份与最近一次既有备份逐字节一致。
    Matched,
    // 不一致：树变了，或者最近备份是另一棵树。
    Mismatched,
    // 无法确定：清点退化或最近备份读不回/验不过。priorIdentityDiagnostic 携带独立诊断。
    Unknown,
};

struct ExtensionStagingBackupCaptureResult {
    QString backupId;
    QString subject;
    // 捕获域下的内容身份（清单里的 `identity` 字段原样值）。
    QString treeIdentity;
    // 存储侧清单身份（`removeVerified` 与激活日志按它确认"是哪一份"）。仅在
    // manifestIdentityKnown 为真时有效。
    QString manifestIdentity;
    bool manifestIdentityKnown = false;
    QString manifestIdentityDiagnostic;
    ExtensionStagingPriorIdentity priorIdentity =
        ExtensionStagingPriorIdentity::Unknown;
    QString priorIdentityDiagnostic;
};

class ExtensionStagingBackupCapture
{
public:
    // 捕获一个扩展的树并写入暂存备份域。`subject` 是 `kind:id` 主体，先于一切文件系统
    // 工作校验；`sourceRoot` 是调用方给出的扩展根；`backupRoot` 与 `keyProvider` 是
    // 调用方给出的存储目的地与密钥来源（与 ToolManager 的用法一致）。失败时不产出部分
    // 结果、不留下半份备份；成功时填充 result（含上述降级字段）。
    static bool capture(const QString &subject,
                        const QString &sourceRoot,
                        const QString &backupRoot,
                        ConfigurationBackupKeyProvider *keyProvider,
                        ExtensionStagingBackupCaptureResult *result,
                        QString *error);
};

#endif // EXTENSION_STAGING_BACKUP_CAPTURE_H
