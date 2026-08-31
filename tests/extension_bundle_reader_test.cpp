#include "extension_bundle_reader.h"

#include "extension_import_preview.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
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

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written;
}

QByteArray manifestBytes(const QByteArray &components,
                         const QByteArray &extra = QByteArray())
{
    return "{\"id\":\"acme.bundle\",\"name\":\"Acme Bundle\",\"version\":\"1.2.0\","
        + extra + "\"components\":" + components + "}";
}

const QByteArray kTwoComponents =
    "[{\"id\":\"acme.formatter\",\"name\":\"Formatter\",\"type\":\"skill\","
    "\"path\":\"skills/formatter\",\"capabilities\":[\"skill-content\"]},"
    "{\"id\":\"acme.notes\",\"name\":\"Notes\",\"type\":\"asset\","
    "\"path\":\"assets/notes\",\"capabilities\":[]}]";

// 一个结构完整的包：清单加上两个组件各自的内容。
bool buildBundle(const QString &root, const QByteArray &manifest)
{
    return writeFile(root + QStringLiteral("/aegisy-bundle.json"), manifest)
        && writeFile(root + QStringLiteral("/skills/formatter/SKILL.md"),
                     QByteArrayLiteral("# formatter\n"))
        && writeFile(root + QStringLiteral("/assets/notes/readme.txt"),
                     QByteArrayLiteral("notes\n"));
}

// 读取一个包不解包：这一层扫描一个已经存在的目录，绝不创建任何路径。解压就是写盘，而在
// 权限、审批、沙箱与恢复门禁完成之前写盘正是被禁止的那件事。
void testReadsWithoutWriting()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    if (!expect(buildBundle(root, manifestBytes(kTwoComponents)),
                "bundle fixture could not be written")) {
        return;
    }
    QStringList before;
    for (const QString &entry : QDir(root).entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        before.append(entry);
    }
    before.sort();

    const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
    expect(result.state == ExtensionBundleReadState::Ready,
           "a well-formed bundle directory must be readable");
    expect(result.errorCode.isEmpty(),
           "a readable bundle must carry no diagnostic");

    QStringList after;
    for (const QString &entry : QDir(root).entryList(
             QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        after.append(entry);
    }
    after.sort();
    expect(before == after, "reading a bundle created or removed entries on disk");
    expect(!QDir(temporary.path()).exists(QStringLiteral("bundle-extracted")),
           "reading a bundle unpacked it somewhere");

    // 读出来的清单必须能直接喂给预览层，否则这一层没有产出任何可用的东西。
    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(result.manifest);
    expect(preview.state == ExtensionImportPreviewState::Ready,
           "the produced manifest is not previewable");
    expect(preview.components.size() == 2,
           "the preview lost a component the manifest carried");
    expect(!preview.grantsInstallation,
           "the preview claims the read granted an installation");
}

// 每一个摘要都由磁盘上的字节算出。一个能自己声明摘要的包可以描述它并未携带的内容，而人
// 恰恰是按逐组件披露做决定的：屏幕上写着这个组件的内容是 A，实际被引入的是 B。
void testDigestsComeFromDisk()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    if (!expect(buildBundle(root, manifestBytes(kTwoComponents)),
                "bundle fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult first = ExtensionBundleReader::read(root);
    if (!expect(first.state == ExtensionBundleReadState::Ready,
                "the baseline bundle is not readable")) {
        return;
    }
    const QString bundleDigest = first.manifest.contentIdentity;
    QString formatterDigest;
    QString notesDigest;
    for (const ExtensionBundleComponent &component : first.manifest.components) {
        if (component.id == QStringLiteral("acme.formatter")) {
            formatterDigest = component.contentIdentity;
        }
        if (component.id == QStringLiteral("acme.notes")) {
            notesDigest = component.contentIdentity;
        }
    }
    expect(bundleDigest.startsWith(QStringLiteral("extension-content:sha256:")),
           "the bundle digest is not a canonical content identity");
    expect(!formatterDigest.isEmpty() && formatterDigest != notesDigest,
           "two components with different content share a digest");
    expect(formatterDigest != bundleDigest,
           "a component digest collides with the bundle digest");

    // 重复读取必须得到同一个摘要：授权绑定的是这个值。
    const ExtensionBundleReadResult again = ExtensionBundleReader::read(root);
    expect(again.manifest.contentIdentity == bundleDigest,
           "reading the same bundle twice produced different digests");

    // 改一个组件的字节：该组件的摘要与整包摘要都必须变，另一个组件的不变。
    if (!expect(writeFile(root + QStringLiteral("/skills/formatter/SKILL.md"),
                          QByteArrayLiteral("# formatter tampered\n")),
                "the tampered component could not be written")) {
        return;
    }
    const ExtensionBundleReadResult drifted = ExtensionBundleReader::read(root);
    if (!expect(drifted.state == ExtensionBundleReadState::Ready,
                "the drifted bundle is not readable")) {
        return;
    }
    expect(drifted.manifest.contentIdentity != bundleDigest,
           "changing a component's bytes left the bundle digest unchanged");
    for (const ExtensionBundleComponent &component : drifted.manifest.components) {
        if (component.id == QStringLiteral("acme.formatter")) {
            expect(component.contentIdentity != formatterDigest,
                   "changing a component's bytes left its own digest unchanged");
        }
        if (component.id == QStringLiteral("acme.notes")) {
            expect(component.contentIdentity == notesDigest,
                   "changing one component changed another component's digest");
        }
    }
}

// 清单里出现摘要字段一律拒绝，而不是忽略：忽略会让写清单的人以为那个字段生效了，而实际
// 生效的是磁盘上的字节。
void testDeclaredDigestsAreRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    const QByteArray declared =
        "\"contentIdentity\":\"extension-content:sha256:"
        + QByteArray(64, 'a') + "\",";
    if (!expect(buildBundle(root, manifestBytes(kTwoComponents, declared)),
                "bundle fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
    expect(result.state == ExtensionBundleReadState::Invalid,
           "a manifest declaring its own digest was accepted");
    expect(result.errorCode
               == QStringLiteral("extension-bundle-manifest-fields-invalid"),
           "a declared digest does not report a field diagnostic");

    // 组件级别同样如此。
    const QByteArray componentDeclared =
        "[{\"id\":\"acme.formatter\",\"name\":\"Formatter\",\"type\":\"skill\","
        "\"path\":\"skills/formatter\",\"capabilities\":[\"skill-content\"],"
        "\"contentIdentity\":\"extension-content:sha256:" + QByteArray(64, 'b')
        + "\"}]";
    if (!expect(buildBundle(root, manifestBytes(componentDeclared)),
                "the component fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult component = ExtensionBundleReader::read(root);
    expect(component.state == ExtensionBundleReadState::Invalid,
           "a component declaring its own digest was accepted");
    expect(component.errorCode
               == QStringLiteral("extension-bundle-component-fields-invalid"),
           "a declared component digest does not report a field diagnostic");
}

// 不认识的组件类型保留为 Unsupported 并带上原始类型串。丢弃它会让包的实际行为超出预览所
// 描述的范围，而预览层正是依据 Unsupported 决定失败关闭。
void testUnknownTypeSurvivesAsEvidence()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    const QByteArray components =
        "[{\"id\":\"acme.formatter\",\"name\":\"Formatter\",\"type\":\"skill\","
        "\"path\":\"skills/formatter\",\"capabilities\":[\"skill-content\"]},"
        "{\"id\":\"acme.daemon\",\"name\":\"Daemon\",\"type\":\"background-agent\","
        "\"path\":\"assets/notes\",\"capabilities\":[\"process\"]}]";
    if (!expect(buildBundle(root, manifestBytes(components)),
                "bundle fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
    if (!expect(result.state == ExtensionBundleReadState::Ready,
                "an unknown component type must not make the bundle unreadable")) {
        return;
    }
    expect(result.manifest.components.size() == 2,
           "an unknown component type was dropped from the manifest");
    bool sawUnsupported = false;
    for (const ExtensionBundleComponent &component : result.manifest.components) {
        if (component.id != QStringLiteral("acme.daemon")) continue;
        sawUnsupported = true;
        expect(component.kind == ExtensionComponentKind::Unsupported,
               "an unknown component type was classified as something known");
        expect(component.declaredType == QStringLiteral("background-agent"),
               "an unknown component type lost its declared type string");
        expect(component.requestedCapabilities
                   == QStringList{QStringLiteral("process")},
               "an unknown component lost its requested capabilities");
    }
    expect(sawUnsupported, "the unsupported component is absent from the manifest");

    // 失败关闭是预览层的结论，而它只有在证据完整传上去时才成立。
    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(result.manifest);
    expect(preview.state == ExtensionImportPreviewState::FailedClosed,
           "an unknown executable component did not fail the import closed");
    expect(preview.components.size() == 2,
           "the failed-closed preview hid the evidence it refused on");
    expect(preview.anyBeyondReadOnly,
           "a component requesting process capability was not marked");
}

// 能力逐组件原样传递，不做合并或归一。两个组件各自请求"读文件"与"连网"时，汇总看起来与
// 一个组件同时请求两者完全一样，而后者才是真正危险的组合。
void testCapabilitiesStayPerComponent()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    const QByteArray components =
        "[{\"id\":\"acme.reader\",\"name\":\"Reader\",\"type\":\"skill\","
        "\"path\":\"skills/formatter\",\"capabilities\":[\"filesystem-read\"]},"
        "{\"id\":\"acme.caller\",\"name\":\"Caller\",\"type\":\"mcp-server\","
        "\"path\":\"assets/notes\",\"capabilities\":[\"network\"]}]";
    if (!expect(buildBundle(root, manifestBytes(components)),
                "bundle fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
    if (!expect(result.state == ExtensionBundleReadState::Ready,
                "the per-component capability bundle is not readable")) {
        return;
    }
    for (const ExtensionBundleComponent &component : result.manifest.components) {
        expect(component.requestedCapabilities.size() == 1,
               "a component's capability list absorbed its neighbour's request");
        if (component.id == QStringLiteral("acme.reader")) {
            expect(component.requestedCapabilities.first()
                       == QStringLiteral("filesystem-read"),
                   "a component's own capability was rewritten");
        }
    }
    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(result.manifest);
    expect(preview.state == ExtensionImportPreviewState::Ready,
           "read-only components did not produce a ready preview");
    for (const ExtensionComponentPreview &item : preview.components) {
        expect(!item.beyondReadOnly,
               "a read-only component was coloured by its neighbour");
    }

    // 重复声明的能力被拒绝而不是去重：无法判断哪一次声明是有意的。
    const QByteArray duplicated =
        "[{\"id\":\"acme.reader\",\"name\":\"Reader\",\"type\":\"skill\","
        "\"path\":\"skills/formatter\","
        "\"capabilities\":[\"network\",\"network\"]}]";
    if (!expect(buildBundle(root, manifestBytes(duplicated)),
                "the duplicate capability fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult repeated = ExtensionBundleReader::read(root);
    expect(repeated.state == ExtensionBundleReadState::Invalid,
           "a component repeating a capability was accepted");
}

// 归档文件必须先解压才能读，而解压就是写盘。因此只接受目录。
void testArchivesAreRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString archive = temporary.path() + QStringLiteral("/bundle.zip");
    if (!expect(writeFile(archive, QByteArrayLiteral("PK\x03\x04not-really")),
                "the archive fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(archive);
    expect(result.state == ExtensionBundleReadState::Invalid,
           "an archive file was accepted as a bundle");
    expect(result.errorCode
               == QStringLiteral("extension-bundle-root-not-directory"),
           "a refused archive does not name the reason");
    expect(QFile::exists(archive),
           "refusing an archive removed it");
}

// 目录不存在不是错误：还没有包可以导入。把它报成 Invalid 会让界面显示"这个包有问题"，
// 而实际情况是根本没有包。
void testAbsentRootIsEmptyNotInvalid()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(
        temporary.path() + QStringLiteral("/never-created"));
    expect(result.state == ExtensionBundleReadState::Empty,
           "an absent bundle directory was reported as a malformed bundle");
    expect(result.errorCode.isEmpty(),
           "an absent bundle directory carries a defect diagnostic");
    expect(result.manifest.components.isEmpty(),
           "an absent bundle directory produced components");
}

// 符号链接被拒绝而不是跟随：跟随会让摘要覆盖包外的字节，而包外的内容不在人批准的范围里。
void testSymlinksAreRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    if (!expect(buildBundle(root, manifestBytes(kTwoComponents)),
                "bundle fixture could not be written")) {
        return;
    }
    const QString outside = temporary.path() + QStringLiteral("/outside.txt");
    if (!expect(writeFile(outside, QByteArrayLiteral("secret\n")),
                "the outside file could not be written")) {
        return;
    }
    if (!QFile::link(outside, root + QStringLiteral("/skills/formatter/link.txt"))) {
        // 该平台不支持建立符号链接时跳过，而不是把这条约束报成通过。
        QTextStream(stdout) << "note: symlink creation unsupported, case skipped\n";
        return;
    }
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
    expect(result.state == ExtensionBundleReadState::Invalid,
           "a symlink inside the bundle was followed");
    expect(result.errorCode == QStringLiteral("extension-bundle-symlink-invalid"),
           "a refused symlink does not name the reason");
}

// 结构性拒绝：缺清单、空组件列表、重复标识、越界路径、歧义 JSON。
void testStructuralRefusals()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString base = temporary.path();

    const QString noManifest = base + QStringLiteral("/no-manifest");
    if (writeFile(noManifest + QStringLiteral("/skills/formatter/SKILL.md"),
                  QByteArrayLiteral("x\n"))) {
        const ExtensionBundleReadResult result =
            ExtensionBundleReader::read(noManifest);
        expect(result.state == ExtensionBundleReadState::Invalid
                   && result.errorCode
                       == QStringLiteral("extension-bundle-manifest-absent"),
               "a directory with no manifest was accepted");
    }

    struct Case {
        const char *directory;
        QByteArray manifest;
        QString code;
        const char *message;
    };
    const QList<Case> cases{
        {"empty-components", manifestBytes("[]"),
         QStringLiteral("extension-bundle-no-components"),
         "a bundle with no components was accepted"},
        {"duplicate-id",
         manifestBytes("[{\"id\":\"acme.one\",\"name\":\"A\",\"type\":\"skill\","
                       "\"path\":\"skills/formatter\",\"capabilities\":[]},"
                       "{\"id\":\"acme.one\",\"name\":\"B\",\"type\":\"asset\","
                       "\"path\":\"assets/notes\",\"capabilities\":[]}]"),
         QStringLiteral("extension-bundle-component-duplicate"),
         "a repeated component identifier was accepted"},
        {"escaping-path",
         manifestBytes("[{\"id\":\"acme.one\",\"name\":\"A\",\"type\":\"skill\","
                       "\"path\":\"../outside\",\"capabilities\":[]}]"),
         QStringLiteral("extension-bundle-component-path-invalid"),
         "a component path escaping the bundle was accepted"},
        {"absolute-path",
         manifestBytes("[{\"id\":\"acme.one\",\"name\":\"A\",\"type\":\"skill\","
                       "\"path\":\"/etc\",\"capabilities\":[]}]"),
         QStringLiteral("extension-bundle-component-path-invalid"),
         "an absolute component path was accepted"},
        {"duplicate-key",
         QByteArrayLiteral("{\"id\":\"acme.bundle\",\"id\":\"other.bundle\","
                           "\"name\":\"A\",\"version\":\"1.0\",\"components\":"
                           "[{\"id\":\"acme.one\",\"name\":\"A\",\"type\":\"skill\","
                           "\"path\":\"skills/formatter\",\"capabilities\":[]}]}"),
         QStringLiteral("extension-bundle-manifest-invalid"),
         "an ambiguous manifest with a repeated key was accepted"},
        {"missing-field",
         QByteArrayLiteral("{\"id\":\"acme.bundle\",\"name\":\"A\","
                           "\"components\":[{\"id\":\"acme.one\",\"name\":\"A\","
                           "\"type\":\"skill\",\"path\":\"skills/formatter\","
                           "\"capabilities\":[]}]}"),
         QStringLiteral("extension-bundle-manifest-fields-invalid"),
         "a manifest missing a required field was accepted"},
    };
    for (const Case &item : cases) {
        const QString root = base + QLatin1Char('/')
            + QString::fromLatin1(item.directory);
        if (!buildBundle(root, item.manifest)) continue;
        const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
        expect(result.state == ExtensionBundleReadState::Invalid, item.message);
        expect(result.errorCode == item.code, item.message);
    }
}

// 不可展示的文本被拒绝，而判定只有一份：读取器复制一套规则会与呈现层漂移，而漂移意味着
// 读取器放行了预览会拒绝的字符。
void testUnsafeTextIsRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    const QByteArray hostile =
        QByteArrayLiteral("{\"id\":\"acme.bundle\",\"name\":\"Acme\\u202eBundle\","
                          "\"version\":\"1.0\",\"components\":[{\"id\":\"acme.one\","
                          "\"name\":\"A\",\"type\":\"skill\","
                          "\"path\":\"skills/formatter\",\"capabilities\":[]}]}");
    if (!expect(buildBundle(root, hostile),
                "the hostile-name fixture could not be written")) {
        return;
    }
    const ExtensionBundleReadResult result = ExtensionBundleReader::read(root);
    expect(result.state == ExtensionBundleReadState::Invalid,
           "a bundle name carrying a bidirectional override was accepted");
    expect(result.errorCode == QStringLiteral("extension-bundle-manifest-unsafe"),
           "an unsafe bundle name does not name the reason");
}

// 每一段输入都带长度前缀。不带长度的拼接可以让两组不同的输入产生同一个摘要，而摘要正是
// 把"人看到的那份内容"与"实际被引入的内容"绑在一起的唯一手段：两个不同的包算出同一个
// 摘要时，一次针对其中一个的授权会同样适用于另一个。
void testDigestFramingResistsCollision()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    // 两个包的路径与字节在不带分隔的拼接下完全一致："ab" + "c" 与 "a" + "bc"。
    const QByteArray components =
        "[{\"id\":\"acme.one\",\"name\":\"A\",\"type\":\"asset\","
        "\"path\":\"payload\",\"capabilities\":[]}]";
    const QString left = temporary.path() + QStringLiteral("/left");
    const QString right = temporary.path() + QStringLiteral("/right");
    const bool written =
        writeFile(left + QStringLiteral("/aegisy-bundle.json"),
                  manifestBytes(components))
        && writeFile(left + QStringLiteral("/payload/ab"), QByteArrayLiteral("c"))
        && writeFile(right + QStringLiteral("/aegisy-bundle.json"),
                     manifestBytes(components))
        && writeFile(right + QStringLiteral("/payload/a"), QByteArrayLiteral("bc"));
    if (!expect(written, "the collision fixtures could not be written")) return;

    const ExtensionBundleReadResult first = ExtensionBundleReader::read(left);
    const ExtensionBundleReadResult second = ExtensionBundleReader::read(right);
    if (!expect(first.state == ExtensionBundleReadState::Ready
                    && second.state == ExtensionBundleReadState::Ready,
                "the collision fixtures are not readable")) {
        return;
    }
    expect(first.manifest.contentIdentity != second.manifest.contentIdentity,
           "two different bundles produced the same content digest");
    QString firstComponent;
    QString secondComponent;
    for (const ExtensionBundleComponent &component : first.manifest.components) {
        firstComponent = component.contentIdentity;
    }
    for (const ExtensionBundleComponent &component : second.manifest.components) {
        secondComponent = component.contentIdentity;
    }
    expect(!firstComponent.isEmpty() && firstComponent != secondComponent,
           "two different component payloads produced the same digest");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testReadsWithoutWriting();
    testDigestsComeFromDisk();
    testDeclaredDigestsAreRefused();
    testUnknownTypeSurvivesAsEvidence();
    testCapabilitiesStayPerComponent();
    testArchivesAreRefused();
    testAbsentRootIsEmptyNotInvalid();
    testSymlinksAreRefused();
    testStructuralRefusals();
    testUnsafeTextIsRefused();
    testDigestFramingResistsCollision();
    if (failures == 0) {
        QTextStream(stdout) << "extension bundle reader guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
