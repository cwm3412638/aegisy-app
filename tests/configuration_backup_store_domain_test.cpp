#include "configuration_backup_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>
#include <QRegularExpression>

namespace {

int failures = 0;

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

// 故意对所有作用域返回同一把密钥。跨域分隔必须在密钥相同的情况下依然成立:仅靠密钥作用域
// 的分隔只是一个字符串约定,而两个域共用同一份密钥的情形在真实故障里完全可能出现——例如
// 有人复制粘贴了作用域前缀。这个 provider 让 AAD 分隔成为唯一起作用的那道防线。
class SharedKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &scope, bool, QByteArray *key,
                     QString *) override
    {
        scopes.insert(scope);
        if (!key) return false;
        *key = QByteArray(32, 'k');
        return true;
    }

    QSet<QString> scopes;
};

ConfigurationBackupStoreDomain fixtureDomain(const char *tag)
{
    ConfigurationBackupStoreDomain value;
    value.aadPrefix = QByteArray("aegisy-fixture-") + tag + QByteArray("-aad/0.1");
    value.manifestFormat = QStringLiteral("aegisy-fixture-%1").arg(QLatin1String(tag));
    value.payloadFormat =
        QStringLiteral("aegisy-fixture-%1-payload/0.1").arg(QLatin1String(tag));
    value.identityDomain = QByteArray("aegisy-fixture-") + tag + QByteArray("-identity/0.1");
    value.identityPrefix =
        QStringLiteral("fixture-%1-manifest:sha256:").arg(QLatin1String(tag));
    value.identityPattern =
        QStringLiteral("^fixture-%1-manifest:sha256:[0-9a-f]{64}$").arg(QLatin1String(tag));
    value.keyScopePrefix =
        QStringLiteral("fixture/%1-master/v1/").arg(QLatin1String(tag));
    value.subjectJsonKey = QStringLiteral("subject");
    value.manifestName = QStringLiteral("manifest.json");
    value.pendingName = QStringLiteral("manifest.pending");
    value.lockFileName = QStringLiteral(".fixture.lock");
    value.backupIdPattern = QStringLiteral("^[0-9a-f]{8}$");
    value.subjectPattern = QStringLiteral("^subject-[a-z]+$");
    value.maxFiles = 4;
    value.maxFileBytes = 4096;
    value.maxPayloadBytes = 8192;
    value.maxManifestBytes = 16384;
    value.maxBackups = 8;
    value.errorPrefix = QStringLiteral("fixture-%1").arg(QLatin1String(tag));
    value.legacyV1MigrationEnabled = false;
    return value;
}

ConfigurationBackupSnapshot snapshot()
{
    ConfigurationBackupSnapshot value;
    value.backupId = QStringLiteral("0123abcd");
    value.tool = QStringLiteral("subject-one");
    value.createdAt = QDateTime::fromString(
        QStringLiteral("2026-08-23T12:00:00.123Z"), Qt::ISODateWithMs);
    ConfigurationBackupFile file;
    file.slot = 0;
    file.existed = true;
    file.content = QByteArrayLiteral("fixture-content");
    value.files.append(file);
    return value;
}

// 任意主体与 id 的夹具快照:混合主体根的测试需要它。
ConfigurationBackupSnapshot snapshotFor(const QString &subject,
                                        const QString &backupId)
{
    ConfigurationBackupSnapshot value = snapshot();
    value.tool = subject;
    value.backupId = backupId;
    return value;
}

QString idForIndex(int index)
{
    return QStringLiteral("%1").arg(index, 8, 16, QLatin1Char('0'));
}

// 只为选定主体提供密钥的 provider:foreign 条目的密钥不可得时,清点无法分辨它是
// foreign-intact 还是 corrupt,必须如实退化而不是猜。
class SelectiveKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    explicit SelectiveKeyProvider(const QString &servedScope)
        : m_servedScope(servedScope)
    {
    }

    bool keyForScope(const QString &scope, bool, QByteArray *key,
                     QString *) override
    {
        if (scope != m_servedScope || !key) return false;
        *key = QByteArray(32, 'k');
        return true;
    }

private:
    QString m_servedScope;
};

bool writePlantedManifest(const QString &directoryPath,
                          const ConfigurationBackupStoreDomain &domain,
                          const QByteArray &bytes)
{
    if (!QDir().mkpath(directoryPath)) return false;
    QFile file(QDir(directoryPath).filePath(domain.manifestName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written;
}

QByteArray readPlantedManifest(const QString &directoryPath,
                               const ConfigurationBackupStoreDomain &domain)
{
    QFile file(QDir(directoryPath).filePath(domain.manifestName));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

} // namespace

// 一份在 A 域创建的备份不得在 B 域被读出来,而且这必须在两个域拿到同一把密钥时依然成立。
// 只有这样才能证明分隔来自 AAD,而不是恰好来自不同的密钥。
void testCrossDomainReadIsRefusedUnderAnIdenticalKey()
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    SharedKeyProvider provider;
    const ConfigurationBackupStoreDomain a = fixtureDomain("alpha");
    ConfigurationBackupStoreDomain b = fixtureDomain("beta");
    // 让 B 域与 A 共用密钥作用域前缀,于是两者拿到的密钥字节相同:下面的拒绝不可能是解密
    // 密钥不同造成的。
    b.keyScopePrefix = a.keyScopePrefix;
    b.subjectPattern = a.subjectPattern;
    b.backupIdPattern = a.backupIdPattern;

    ConfigurationBackupStore storeA(a, root.path(), &provider);
    QString error;
    if (!expect(storeA.create(snapshot(), &error),
                "the fixture domain could not create a backup")) {
        QTextStream(stderr) << "  create said: " << error << '\n';
        return;
    }
    QByteArray keyA;
    QByteArray keyB;
    provider.keyForScope(a.keyScopePrefix + QStringLiteral("subject-one"), false,
                         &keyA, nullptr);
    provider.keyForScope(b.keyScopePrefix + QStringLiteral("subject-one"), false,
                         &keyB, nullptr);
    if (!expect(keyA == keyB && !keyA.isEmpty(),
                "the fixture did not hand both domains the same key")) {
        return;
    }

    ConfigurationBackupStore storeB(b, root.path(), &provider);
    ConfigurationBackupSnapshot readBack;
    QString crossError;
    expect(!storeB.read(QStringLiteral("subject-one"), QStringLiteral("0123abcd"),
                        &readBack, &crossError),
           "a backup created in one domain was accepted in another");
    expect(!crossError.isEmpty(),
           "a refused cross-domain read carries no diagnostic");
    // 而同一个域必须读得回来:否则上面的拒绝可能只是因为备份本身坏了。
    ConfigurationBackupSnapshot sameDomain;
    QString sameError;
    if (expect(storeA.read(QStringLiteral("subject-one"), QStringLiteral("0123abcd"),
                           &sameDomain, &sameError),
               "the creating domain could not read back its own backup")) {
        expect(sameDomain.files.size() == 1
                   && sameDomain.files.at(0).content
                       == QByteArrayLiteral("fixture-content"),
               "the round trip did not preserve the payload");
    }
}

// AAD 前缀单独就必须足以拒绝跨域读取。上一个测试里 B 域的每一个持久化字符串都不同,于是
// 拒绝可能来自其中任何一个——最先被检查到的很可能是明文清单里的 `format` 字段,而那道检查
// 只是一个字符串比较,任何能写目录的人都能改。真正不可绕过的那道是 AAD:改掉它就得重新
// 认证密文,而没有密钥做不到。因此这里构造一个除 AAD 前缀之外与 A 逐字段相同的域,并要求
// 读取仍然被拒绝。这个测试存在的意义是:如果将来有人把 AAD 前缀"统一"成一个共享常量,
// 上一个测试仍然会通过,而这一个会失败。
void testTheAadPrefixAloneSeparatesTheDomains()
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    SharedKeyProvider provider;
    const ConfigurationBackupStoreDomain a = fixtureDomain("alpha");
    ConfigurationBackupStoreDomain shadow = a;
    // 唯一的差别。清单格式、身份域、密钥作用域、主体键名、目录布局全都相同,于是明文层面
    // 的每一道检查都会通过,只剩认证这一道。
    shadow.aadPrefix = QByteArrayLiteral("aegisy-fixture-shadow-aad/0.1");
    expect(shadow.configured(),
           "the shadow domain is not configured, so the test proves nothing");

    ConfigurationBackupStore storeA(a, root.path(), &provider);
    QString error;
    if (!expect(storeA.create(snapshot(), &error),
                "the fixture domain could not create a backup")) {
        QTextStream(stderr) << "  create said: " << error << '\n';
        return;
    }

    ConfigurationBackupStore storeShadow(shadow, root.path(), &provider);
    ConfigurationBackupSnapshot readBack;
    QString crossError;
    expect(!storeShadow.read(QStringLiteral("subject-one"),
                             QStringLiteral("0123abcd"), &readBack, &crossError),
           "a differing AAD prefix alone did not refuse a foreign backup");
    expect(!crossError.isEmpty(),
           "a refused read carries no diagnostic");
}

// 清单身份同样必须单独成立。身份前缀会流出本存储——激活日志按前缀校验它记下的记录——因此
// 两个域为同一份字节算出同一个身份,意味着一个域的备份身份满足另一个域的校验,而身份正是
// `removeVerified` 用来确认"删的是我看到的那一份"的东西。
void testTheIdentityDomainAloneSeparatesTheIdentities()
{
    const ConfigurationBackupStoreDomain a = fixtureDomain("alpha");
    ConfigurationBackupStoreDomain shadow = a;
    shadow.identityDomain = QByteArrayLiteral("aegisy-fixture-shadow-identity/0.1");
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    SharedKeyProvider provider;
    ConfigurationBackupStore storeA(a, root.path(), &provider);
    QString error;
    if (!expect(storeA.create(snapshot(), &error),
                "the fixture domain could not create a backup")) {
        return;
    }
    const ConfigurationBackupInventoryResult fromA =
        storeA.inventory(QStringLiteral("subject-one"), 0, {});
    ConfigurationBackupStore storeShadow(shadow, root.path(), &provider);
    const ConfigurationBackupInventoryResult fromShadow =
        storeShadow.inventory(QStringLiteral("subject-one"), 0, {});
    if (!expect(fromA.state == ConfigurationBackupInventoryState::Ready
                    && fromA.entries.size() == 1,
                "the creating domain could not inventory its own backup")) {
        return;
    }
    if (!expect(fromShadow.state == ConfigurationBackupInventoryState::Ready
                    && fromShadow.entries.size() == 1,
                "the shadow domain could not inventory the same bytes")) {
        return;
    }
    // 同一份清单字节,两个身份。这正是要求:身份是"哪一个域的哪一份",不只是"哪一份"。
    expect(fromA.entries.at(0).identity != fromShadow.entries.at(0).identity,
           "two domains computed the same identity for the same manifest bytes");
    // 而且一个域的身份不得通过另一个域的身份校验:`removeVerified` 拿一个外域身份必须拒绝。
    QString removeError;
    expect(!storeShadow.removeVerified(QStringLiteral("subject-one"),
                                       QStringLiteral("0123abcd"),
                                       fromA.entries.at(0).identity, &removeError),
           "one domain's identity satisfied another domain's verified removal");
    expect(!removeError.isEmpty(), "a refused verified removal carries no diagnostic");
    // 被拒绝的删除不得删掉任何东西:否则一次拒绝反而完成了它拒绝的那件事。
    const ConfigurationBackupInventoryResult after =
        storeA.inventory(QStringLiteral("subject-one"), 0, {});
    expect(after.state == ConfigurationBackupInventoryState::Ready
               && after.entries.size() == 1,
           "a refused verified removal still removed the backup");
}

// 半填的域必须被每一个入口拒绝。一个空的 AAD 前缀不会报错——它只会让跨域互认的最后一道
// 防线消失,而那种失效是沉默的。
void testUnconfiguredDomainIsRefused()
{
    const ConfigurationBackupStoreDomain empty;
    expect(!empty.configured(),
           "a default-constructed domain claims to be configured");
    // 逐个字段清空:每一个字段都必须是必需的,否则那个字段的约束等于不存在。
    const char *names[] = {"aadPrefix", "manifestFormat", "payloadFormat",
                           "identityDomain", "identityPrefix", "identityPattern",
                           "keyScopePrefix", "subjectJsonKey", "manifestName",
                           "pendingName", "lockFileName", "backupIdPattern",
                           "subjectPattern", "errorPrefix"};
    for (int i = 0; i < 14; ++i) {
        ConfigurationBackupStoreDomain partial = fixtureDomain("alpha");
        switch (i) {
        case 0: partial.aadPrefix.clear(); break;
        case 1: partial.manifestFormat.clear(); break;
        case 2: partial.payloadFormat.clear(); break;
        case 3: partial.identityDomain.clear(); break;
        case 4: partial.identityPrefix.clear(); break;
        case 5: partial.identityPattern.clear(); break;
        case 6: partial.keyScopePrefix.clear(); break;
        case 7: partial.subjectJsonKey.clear(); break;
        case 8: partial.manifestName.clear(); break;
        case 9: partial.pendingName.clear(); break;
        case 10: partial.lockFileName.clear(); break;
        case 11: partial.backupIdPattern.clear(); break;
        case 12: partial.subjectPattern.clear(); break;
        case 13: partial.errorPrefix.clear(); break;
        default: break;
        }
        if (!partial.configured()) continue;
        QTextStream(stderr) << "FAIL: clearing " << names[i]
                            << " still left the domain configured\n";
        ++failures;
    }
    // 上限为 0 意味着任何载荷都超限,于是存储变成永久拒绝——同样是一种沉默的失效。
    ConfigurationBackupStoreDomain zeroBound = fixtureDomain("alpha");
    zeroBound.maxFiles = 0;
    expect(!zeroBound.configured(),
           "a domain with a zero file bound claims to be configured");
    // 清单名与 pending 名相同会让崩溃恢复权威覆盖正式清单。
    ConfigurationBackupStoreDomain collided = fixtureDomain("alpha");
    collided.pendingName = collided.manifestName;
    expect(!collided.configured(),
           "a domain whose pending file shadows its manifest claims to be configured");
    // 锁名与清单名相同会让根目录扫描把锁当成清单。
    ConfigurationBackupStoreDomain lockCollided = fixtureDomain("alpha");
    lockCollided.lockFileName = lockCollided.manifestName;
    expect(!lockCollided.configured(),
           "a domain whose lock file shadows its manifest claims to be configured");
}

// 没有 v1 历史的域绝不能继承旧版迁移:它是本存储唯一一处依据未经认证的输入清单去写盘的
// 路径,继承它等于凭一份任何人都能放进目录的明文清单触发写入。
void testLegacyMigrationIsOffByDefaultAndWritesNothing()
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    const ConfigurationBackupStoreDomain domain = fixtureDomain("alpha");
    expect(!domain.legacyV1MigrationEnabled,
           "a fixture domain inherited legacy migration by default");
    SharedKeyProvider provider;
    ConfigurationBackupStore store(domain, root.path(), &provider);
    QString error;
    expect(!store.migrateLegacy(QStringLiteral("subject-one"), 1,
                                QStringLiteral("0123abcd"),
                                { QStringLiteral("/tmp/a.json") }, &error),
           "a domain without v1 history performed a legacy migration");
    expect(error == QStringLiteral("fixture-alpha-migration-unsupported"),
           "a refused legacy migration did not say it is unsupported");
    // 而且一个字节都没写:根目录甚至不该被建立起来。拒绝之后留下一个空的根目录会让下一次
    // 清点看到一个它无法解释的形状。
    const QDir rootDir(root.path());
    const QStringList entries =
        rootDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    expect(entries.isEmpty(),
           "a refused legacy migration still wrote something to the root");
    // 工具域相反:它确实有 v1 历史需要搬运,因此这条路径必须仍然开着。
    expect(ConfigurationBackupStore::toolDomain().legacyV1MigrationEnabled,
           "the tool domain lost the legacy migration path it still needs");
}

// 清点是第二个会碰到旧版迁移的入口,而它比 `migrateLegacy` 更危险:那一个必须由调用方主动
// 发起,这一个只要有人往目录里放一份非 v2 清单就会被触发。一个没有 v1 历史的域在这里只能
// 判定为无效证据。把它交给迁移等于凭一份任何人都能写进目录的明文清单去写盘,而清点本来是
// 一个只读动作。
void testInventoryRefusesAForeignManifestInsteadOfMigratingIt()
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    const ConfigurationBackupStoreDomain domain = fixtureDomain("alpha");
    // 一个形状合法的备份目录,里面放一份不是本域写的清单。
    const QString directoryPath = QDir(root.path()).filePath(QStringLiteral("0123abcd"));
    if (!expect(QDir().mkpath(directoryPath), "could not create the backup directory")) {
        return;
    }
    const QString manifestPath = QDir(directoryPath).filePath(domain.manifestName);
    QFile manifest(manifestPath);
    if (!expect(manifest.open(QIODevice::WriteOnly), "could not plant a manifest")) {
        return;
    }
    manifest.write(QByteArrayLiteral("{\"format\":\"someone-elses-format\"}"));
    manifest.close();
    const QByteArray plantedBytes = QByteArrayLiteral(
        "{\"format\":\"someone-elses-format\"}");

    SharedKeyProvider provider;
    ConfigurationBackupStore store(domain, root.path(), &provider);
    const ConfigurationBackupInventoryResult result =
        store.inventory(QStringLiteral("subject-one"), 1,
                        { QStringLiteral("/tmp/a.json") });
    expect(result.state == ConfigurationBackupInventoryState::Invalid,
           "a foreign manifest was not judged invalid evidence");
    expect(result.issue == QStringLiteral("fixture-alpha-manifest-invalid"),
           "a refused foreign manifest did not say the manifest is invalid");
    expect(result.entries.isEmpty(),
           "a refused inventory still reported entries");
    // 而且清点没有写任何东西:既没有留下 pending 清单,也没有改动那份被拒绝的清单。一次
    // 只读动作留下写入痕迹,意味着迁移其实跑过了一部分。
    expect(!QFile::exists(QDir(directoryPath).filePath(domain.pendingName)),
           "a refused inventory left a pending manifest behind");
    QFile after(manifestPath);
    if (expect(after.open(QIODevice::ReadOnly), "could not re-read the manifest")) {
        expect(after.readAll() == plantedBytes,
               "a refused inventory rewrote the manifest it refused");
    }
}

// 每一个进入持久化字节的字符串在任意两个域之间都必须不同。共用其中任何一个都会让跨域分隔
// 出现一个缺口,而缺口的方向是一个域的备份满足另一个域的校验。
void testPersistedStringsArePairwiseDistinct()
{
    const ConfigurationBackupStoreDomain domains[] = {
        ConfigurationBackupStore::toolDomain(),
        ConfigurationBackupStore::extensionStagingDomain(),
        fixtureDomain("alpha"),
        fixtureDomain("beta"),
    };
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            const ConfigurationBackupStoreDomain &a = domains[i];
            const ConfigurationBackupStoreDomain &b = domains[j];
            expect(a.aadPrefix != b.aadPrefix,
                   "two domains share an AAD prefix");
            expect(a.identityDomain != b.identityDomain,
                   "two domains share a manifest identity hash domain");
            expect(a.identityPrefix != b.identityPrefix,
                   "two domains share a manifest identity prefix");
            expect(a.keyScopePrefix != b.keyScopePrefix,
                   "two domains share a key scope prefix");
            expect(a.manifestFormat != b.manifestFormat,
                   "two domains share a manifest format string");
            expect(a.payloadFormat != b.payloadFormat,
                   "two domains share a payload format string");
            expect(a.errorPrefix != b.errorPrefix,
                   "two domains share a diagnostic prefix");
        }
    }
}

void testExtensionStagingDomainIsConfiguredAndBounded()
{
    const ConfigurationBackupStoreDomain extension =
        ConfigurationBackupStore::extensionStagingDomain();
    expect(extension.configured(),
           "the extension staging domain is not fully configured");
    expect(!extension.legacyV1MigrationEnabled,
           "the extension staging domain inherited legacy migration authority");
    expect(extension.aadPrefix.endsWith('\0')
               && extension.identityDomain.endsWith('\0'),
           "the extension staging domain lost its embedded NUL separators");
    expect(extension.subjectJsonKey == QStringLiteral("extension"),
           "the extension staging manifest uses the tool subject key");
    expect(QRegularExpression(extension.subjectPattern)
                   .match(QStringLiteral("skill:my-skill"))
                   .hasMatch(),
           "the extension staging subject grammar rejects a valid extension id");
    expect(!QRegularExpression(extension.subjectPattern)
                    .match(QStringLiteral("codex"))
                    .hasMatch(),
           "the extension staging subject grammar overlaps the tool namespace");

    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    SharedKeyProvider provider;
    ConfigurationBackupStore store(extension, root.path(), &provider);
    ConfigurationBackupSnapshot value;
    value.backupId = QStringLiteral("ext_20260902_120000_0123abcd");
    value.tool = QStringLiteral("skill:my-skill");
    value.createdAt = QDateTime::fromString(
        QStringLiteral("2026-09-02T12:00:00.123Z"), Qt::ISODateWithMs);
    value.files.append({0, true, QByteArrayLiteral("skill bytes")});
    QString error;
    if (expect(store.create(value, &error),
               "the extension staging domain could not create a backup")) {
        ConfigurationBackupSnapshot restored;
        expect(store.read(value.tool, value.backupId, &restored, &error)
                   && restored.files.size() == 1
                   && restored.files.at(0).content
                          == QByteArrayLiteral("skill bytes"),
               "the extension staging domain did not round-trip its payload");
    }
}

// 工具域的每一个字面量都已经随既有备份发布,逐字节固定。两处带内嵌 NUL 的前缀尤其危险:
// 写错会静默改变每一份 AAD 与每一个清单身份,于是既有备份在需要回滚的那一刻才被发现无法
// 解密。这是一个金字符串测试,存在的意义是让将来任何一次"整理"这些常量的改动立刻失败。
void testToolDomainReproducesEveryPublishedLiteral()
{
    const ConfigurationBackupStoreDomain tool = ConfigurationBackupStore::toolDomain();
    expect(tool.configured(), "the tool domain is not fully configured");

    static constexpr char kAad[] = "aegisy-tool-config-backup-manifest/0.2\0";
    static constexpr char kIdentity[] =
        "aegisy-tool-config-backup-manifest-identity/0.1\0";
    // 内嵌 NUL 必须在内:`sizeof - 1` 保留它,而 `QByteArray(const char *)` 会截掉它。
    expect(tool.aadPrefix == QByteArray(kAad, sizeof(kAad) - 1),
           "the tool AAD prefix drifted, invalidating every existing backup");
    expect(tool.aadPrefix.endsWith('\0'),
           "the tool AAD prefix lost its embedded NUL");
    expect(tool.identityDomain == QByteArray(kIdentity, sizeof(kIdentity) - 1),
           "the tool identity domain drifted, invalidating every manifest identity");
    expect(tool.identityDomain.endsWith('\0'),
           "the tool identity domain lost its embedded NUL");

    expect(tool.manifestFormat == QStringLiteral("aegisy-tool-config-backup"),
           "the tool manifest format drifted");
    expect(tool.payloadFormat
               == QStringLiteral("aegisy-tool-config-backup-payload/0.1"),
           "the tool payload format drifted");
    expect(tool.identityPrefix
               == QStringLiteral("configuration-backup-manifest:sha256:"),
           "the tool identity prefix drifted, breaking the activation journal");
    expect(tool.keyScopePrefix
               == QStringLiteral("tool-manager/config-backup-master/v1/"),
           "the tool key scope drifted, orphaning every existing backup key");
    expect(tool.subjectJsonKey == QStringLiteral("tool"),
           "the tool manifest subject key drifted");
    expect(tool.manifestName == QStringLiteral("manifest.json"),
           "the tool manifest file name drifted");
    expect(tool.pendingName == QStringLiteral("manifest.v2.pending"),
           "the tool pending manifest name drifted, breaking crash recovery");
    expect(tool.lockFileName == QStringLiteral(".backup.lock"),
           "the tool lock file name drifted");
    expect(tool.backupIdPattern
               == QStringLiteral("^[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9a-f]{8}$"),
           "the tool backup id grammar drifted");
    expect(tool.errorPrefix == QStringLiteral("configuration-backup"),
           "the tool diagnostic prefix drifted");
    expect(tool.maxFiles == ConfigurationBackupStore::MaxFiles
               && tool.maxFileBytes == ConfigurationBackupStore::MaxFileBytes
               && tool.maxPayloadBytes == ConfigurationBackupStore::MaxPayloadBytes
               && tool.maxManifestBytes == ConfigurationBackupStore::MaxManifestBytes
               && tool.maxBackups == ConfigurationBackupStore::MaxBackups,
           "the tool domain bounds diverged from the published class constants");

    // 主体命名空间不得重叠:某个扩展标识恰好是字面量 `codex` 会让基于主体的分隔消失。
    expect(ConfigurationBackupStore::isValidTool(QStringLiteral("codex")),
           "the tool subject validator stopped accepting a supported target");
    expect(!ConfigurationBackupStore::isValidTool(QStringLiteral("subject-one")),
           "the tool subject validator accepted a foreign domain's subject");
    expect(ConfigurationBackupStore::isValidBackupId(
               QStringLiteral("20260101_120000_001_0123abcd")),
           "the tool backup id validator stopped accepting its own grammar");
    expect(!ConfigurationBackupStore::isValidBackupId(QStringLiteral("0123abcd")),
           "the tool backup id validator accepted a foreign domain's grammar");
}

// 混合主体根:别人主体的完整备份越出作用域而不是错误。按主体清点只返回所查主体的备份,
// 全部验证通过,无任何退化;只有别人主体备份的根对所查主体是 Ready 加空清单(根存在,
// 只是没有你的),绝不是 Empty 伪装也绝不是 Invalid。
void testSubjectScopedInventorySkipsForeignIntactBackups()
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    const ConfigurationBackupStoreDomain domain = fixtureDomain("alpha");
    SharedKeyProvider provider;
    ConfigurationBackupStore store(domain, root.path(), &provider);
    QString error;
    if (!expect(store.create(snapshotFor(QStringLiteral("subject-one"),
                                         QStringLiteral("00000001")), &error)
                && store.create(snapshotFor(QStringLiteral("subject-one"),
                                            QStringLiteral("00000002")), &error)
                && store.create(snapshotFor(QStringLiteral("subject-two"),
                                            QStringLiteral("00000011")), &error),
                "the mixed-subject fixtures could not be created")) {
        return;
    }

    const ConfigurationBackupInventoryResult one =
        store.inventory(QStringLiteral("subject-one"), 0, {});
    if (!expect(one.state == ConfigurationBackupInventoryState::Ready
                    && one.entries.size() == 2,
                "a foreign intact backup degraded the scoped inventory")) {
        return;
    }
    for (const ConfigurationBackupInventoryEntry &entry : one.entries) {
        expect(entry.tool == QStringLiteral("subject-one")
                   && entry.fileCount == 1 && !entry.identity.isEmpty(),
               "the scoped inventory leaked or degraded an entry");
    }
    const ConfigurationBackupInventoryResult two =
        store.inventory(QStringLiteral("subject-two"), 0, {});
    expect(two.state == ConfigurationBackupInventoryState::Ready
               && two.entries.size() == 1
               && two.entries.at(0).backupId == QStringLiteral("00000011"),
           "the second subject does not see exactly its own backup");

    // 只有别人主体备份的根:Ready 加空清单——捕获层据此如实报告"没有既有备份"。
    const ConfigurationBackupInventoryResult three =
        store.inventory(QStringLiteral("subject-three"), 0, {});
    expect(three.state == ConfigurationBackupInventoryState::Ready
               && three.entries.isEmpty(),
           "a root holding only foreign backups was not Ready-but-empty");
}

// 损坏证据绝不因为"可能属于别人"而被静默跳过:foreign 与 corrupt 的分界线是对该条目以
// 它自己的主体做完整验证(目录形状、结构、GCM 认证)。篡改过的 foreign 清单、没有可归类
// 主体的清单、id 与目录名不符的清单,各自如实退化整个结果;foreign 密钥不可得时无法分辨
// intact 与 corrupt,如实 Unavailable 而不是猜。
void testForeignCorruptBackupsStillDegradeScopedInventory()
{
    const ConfigurationBackupStoreDomain domain = fixtureDomain("alpha");

    // A. foreign 清单被篡改(AAD 里的 created_at 被改写,GCM 认证失败)。
    {
        QTemporaryDir root;
        if (!expect(root.isValid(), "temporary directory unavailable")) return;
        SharedKeyProvider provider;
        ConfigurationBackupStore store(domain, root.path(), &provider);
        QString error;
        if (!expect(store.create(snapshotFor(QStringLiteral("subject-one"),
                                             QStringLiteral("00000001")), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-two"),
                                                QStringLiteral("00000011")),
                                    &error),
                    "the tamper fixtures could not be created")) {
            return;
        }
        const QString foreignDir =
            QDir(root.path()).filePath(QStringLiteral("00000011"));
        QByteArray tampered = readPlantedManifest(foreignDir, domain);
        if (!expect(tampered.contains("2026-08-23T12:00:00.123Z"),
                    "the tamper target does not contain the timestamp")) {
            return;
        }
        tampered.replace("2026-08-23T12:00:00.123Z", "2026-08-23T12:00:01.123Z");
        if (!expect(writePlantedManifest(foreignDir, domain, tampered),
                    "the tampered manifest could not be planted")) {
            return;
        }
        const ConfigurationBackupInventoryResult result =
            store.inventory(QStringLiteral("subject-one"), 0, {});
        expect(result.state == ConfigurationBackupInventoryState::Invalid
                   && result.issue == QStringLiteral(
                       "fixture-alpha-authentication-failed")
                   && result.entries.isEmpty(),
               "a tampered foreign manifest was silently skipped");
    }

    // B. 没有可归类主体的清单(外域格式):不是"别人的",是无效证据。
    {
        QTemporaryDir root;
        if (!expect(root.isValid(), "temporary directory unavailable")) return;
        SharedKeyProvider provider;
        ConfigurationBackupStore store(domain, root.path(), &provider);
        QString error;
        if (!expect(store.create(snapshotFor(QStringLiteral("subject-one"),
                                             QStringLiteral("00000001")), &error)
                    && writePlantedManifest(
                        QDir(root.path()).filePath(QStringLiteral("00000021")),
                        domain,
                        QByteArrayLiteral(
                            "{\"format\":\"someone-elses-format\"}")),
                    "the unclassifiable fixtures could not be created")) {
            return;
        }
        const ConfigurationBackupInventoryResult result =
            store.inventory(QStringLiteral("subject-one"), 0, {});
        expect(result.state == ConfigurationBackupInventoryState::Invalid
                   && result.issue == QStringLiteral(
                       "fixture-alpha-manifest-invalid")
                   && result.entries.isEmpty(),
               "an unclassifiable manifest was silently skipped");
    }

    // C. 声称主体合法但清单内 backup_id 与目录名不符:结构校验都过不了,不是
    // foreign-intact。
    {
        QTemporaryDir root;
        if (!expect(root.isValid(), "temporary directory unavailable")) return;
        SharedKeyProvider provider;
        ConfigurationBackupStore store(domain, root.path(), &provider);
        QString error;
        if (!expect(store.create(snapshotFor(QStringLiteral("subject-one"),
                                             QStringLiteral("00000001")), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-two"),
                                                QStringLiteral("00000011")),
                                    &error),
                    "the mismatched fixtures could not be created")) {
            return;
        }
        const QByteArray foreignManifest = readPlantedManifest(
            QDir(root.path()).filePath(QStringLiteral("00000011")), domain);
        if (!expect(writePlantedManifest(
                        QDir(root.path()).filePath(QStringLiteral("00000022")),
                        domain, foreignManifest),
                    "the mismatched manifest could not be planted")) {
            return;
        }
        const ConfigurationBackupInventoryResult result =
            store.inventory(QStringLiteral("subject-one"), 0, {});
        expect(result.state == ConfigurationBackupInventoryState::Invalid
                   && result.issue == QStringLiteral(
                       "fixture-alpha-manifest-invalid")
                   && result.entries.isEmpty(),
               "an id-mismatched foreign manifest was silently skipped");
    }

    // D. foreign 条目的密钥不可得:无法分辨 intact 与 corrupt,如实 Unavailable,
    // 绝不猜成"别人的完整备份"而跳过。
    {
        QTemporaryDir root;
        if (!expect(root.isValid(), "temporary directory unavailable")) return;
        SharedKeyProvider setupProvider;
        ConfigurationBackupStore setup(domain, root.path(), &setupProvider);
        QString error;
        if (!expect(setup.create(snapshotFor(QStringLiteral("subject-one"),
                                             QStringLiteral("00000001")), &error)
                    && setup.create(snapshotFor(QStringLiteral("subject-two"),
                                                QStringLiteral("00000011")),
                                    &error),
                    "the selective-key fixtures could not be created")) {
            return;
        }
        SelectiveKeyProvider selective(
            domain.keyScopePrefix + QStringLiteral("subject-one"));
        ConfigurationBackupStore store(domain, root.path(), &selective);
        const ConfigurationBackupInventoryResult result =
            store.inventory(QStringLiteral("subject-one"), 0, {});
        expect(result.state == ConfigurationBackupInventoryState::Unavailable
                   && result.issue == QStringLiteral(
                       "fixture-alpha-key-unavailable")
                   && result.entries.isEmpty(),
               "an unverifiable foreign backup was guessed instead of degraded");
    }
}

// 上限与作用域的交互:扫描上限(maxBackups 的 4 倍)数的是目录总数——分辨 foreign 与
// corrupt 要求看到每一个目录;而超限即 Invalid 的判定只数作用域内的完整备份。别人主体
// 的完整备份不占所查主体的额度。
void testScopedCeilingCountsOnlyInScopeEntries()
{
    QTemporaryDir root;
    if (!expect(root.isValid(), "temporary directory unavailable")) return;
    const ConfigurationBackupStoreDomain domain = fixtureDomain("alpha");
    // 夹具域:maxBackups = 8,扫描上限 = 32。
    SharedKeyProvider provider;
    ConfigurationBackupStore store(domain, root.path(), &provider);
    QString error;
    for (int i = 0; i < 8; ++i) {
        if (!expect(store.create(
                        snapshotFor(QStringLiteral("subject-one"), idForIndex(i)),
                        &error)
                    && store.create(
                        snapshotFor(QStringLiteral("subject-two"),
                                    idForIndex(0x100 + i)), &error)
                    && store.create(
                        snapshotFor(QStringLiteral("subject-three"),
                                    idForIndex(0x200 + i)), &error),
                "a ceiling fixture backup could not be created")) {
            return;
        }
    }
    // 24 份目录,每个主体 8 份:各方清点都 Ready,别人主体的备份不占额度。
    const ConfigurationBackupInventoryResult one =
        store.inventory(QStringLiteral("subject-one"), 0, {});
    if (!expect(one.state == ConfigurationBackupInventoryState::Ready
                    && one.entries.size() == 8,
                "a full-but-in-limit subject was not Ready in a mixed root")) {
        return;
    }
    // 第 9 份作用域内备份:所查主体超限 → Invalid;别的主体不受影响。
    if (!expect(store.create(snapshotFor(QStringLiteral("subject-one"),
                                         idForIndex(8)), &error),
                "the over-limit fixture could not be created")) {
        return;
    }
    const ConfigurationBackupInventoryResult over =
        store.inventory(QStringLiteral("subject-one"), 0, {});
    expect(over.state == ConfigurationBackupInventoryState::Invalid
               && over.issue == QStringLiteral("fixture-alpha-inventory-invalid")
               && over.entries.isEmpty(),
           "an in-scope over-limit subject was not judged invalid");
    const ConfigurationBackupInventoryResult bystander =
        store.inventory(QStringLiteral("subject-two"), 0, {});
    expect(bystander.state == ConfigurationBackupInventoryState::Ready
               && bystander.entries.size() == 8,
           "a subject's over-limit state leaked into another subject");
    // 目录总数越过扫描上限(33 > 32):一次诚实清点读不完,任何主体的清点都 Invalid,
    // 而不是截断出一份看似完整的清单。
    if (!expect(store.create(snapshotFor(QStringLiteral("subject-four"),
                                         idForIndex(0x300)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x301)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x302)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x303)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x304)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x305)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x306)), &error)
                    && store.create(snapshotFor(QStringLiteral("subject-four"),
                                                idForIndex(0x307)), &error),
                "the scan-ceiling fixtures could not be created")) {
        return;
    }
    const ConfigurationBackupInventoryResult flooded =
        store.inventory(QStringLiteral("subject-two"), 0, {});
    expect(flooded.state == ConfigurationBackupInventoryState::Invalid
               && flooded.issue
                   == QStringLiteral("fixture-alpha-inventory-invalid"),
           "a root past the scan ceiling was not judged invalid");
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    testCrossDomainReadIsRefusedUnderAnIdenticalKey();
    testTheAadPrefixAloneSeparatesTheDomains();
    testTheIdentityDomainAloneSeparatesTheIdentities();
    testUnconfiguredDomainIsRefused();
    testLegacyMigrationIsOffByDefaultAndWritesNothing();
    testInventoryRefusesAForeignManifestInsteadOfMigratingIt();
    testPersistedStringsArePairwiseDistinct();
    testExtensionStagingDomainIsConfiguredAndBounded();
    testToolDomainReproducesEveryPublishedLiteral();
    testSubjectScopedInventorySkipsForeignIntactBackups();
    testForeignCorruptBackupsStillDegradeScopedInventory();
    testScopedCeilingCountsOnlyInScopeEntries();
    if (failures != 0) {
        QTextStream(stderr) << failures
                            << " configuration backup domain guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "configuration backup domain guards passed\n";
    return 0;
}
