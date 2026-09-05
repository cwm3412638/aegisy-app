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
// 同一个身份。`mcp:` 主体经 `McpConfigurationInventory::backupCaptureDomain()` 的 MCP 备份
// 域捕获，但备份单元不是一棵树：`sourceRoot` 此时是调用方给出的 MCP 设置文件路径，本组件以
// 与 `McpConfigurationInventory::inspectFile` 相同的纪律（符号链接拒绝、1 MiB 上限、读取后
// 漂移复查）读出整个文件的原始字节，再合成一棵固定单条目树（相对路径恒为字面量
// `settings.json`，绝不从调用方的文件名推导，因此无论文件住在哪里清单形状都稳定）送入共享
// 快照契约。这是对诚实性的强制：该文件同时是来源身份的单位与变更的单位（McpConfigDialog
// 整文档重写、ToolManager 合并写 env 键都作用于整个文件），且被文件里所有 `mcp:` 主体共享——
// 按单个服务器抽取字节会产出一份恢复时会覆盖其他服务器配置的 dishonest 备份，因此备份是
// 整个文件，恢复语义也是整文件；结果用 `coversSharedSettingsFile` 把这一点显式暴露给调用方。
// 备份层刻意不解析 JSON：字节就是真相，一个 JSON 已损坏的设置文件恰恰是最需要先备份下来的
// 状态，有效性判定属于清单与恢复路径。备份树身份是一个新身份（域
// `aegisy-mcp-config-backup-content/0.1\0`），与清单的来源身份
// （`aegisy-mcp-config-source/0.1\0`）输入帧不同，绝不应被声称相等。`codex-plugin:` 主体仍被
// 原诊断拒绝，而且理由是更深一层的"只有观察"：应用只观察捕获到的 CLI 列表输出，`source.path`
// 是从未被子进程之外打开的、未验证的子进程元数据，应用对插件没有任何处于自身权威内的可备份
// 字节，也没有任何变更面。语法之外或映射之外的种类绝不落到某个默认域。
//
// 上限对账：MCP 读取走清单的 1 MiB 上限，它比捕获层的单文件 2 MiB 与暂存域的单槽 4 MiB 都
// 紧，更紧的一侧在产出任何字节之前获胜——同一份字节在任何一层都不会因为上限差异而被另一层
// 拒绝。
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
    // 为 true 时这份备份覆盖的是整个共享 MCP 设置文件，而不只是该主体的服务器条目：
    // 文件被所有 `mcp:` 主体共享，恢复语义也是整文件。`skill:` 捕获恒为 false。
    bool coversSharedSettingsFile = false;
};

class ExtensionStagingBackupCapture
{
public:
    // 捕获一个扩展并写入暂存备份域。`subject` 是 `kind:id` 主体，先于一切文件系统
    // 工作校验；`sourceRoot` 是调用方给出的位置——`skill:` 主体时是扩展根目录，`mcp:`
    // 主体时是 MCP 设置文件路径；`backupRoot` 与 `keyProvider` 是调用方给出的存储目的地
    // 与密钥来源（与 ToolManager 的用法一致）。失败时不产出部分结果、不留下半份备份；
    // 成功时填充 result（含上述降级字段）。
    static bool capture(const QString &subject,
                        const QString &sourceRoot,
                        const QString &backupRoot,
                        ConfigurationBackupKeyProvider *keyProvider,
                        ExtensionStagingBackupCaptureResult *result,
                        QString *error);
};

#endif // EXTENSION_STAGING_BACKUP_CAPTURE_H
