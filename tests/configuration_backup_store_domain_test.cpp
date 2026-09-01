#include "configuration_backup_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

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
    testToolDomainReproducesEveryPublishedLiteral();
    if (failures != 0) {
        QTextStream(stderr) << failures
                            << " configuration backup domain guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "configuration backup domain guards passed\n";
    return 0;
}
