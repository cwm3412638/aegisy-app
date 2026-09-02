#ifndef CONFIGURATION_BACKUP_STORE_H
#define CONFIGURATION_BACKUP_STORE_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

class ConfigurationBackupKeyProvider
{
public:
    virtual ~ConfigurationBackupKeyProvider() = default;

    // Implementations must return exactly 32 bytes. Production may create the
    // SecureStorage-backed key only when allowCreate is true.
    virtual bool keyForScope(const QString &scope, bool allowCreate,
                             QByteArray *key, QString *error) = 0;
};

// 加密备份存储的持久化域。抽出这一层的理由与共享证据账本相同:备份机制本身(AES-256-GCM、
// 锁、原子写、pending 清单崩溃恢复、四态清点、身份绑定删除)与"备份的是谁"无关,而把整套机制
// 复制一份给扩展暂存去用,意味着两份实现会各自漂移——漂移的方向是其中一份丢掉某项检查。
//
// **域字符串一旦发布就不能再改。** 它们进入 AAD、密钥作用域与清单身份,因此改动其中任何一个
// 都会让既有备份全部无法解密或无法通过身份校验,而那等于在需要回滚的那一刻发现没有备份。
//
// **不同域之间必须无法互认。** 一份工具配置备份不得被当作扩展暂存备份接受,反之亦然。分隔来自
// 两处且都必须成立:密钥作用域不同(于是解密直接失败),以及 AAD 前缀不同(于是即使密钥相同也
// 认证失败)。仅靠密钥作用域是不够的——它只是一个字符串约定,而两个域共用同一份密钥的情形在
// 测试里必须能被构造出来并被拒绝。
//
// **新域应把域标识与载荷形状编进自己的 AAD 前缀,而不是作为新增的长度分帧字段。** 工具域的
// 前缀已经发布,给它追加分帧字段会让既有备份全部失效。因此前缀本身就是那个不透明的域标识,
// 新域只需选一个不同的字面量。不要"顺手改进"工具域的前缀。
struct ConfigurationBackupStoreDomain {
    // 进入持久化字节,发布后不可更改。
    // AAD 前缀。必须逐字节唯一:它是跨域互认的最后一道防线。
    QByteArray aadPrefix;
    QString manifestFormat;
    QString payloadFormat;
    // 清单身份的散列域与前缀。前缀会流出本存储(激活日志按前缀校验记录),因此共用前缀会让
    // 一个域的备份身份满足另一个域的校验。
    QByteArray identityDomain;
    QString identityPrefix;
    QString identityPattern;
    // 密钥作用域前缀。不同前缀意味着不同密钥,于是跨域解密直接失败。
    QString keyScopePrefix;
    // 清单里那个主体字段的 JSON 键名。工具域是 `tool`,而扩展域不该带一个叫 `tool` 的字段。
    QString subjectJsonKey;

    // 目录布局。
    QString manifestName;
    QString pendingName;
    QString lockFileName;

    // 语法与上限。
    QString backupIdPattern;
    // 主体标识的合法形状。工具域是四个 CLI 名字的闭集合;两个域的主体命名空间必须不重叠,
    // 否则基于主体的分隔就消失了——例如某个扩展标识恰好是字面量 `codex`。
    QString subjectPattern;
    int maxFiles = 0;
    qint64 maxFileBytes = 0;
    qint64 maxPayloadBytes = 0;
    qint64 maxManifestBytes = 0;
    int maxBackups = 0;

    // 诊断代号前缀。
    QString errorPrefix;

    // **默认为假,而且这是这个结构体里最重要的一个字段。** 旧版 v1 迁移是本存储唯一一处依据
    // 未经认证的输入清单去写盘的路径,它的存在只是为了搬运工具域真实存在过的历史记录。一个
    // 没有 v1 历史的新域绝不能继承这条路径:继承它等于凭一份任何人都能放进目录的明文清单
    // 触发写入。
    bool legacyV1MigrationEnabled = false;

    // 每一个字段都必须被填写。半填的域会让某一项检查退化成"没有约束",而那不会报错。
    bool configured() const;
};

struct ConfigurationBackupFile {
    int slot = -1;
    bool existed = false;
    QByteArray content;
};

struct ConfigurationBackupSnapshot {
    QString backupId;
    QString tool;
    QDateTime createdAt;
    QList<ConfigurationBackupFile> files;
};

enum class ConfigurationBackupInventoryState {
    Empty,
    Ready,
    Unavailable,
    Invalid,
};

struct ConfigurationBackupInventoryEntry {
    QString backupId;
    QString tool;
    QDateTime createdAt;
    int fileCount = 0;
    QString identity;
};

struct ConfigurationBackupInventoryResult {
    ConfigurationBackupInventoryState state =
        ConfigurationBackupInventoryState::Invalid;
    QList<ConfigurationBackupInventoryEntry> entries;
    QString issue;
};

class ConfigurationBackupStore
{
public:
    static constexpr qint64 MaxFileBytes = 4 * 1024 * 1024;
    static constexpr qint64 MaxPayloadBytes = 8 * 1024 * 1024;
    static constexpr qint64 MaxManifestBytes = 16 * 1024 * 1024;
    static constexpr int MaxFiles = 16;
    static constexpr int MaxBackups = 64;

    // 工具配置备份的域。常量留在实现文件里,而不是由调用方拼装:`product_scope_policy` 固定了
    // 旧版载荷名不得出现在 `tool_manager.cpp`,而更根本的理由是这些字面量一旦散到调用方,
    // "发布后不可更改"就没有任何一处可以被集中审查。
    static ConfigurationBackupStoreDomain toolDomain();

    // 既有的两参构造保留并委托到工具域,因此所有现存调用方与其测试逐字节不变。
    ConfigurationBackupStore(const QString &rootPath,
                             ConfigurationBackupKeyProvider *keyProvider);

    // 扩展暂存域只定义持久化边界,当前没有产品调用方。目录快照与恢复流程必须在后续
    // 切片中明确接入前,不能把这个域误当成已经开放的安装或启用权限。
    static ConfigurationBackupStoreDomain extensionStagingDomain();

    ConfigurationBackupStore(const ConfigurationBackupStoreDomain &domain,
                             const QString &rootPath,
                             ConfigurationBackupKeyProvider *keyProvider);

    bool create(const ConfigurationBackupSnapshot &snapshot, QString *error);
    bool read(const QString &tool, const QString &backupId,
              ConfigurationBackupSnapshot *snapshot, QString *error);

    // Migrates the exact legacy ToolManager v1 directory in place. A valid
    // manifest.v2.pending file is the crash-recovery authority once published.
    bool migrateLegacy(const QString &tool, int legacyToolValue,
                       const QString &backupId,
                       const QStringList &managedPaths, QString *error);

    ConfigurationBackupInventoryResult inventory(
        const QString &tool, int legacyToolValue,
        const QStringList &managedPaths);
    bool removeVerified(const QString &tool, const QString &backupId,
                        const QString &expectedIdentity, QString *error);

    static bool isValidBackupId(const QString &backupId);
    static bool isValidTool(const QString &tool);

private:
    bool migrateLegacyLocked(const QString &tool, int legacyToolValue,
                             const QString &backupId,
                             const QStringList &managedPaths, QString *error);

    ConfigurationBackupStoreDomain m_domain;
    QString m_rootPath;
    ConfigurationBackupKeyProvider *m_keyProvider = nullptr;
};

#endif // CONFIGURATION_BACKUP_STORE_H
