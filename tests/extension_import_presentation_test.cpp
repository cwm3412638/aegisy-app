#include "extension_import_presentation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

QByteArray manifestBytes(const QByteArray &components)
{
    return "{\"id\":\"acme.bundle\",\"name\":\"Acme Bundle\",\"version\":\"1.2.0\","
           "\"components\":" + components + "}";
}

// 一个可执行组件请求了越出只读边界的能力,加一个纯资源组件。
const QByteArray kMixedComponents =
    "[{\"id\":\"acme.formatter\",\"name\":\"Formatter\",\"type\":\"skill\","
    "\"path\":\"skills/formatter\",\"capabilities\":[\"write-files\"]},"
    "{\"id\":\"acme.notes\",\"name\":\"Notes\",\"type\":\"asset\","
    "\"path\":\"assets/notes\",\"capabilities\":[]}]";

bool buildBundle(const QString &root, const QByteArray &manifest)
{
    return writeFile(root + QStringLiteral("/aegisy-bundle.json"), manifest)
        && writeFile(root + QStringLiteral("/skills/formatter/SKILL.md"),
                     QByteArrayLiteral("# formatter\n"))
        && writeFile(root + QStringLiteral("/assets/notes/readme.txt"),
                     QByteArrayLiteral("notes\n"));
}

ExtensionImportDisclosure disclose(const QString &root)
{
    return ExtensionImportPresentation::build(ExtensionBundleReader::read(root));
}

// 披露不导入。这一层与它的界面都不解包、不写盘、不安装、不启用任何东西,而这两个恒假字段
// 是显式暴露的:界面若把"已经看过这个包的内容"说成"已经导入这个包",人会以为磁盘上已经
// 多了一份东西并据此往下走。
void testDisclosureNeverImports()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    if (!expect(buildBundle(root, manifestBytes(kMixedComponents)),
                "bundle fixture could not be written")) {
        return;
    }
    QStringList before = QDir(temporary.path()).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    before.sort();

    const ExtensionImportDisclosure disclosure = disclose(root);
    expect(disclosure.state == ExtensionImportDisclosureState::Ready,
           "a well-formed bundle is not disclosable");
    expect(!disclosure.importsBundle,
           "the disclosure claims it imported the bundle");
    expect(!disclosure.writesToDisk,
           "the disclosure claims it wrote to disk");

    QStringList after = QDir(temporary.path()).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    after.sort();
    expect(before == after, "disclosing a bundle changed the filesystem");

    // 状态文本必须自己说清楚这是一次披露而不是一次导入。
    const QString label =
        ExtensionImportPresentation::stateLabel(disclosure.state);
    expect(label.contains(QStringLiteral("尚未导入")),
           "the ready label does not say nothing was imported");
}

// 每一条被拒绝的路径同样不导入、同样不写盘。把它们留给结构体默认值会让这条规则在源码里
// 没有任何一处可读的声明。
void testEveryRefusalStillClaimsNothing()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;

    struct Case {
        ExtensionBundleReadState read;
        ExtensionImportDisclosureState expected;
    };
    const Case cases[] = {
        {ExtensionBundleReadState::Empty, ExtensionImportDisclosureState::Absent},
        {ExtensionBundleReadState::Unavailable,
         ExtensionImportDisclosureState::Unreadable},
        {ExtensionBundleReadState::Invalid,
         ExtensionImportDisclosureState::Unpresentable},
    };
    for (const Case &item : cases) {
        ExtensionBundleReadResult read;
        read.state = item.read;
        read.errorCode = QStringLiteral("extension-bundle-fixture");
        const ExtensionImportDisclosure disclosure =
            ExtensionImportPresentation::build(read);
        expect(disclosure.state == item.expected,
               "a refused read produced the wrong disclosure state");
        expect(!disclosure.importsBundle,
               "a refused disclosure claims it imported the bundle");
        expect(!disclosure.writesToDisk,
               "a refused disclosure claims it wrote to disk");
        expect(disclosure.components.isEmpty(),
               "a refused disclosure lists components it never read");
    }
}

// 一个读不出来的目录与一个畸形的包要求人做不同的事:一个要去看权限,一个要去修包。把它们
// 并成一个"无效"会把人送去重写一个本来没问题的包。
void testUnreadableIsNotMalformed()
{
    ExtensionBundleReadResult unavailable;
    unavailable.state = ExtensionBundleReadState::Unavailable;
    unavailable.errorCode = QStringLiteral("extension-bundle-manifest-unreadable");
    const ExtensionImportDisclosure unreadable =
        ExtensionImportPresentation::build(unavailable);

    ExtensionBundleReadResult malformed;
    malformed.state = ExtensionBundleReadState::Invalid;
    malformed.errorCode = QStringLiteral("extension-bundle-manifest-fields-invalid");
    const ExtensionImportDisclosure invalid =
        ExtensionImportPresentation::build(malformed);

    expect(unreadable.state != invalid.state,
           "an unreadable bundle is indistinguishable from a malformed one");
    // 每一层的诊断原样带出:这一层再编一个自己的代号会让人拿着一个查不到出处的东西。
    expect(unreadable.errorCode
               == QStringLiteral("extension-bundle-manifest-unreadable"),
           "the read diagnostic was replaced by a locally invented one");
    expect(invalid.errorCode
               == QStringLiteral("extension-bundle-manifest-fields-invalid"),
           "the read diagnostic was replaced by a locally invented one");
    expect(ExtensionImportPresentation::stateLabel(unreadable.state)
               != ExtensionImportPresentation::stateLabel(invalid.state),
           "unreadable and malformed share one label on screen");
}

// 目录不存在不是错误:还没有包可以披露,与一个畸形的包必须区分开。这条路径不带诊断,因为
// 没有任何东西出错。
void testAbsentIsNotAFailure()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const ExtensionImportDisclosure disclosure =
        disclose(temporary.path() + QStringLiteral("/never-created"));
    expect(disclosure.state == ExtensionImportDisclosureState::Absent,
           "an absent bundle directory is reported as a failure");
    expect(disclosure.errorCode.isEmpty(),
           "an absent bundle carries a diagnostic as if something went wrong");
    expect(disclosure.components.isEmpty(),
           "an absent bundle lists components");
}

// 读取失败时绝不构造预览:一次失败读取里的清单是垃圾,对它做预览有可能算出 Ready,于是
// 一个读不出来的包在屏幕上变成一个可以批准的包。
void testFailedReadNeverBecomesReady()
{
    ExtensionBundleReadResult read;
    read.state = ExtensionBundleReadState::Unavailable;
    read.errorCode = QStringLiteral("extension-bundle-manifest-unreadable");
    // 清单本身是完全合法的:唯一让它不能被披露的是读取状态。
    read.manifest.id = QStringLiteral("acme.bundle");
    read.manifest.name = QStringLiteral("Acme Bundle");
    read.manifest.version = QStringLiteral("1.0.0");
    read.manifest.sourceIdentity =
        QStringLiteral("extension-source:sha256:") + QString(64, QLatin1Char('a'));
    read.manifest.contentIdentity =
        QStringLiteral("extension-content:sha256:") + QString(64, QLatin1Char('b'));
    ExtensionBundleComponent component;
    component.kind = ExtensionComponentKind::Skill;
    component.id = QStringLiteral("acme.formatter");
    component.name = QStringLiteral("Formatter");
    component.declaredType = QStringLiteral("skill");
    component.contentIdentity =
        QStringLiteral("extension-content:sha256:") + QString(64, QLatin1Char('c'));
    read.manifest.components.append(component);

    // 先确认这份清单单独拿去预览确实是 Ready:否则这个测试是空的。
    expect(ExtensionImportPreviewBuilder::build(read.manifest).state
               == ExtensionImportPreviewState::Ready,
           "the fixture manifest is not previewable, so the test proves nothing");

    const ExtensionImportDisclosure disclosure =
        ExtensionImportPresentation::build(read);
    expect(disclosure.state == ExtensionImportDisclosureState::Unreadable,
           "a failed read with a valid-looking manifest became approvable");
    expect(disclosure.components.isEmpty(),
           "a failed read disclosed components from an unread manifest");
    expect(disclosure.title.isEmpty(),
           "a failed read disclosed a title from an unread manifest");
}

// 失败关闭保留全部组件证据,包括那个不支持的组件。隐藏它会让没人能判断这个包到底想做
// 什么,而失败关闭不等于把证据一起丢掉。
void testFailedClosedKeepsTheEvidence()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    const QByteArray components =
        "[{\"id\":\"acme.formatter\",\"name\":\"Formatter\",\"type\":\"skill\","
        "\"path\":\"skills/formatter\",\"capabilities\":[\"skill-content\"]},"
        "{\"id\":\"acme.future\",\"name\":\"Future\",\"type\":\"quantum-agent\","
        "\"path\":\"assets/notes\",\"capabilities\":[\"command-execution\"]}]";
    if (!expect(buildBundle(root, manifestBytes(components)),
                "bundle fixture could not be written")) {
        return;
    }
    const ExtensionImportDisclosure disclosure = disclose(root);
    expect(disclosure.state == ExtensionImportDisclosureState::FailedClosed,
           "an unsupported executable component did not fail the import closed");
    expect(disclosure.errorCode
               == QStringLiteral("extension-import-unsupported-component"),
           "a failed-closed import carries no diagnostic");
    expect(disclosure.components.size() == 2,
           "failing closed discarded the component evidence");
    expect(!disclosure.importsBundle && !disclosure.writesToDisk,
           "a failed-closed disclosure claims it imported or wrote something");
    bool sawUnsupported = false;
    for (const ExtensionComponentPreview &item : disclosure.components) {
        if (item.identifier != QStringLiteral("acme.future")) continue;
        sawUnsupported = true;
        expect(item.unsupported,
               "the unsupported component is not marked as such");
        expect(item.declaredType == QStringLiteral("quantum-agent"),
               "the declared type of an unsupported component was discarded");
        expect(item.beyondReadOnly,
               "an unsupported component's beyond-read-only request was hidden");
    }
    expect(sawUnsupported,
           "the unsupported component was dropped from the disclosure");
    // 整包标题仍然要在,否则人看不出这是哪个包失败关闭了。
    expect(disclosure.title == QStringLiteral("Acme Bundle"),
           "a failed-closed disclosure hides which bundle it describes");
}

// 能力逐组件披露,这一层不做任何整包汇总:两个组件各自请求"读文件"与"连网"时,汇总看起来
// 与一个组件同时请求两者完全一样,而后者才是真正危险的组合。
void testCapabilitiesStayPerComponent()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/bundle");
    const QByteArray components =
        "[{\"id\":\"acme.reader\",\"name\":\"Reader\",\"type\":\"skill\","
        "\"path\":\"skills/formatter\",\"capabilities\":[\"write-files\"]},"
        "{\"id\":\"acme.caller\",\"name\":\"Caller\",\"type\":\"command\","
        "\"path\":\"assets/notes\",\"capabilities\":[\"command-execution\"]}]";
    if (!expect(buildBundle(root, manifestBytes(components)),
                "bundle fixture could not be written")) {
        return;
    }
    const ExtensionImportDisclosure disclosure = disclose(root);
    if (!expect(disclosure.state == ExtensionImportDisclosureState::Ready,
                "the mixed-capability bundle is not disclosable")) {
        return;
    }
    if (!expect(disclosure.components.size() == 2,
                "a component was lost from the disclosure")) {
        return;
    }
    for (const ExtensionComponentPreview &item : disclosure.components) {
        expect(item.capabilities.size() == 1,
               "capabilities were merged across components");
        if (item.identifier == QStringLiteral("acme.reader")) {
            expect(item.capabilities.first() == QStringLiteral("write-files"),
                   "a component was attributed another component's capability");
        }
        if (item.identifier == QStringLiteral("acme.caller")) {
            expect(item.capabilities.first() == QStringLiteral("command-execution"),
                   "a component was attributed another component's capability");
        }
    }
    expect(disclosure.anyBeyondReadOnly,
           "a bundle requesting writes and commands is not flagged");
}

// 判定层拒绝清单时,这一层把它的诊断原样带出,并且不留下任何组件行:那些行描述的是一份
// 不能作为决定依据的清单。读取层在共享的那几项检查上比判定层更严,所以一次成功的读取几乎
// 不会走到这里——正因如此这条路径必须在单元边界上直接构造,否则它在任何端到端夹具里都是
// 不可达的,而不可达等于没有被测过。
void testPreviewRefusalPassesThrough()
{
    ExtensionBundleReadResult read;
    read.state = ExtensionBundleReadState::Ready;
    // 读取成功,但清单的标识不能安全展示:拒绝完全来自判定层。
    read.manifest.id = QStringLiteral("../escape");
    read.manifest.name = QStringLiteral("Acme Bundle");
    read.manifest.version = QStringLiteral("1.0.0");
    read.manifest.sourceIdentity =
        QStringLiteral("extension-source:sha256:") + QString(64, QLatin1Char('a'));
    read.manifest.contentIdentity =
        QStringLiteral("extension-content:sha256:") + QString(64, QLatin1Char('b'));
    ExtensionBundleComponent component;
    component.kind = ExtensionComponentKind::Skill;
    component.id = QStringLiteral("acme.formatter");
    component.name = QStringLiteral("Formatter");
    component.declaredType = QStringLiteral("skill");
    component.contentIdentity =
        QStringLiteral("extension-content:sha256:") + QString(64, QLatin1Char('c'));
    read.manifest.components.append(component);

    const ExtensionImportPreview refused =
        ExtensionImportPreviewBuilder::build(read.manifest);
    if (!expect(refused.state == ExtensionImportPreviewState::Unpresentable,
                "the fixture manifest is not refused, so the test proves nothing")) {
        return;
    }

    const ExtensionImportDisclosure disclosure =
        ExtensionImportPresentation::build(read);
    expect(disclosure.state == ExtensionImportDisclosureState::Unpresentable,
           "a bundle the preview refuses became disclosable");
    // 判定层的诊断原样带出。这一层再编一个自己的代号会让人拿着一个查不到出处的东西:
    // 屏幕上的代号在判定层里根本不存在，于是没有任何地方能解释这次拒绝的理由。
    expect(disclosure.errorCode == refused.errorCode,
           "the preview diagnostic was replaced by a locally invented one");
    expect(disclosure.errorCode
               == QStringLiteral("extension-import-id-invalid"),
           "the refusal does not carry the preview's own diagnostic");
    expect(disclosure.components.isEmpty(),
           "a refused disclosure lists components anyway");
    expect(disclosure.title.isEmpty(),
           "a refused disclosure discloses a title it must not stand behind");
    expect(!disclosure.importsBundle && !disclosure.writesToDisk,
           "a refused disclosure claims it imported or wrote something");
}

// 每一个状态都有自己的文本。两个状态共用一句话就等于在屏幕上把它们并成一个。
void testEveryStateIsDistinctOnScreen()
{
    const ExtensionImportDisclosureState states[] = {
        ExtensionImportDisclosureState::Absent,
        ExtensionImportDisclosureState::Ready,
        ExtensionImportDisclosureState::FailedClosed,
        ExtensionImportDisclosureState::Unreadable,
        ExtensionImportDisclosureState::Unpresentable,
    };
    QStringList seen;
    for (const ExtensionImportDisclosureState state : states) {
        const QString label = ExtensionImportPresentation::stateLabel(state);
        expect(!label.isEmpty(), "a disclosure state has no label");
        expect(!seen.contains(label), "two disclosure states share one label");
        seen.append(label);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testDisclosureNeverImports();
    testEveryRefusalStillClaimsNothing();
    testUnreadableIsNotMalformed();
    testAbsentIsNotAFailure();
    testFailedReadNeverBecomesReady();
    testFailedClosedKeepsTheEvidence();
    testCapabilitiesStayPerComponent();
    testPreviewRefusalPassesThrough();
    testEveryStateIsDistinctOnScreen();
    if (failures != 0) {
        QTextStream(stderr) << failures << " extension import presentation guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "extension import presentation guards passed\n";
    return 0;
}
