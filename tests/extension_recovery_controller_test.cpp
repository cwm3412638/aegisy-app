#include "extension_recovery_controller.h"

#include <QCoreApplication>
#include <QSettings>
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

class FakeSecureStore final : public ExtensionEnablementLedgerSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        ++reads;
        if (!value) return ReadState::Invalid;
        if (!available) {
            if (errorCode) *errorCode = QStringLiteral("fake-store-locked");
            return ReadState::Unavailable;
        }
        if (!present) {
            value->clear();
            return ReadState::Missing;
        }
        *value = bytes;
        return invalid ? ReadState::Invalid : ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        ++writes;
        lastWrite = value;
        // 记录每一次授权写入发生时载荷字节是否还在。丢弃的两个阶段无法原子完成,因此顺序
        // 本身就是安全性的一部分,而顺序只能这样被观察到。
        if (observed) {
            payloadPresentAtWrite.append(observed->contains(recordKey));
            authorityAtWrite.append(value);
        }
        if (refuseWrite) {
            if (errorCode) *errorCode = QStringLiteral("fake-store-refused");
            return WriteOutcome::DefiniteFailure;
        }
        if (unknownFromWrite > 0 && writes >= unknownFromWrite) {
            // 让第三阶段（完成提交）的结果变成未知，而前两阶段已经落盘。这是"上一次发布
            // 的结果未知"唯一真实的来源：授权还停在预留阶段，因此当前有效内容无从判断。
            return WriteOutcome::OutcomeUnknown;
        }
        if (!available) return WriteOutcome::OutcomeUnknown;
        // 一个确认了写入却没有真的持久化的后端。这不是假想的故障:它正是"被确认的写入不是
        // 证据"这条规则存在的理由。
        if (!persistWrite) return WriteOutcome::Committed;
        bytes = value;
        present = true;
        invalid = false;
        return WriteOutcome::Committed;
    }

    bool available = true;
    bool present = false;
    bool invalid = false;
    bool refuseWrite = false;
    // 从第 N 次写入起返回"结果未知"。0 表示关闭。
    int unknownFromWrite = 0;
    bool persistWrite = true;
    QByteArray bytes;
    QByteArray lastWrite;
    int reads = 0;
    int writes = 0;
    // 设上之后,每一次授权写入都会记录当时载荷字节是否还在,以及写入的授权内容。
    QSettings *observed = nullptr;
    QString recordKey;
    QList<bool> payloadPresentAtWrite;
    QList<QByteArray> authorityAtWrite;
};

ExtensionEnablementGrant grant(const QString &id, QChar fill)
{
    ExtensionEnablementGrant value;
    value.kind = ExtensionKind::Skill;
    value.id = id;
    value.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, fill);
    value.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, fill);
    return value;
}

// 把一份真实的授权集合写进后端，然后让它变成自相矛盾：这是唯一允许恢复的那种损坏。
struct Fixture {
    QTemporaryDir root;
    FakeSecureStore secure;

    bool valid() const { return root.isValid(); }
    QString settingsPath() const
    {
        return root.filePath(QStringLiteral("grants.ini"));
    }
};

} // namespace

// 恢复只能减少授权，永不增加。任何能产出非空授权集合的恢复路径都是一条制造同意的路径，
// 比它试图修复的损坏更危险。
void testRecoveryOnlyWithdraws(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    // 先真的授权两个扩展，让恢复有东西可以收回。
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a')),
                               grant(QStringLiteral("two"), QLatin1Char('b'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record any grant to withdraw")) {
        return;
    }
    // 现在让载荷自相矛盾：这是 `ClearGrants` 唯一的来源。
    fixture.secure.invalid = true;
    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    const ExtensionRecoveryAssessment assessment = view.assessment;
    if (!expect(assessment.need == ExtensionRecoveryNeed::ClearGrants,
                "a self-contradictory ledger is not offered the clearing path")) {
        return;
    }
    expect(assessment.operatorConfirmationRequired,
           "withdrawing every grant was offered without asking the operator");

    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    // 回传评估时读到的代号。一份自相矛盾的账本没有可信代号，读出来就是 0；回传损坏之前
    // 那个代号会让判定层以为界面看的是另一份账本。
    request.expectedGeneration = view.generation;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    expect(result.outcome == ExtensionRecoveryOutcome::Cleared,
           "a confirmed recovery of a contradictory ledger did not clear it");
    expect(result.survivingGrants == 0,
           "a completed recovery left grants behind");
    // 提交的载荷必须是空的。这是这一层唯一被允许写出去的东西。
    expect(!fixture.secure.lastWrite.contains(QByteArrayLiteral("extension-content")),
           "the recovery wrote a grant payload instead of clearing it");
    expect(result.withdrawsAuthorityOnly,
           "the recovery does not declare that it only withdraws authority");
    // 恢复不碰复核账本：复核记录是事后审计唯一的证据来源，而清掉它并不减少任何授权。
    expect(!result.reviewLedgerTouched,
           "the recovery touched the review ledger, destroying the audit evidence");
    // 事务只能由更上层在确认之后清除。
    expect(!result.clearsTransaction,
           "the recovery closed the transaction by itself");
}

// 一次被确认的写入不是证据。结论只能来自重新读出来的字节：一个确认了写入却没有真的持久化
// 的后端会让"授权已全部收回"成为一句谎报，而操作者会因此不再回来看。
void testAcknowledgedWriteIsNotEvidence(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record a grant to withdraw")) {
        return;
    }
    fixture.secure.invalid = true;
    fixture.secure.persistWrite = false;

    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    // 回传评估时读到的代号。一份自相矛盾的账本没有可信代号，读出来就是 0；回传损坏之前
    // 那个代号会让判定层以为界面看的是另一份账本。
    request.expectedGeneration = view.generation;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    expect(result.outcome == ExtensionRecoveryOutcome::Incomplete,
           "a write that was acknowledged but not persisted was reported as cleared");
    expect(!result.errorCode.isEmpty(),
           "an incomplete recovery carries no diagnostic");
    // 事务必须保持打开：一次没做完的恢复被当成做完了，意味着账本里留着一份没人能解释的
    // 授权而没有人会回来看。
    expect(!result.clearsTransaction,
           "an incomplete recovery closed the transaction anyway");
}

// 可读的账本不得被恢复动作触碰。恢复会清空全部授权，如果它能作用在健康账本上，它就是
// 一条不经审批就撤销一切的路径。
void testHealthyLedgerIsNeverCleared(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record a grant")) {
        return;
    }
    const int writesBefore = fixture.secure.writes;
    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    request.expectedGeneration = view.generation;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    expect(result.outcome == ExtensionRecoveryOutcome::Refused,
           "a healthy ledger was cleared through the recovery path");
    expect(result.errorCode
               == QStringLiteral("extension-recovery-not-required"),
           "clearing a healthy ledger was refused for the wrong reason");
    // 一个字节都不许写。
    expect(fixture.secure.writes == writesBefore,
           "a refused recovery still wrote to the grant ledger");
    // 授权仍然在原处。
    const ExtensionEnablementLedgerStoreResult reread = store.load();
    expect(reread.state == ExtensionEnablementLedgerStoreState::Ready
               && reread.grants.size() == 1,
           "a refused recovery destroyed the grants it was refused from touching");
}

// 读不到内容时什么都不做。清空一份读不到的授权集合会销毁看不见的授权，而这些授权此刻
// 无法被展示给操作者确认。
void testUnreadableLedgerIsNeverCleared(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    fixture.secure.available = false;
    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    const ExtensionRecoveryAssessment assessment = view.assessment;
    expect(assessment.need == ExtensionRecoveryNeed::Blocked,
           "an unreadable ledger was offered a recovery action");
    // 读不出来时绝不要求确认：一个能被确认的动作就是一个会被执行的动作。
    expect(!assessment.operatorConfirmationRequired,
           "an unreadable ledger asked the operator to confirm clearing it");

    const int writesBefore = fixture.secure.writes;
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::Blocked;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    expect(result.outcome == ExtensionRecoveryOutcome::Refused,
           "an unreadable ledger was cleared");
    expect(fixture.secure.writes == writesBefore,
           "an unreadable ledger was written to anyway");
    // 读不出来的账本不报告残留条数：那个数字本身是编出来的。
    expect(result.survivingGrants == 0,
           "an unreadable ledger reported a grant count it could not know");
}

// 界面展示的结论必须与重新评估的结论一致。不一致说明操作者看到的是过期状态，而恢复
// 决定必须针对当下真实的损坏做出。
void testStaleAcknowledgementIsRefused(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record a grant")) {
        return;
    }
    fixture.secure.invalid = true;
    const int writesBefore = fixture.secure.writes;
    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);

    // 操作者看到的是"结果未知"，而当下的真实损坏是"证据自相矛盾"。
    ExtensionRecoveryRequest stale;
    stale.acknowledgedNeed = ExtensionRecoveryNeed::Reconfirm;
    stale.expectedGeneration = view.generation;
    stale.operatorConfirmed = true;
    const ExtensionRecoveryResult staleResult =
        ExtensionRecoveryController::apply(&store, stale);
    expect(staleResult.outcome == ExtensionRecoveryOutcome::Refused,
           "a recovery decided against a stale assessment was executed");
    expect(staleResult.errorCode
               == QStringLiteral("extension-recovery-assessment-stale"),
           "a stale assessment was refused for the wrong reason");
    // 拒绝时也必须带上当下真实的结论：操作者需要知道该重新看什么。
    expect(staleResult.need == ExtensionRecoveryNeed::ClearGrants,
           "a refused recovery does not report the damage that is actually there");

    // 未经确认同样不执行：收回全部授权是一次真实的授权变更，即使方向是减少。
    ExtensionRecoveryRequest unconfirmed;
    unconfirmed.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    unconfirmed.expectedGeneration = view.generation;
    unconfirmed.operatorConfirmed = false;
    const ExtensionRecoveryResult unconfirmedResult =
        ExtensionRecoveryController::apply(&store, unconfirmed);
    expect(unconfirmedResult.outcome == ExtensionRecoveryOutcome::Refused,
           "an unconfirmed recovery cleared every grant");
    expect(unconfirmedResult.errorCode
               == QStringLiteral("extension-recovery-confirmation-required"),
           "an unconfirmed recovery was refused for the wrong reason");

    // 代号过期同样不执行：一次并发的授予不允许被恢复静默覆盖。
    ExtensionRecoveryRequest wrongGeneration;
    wrongGeneration.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    wrongGeneration.expectedGeneration = view.generation + 7;
    wrongGeneration.operatorConfirmed = true;
    const ExtensionRecoveryResult generationResult =
        ExtensionRecoveryController::apply(&store, wrongGeneration);
    expect(generationResult.outcome == ExtensionRecoveryOutcome::Refused,
           "a recovery committing a stale generation overwrote a concurrent grant");
    expect(generationResult.errorCode
               == QStringLiteral("extension-recovery-generation-stale"),
           "a stale generation was refused for the wrong reason");

    expect(fixture.secure.writes == writesBefore,
           "a refused recovery still wrote to the grant ledger");
}

// 上一次发布的结果未知时必须重新读取以确立当下状态，而不是在未知之上写入：那可能覆盖
// 一次其实已经提交的发布。
void testUnknownOutcomeIsNotWrittenOver(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record a grant")) {
        return;
    }
    // 让下一次发布停在"结果未知"上：预留与载荷都已落盘，但完成提交这一步的结果无从确认，
    // 于是授权仍停在预留阶段，当前有效内容未知。
    // 第三阶段（完成提交）以及此后每一次写入都返回未知，因此 `load()` 也无法把预留阶段
    // 解决掉：这才是"上一次发布的结果未知"真正的形状。若让 `load()` 能写成功，它会依据
    // 落盘的载荷字节确定性地完成提交，那是这套三阶段发布本来就该做的事，不是未知。
    fixture.secure.unknownFromWrite = fixture.secure.writes + 2;
    ExtensionEnablementLedgerStoreResult second;
    store.replace({grant(QStringLiteral("two"), QLatin1Char('b'))},
                  acknowledged.generation, &second, &errorCode);

    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    if (!expect(view.assessment.need == ExtensionRecoveryNeed::Reconfirm,
                "an unknown publish outcome is not reported as needing a re-read")) {
        // 最低要求：绝不能被当成"从未授权过"。把未知当成空集合会清掉看不见的授权。
        expect(view.assessment.need != ExtensionRecoveryNeed::None,
               "an uncertain ledger was reported as needing no recovery at all");
        return;
    }
    // 未知的结果绝不要求确认：一个能被确认的动作就是一个会被执行的动作，而正确的动作是
    // 重新读取，不是清空。
    expect(!view.assessment.operatorConfirmationRequired,
           "an unknown publish outcome asked the operator to confirm clearing it");

    // 存储层在读取时会尝试把未决的预留阶段解决掉，那一次写入属于读取路径而不是恢复动作。
    // 因此基线取"一次纯读取花掉多少次写入"，恢复必须不超过它。
    const int beforeLoad = fixture.secure.writes;
    store.load();
    const int loadCost = fixture.secure.writes - beforeLoad;
    const int writesBefore = fixture.secure.writes;
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::Reconfirm;
    request.expectedGeneration = view.generation;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    expect(result.outcome == ExtensionRecoveryOutcome::Refused,
           "an unknown publish outcome was written over instead of re-read");
    expect(result.errorCode
               == QStringLiteral("extension-recovery-reread-required"),
           "an unknown outcome was refused for the wrong reason");
    // 恢复自己一个字节都不许写：在未知之上写入可能覆盖一次其实已经提交的发布。
    expect(fixture.secure.writes - writesBefore <= loadCost,
           "an unknown publish outcome was written over anyway");
}

// 没有存储就是读不出来。这里绝不能退化成"从未授权过"：那会把一次读取失败表述成用户从未
// 要求启用过任何东西。
void testAbsentStoreIsUnreadableNotEmpty()
{
    const ExtensionRecoveryAssessment assessment =
        ExtensionRecoveryController::assess(nullptr).assessment;
    expect(assessment.need == ExtensionRecoveryNeed::Blocked,
           "an absent store was treated as a ledger that never granted anything");
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::Blocked;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(nullptr, request);
    expect(result.outcome == ExtensionRecoveryOutcome::Refused,
           "an absent store produced a recovery outcome other than refusal");
    expect(result.withdrawsAuthorityOnly && !result.reviewLedgerTouched
               && !result.clearsTransaction,
           "a refused recovery does not declare its invariants");
}

// 写入被明确拒绝时不能报成完成，而且诊断必须来自存储层：这一层再编一个代号会让操作者
// 拿着一个查不到出处的东西，而恢复恰好是最需要能查出处的那条路径。
void testRefusedWriteKeepsTheStoreDiagnostic(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record a grant")) {
        return;
    }
    fixture.secure.invalid = true;
    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    fixture.secure.refuseWrite = true;
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    // 回传评估时读到的代号。一份自相矛盾的账本没有可信代号，读出来就是 0；回传损坏之前
    // 那个代号会让判定层以为界面看的是另一份账本。
    request.expectedGeneration = view.generation;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    // 绝不能报成完成。也不能报成"什么都没发生"：一次失败的丢弃从外面看不出是"完全没动"
    // 还是"授权密钥已经销毁、载荷成了孤立载荷"——两种情况都读作 Invalid。因此唯一诚实的
    // 结论是"没做完"，而它要求有人回来看。
    expect(result.outcome == ExtensionRecoveryOutcome::Incomplete,
           "a refused write was reported as a completed or untouched recovery");
    expect(!result.errorCode.isEmpty(),
           "a refused write carries no diagnostic");
    expect(!result.clearsTransaction,
           "a refused write closed the transaction anyway");
}

// 顺序是安全性的一部分:先销毁授权密钥,再删除载荷字节。两次写入不可能原子完成,因此必须
// 选一个安全的中间态。先销毁密钥意味着任何残留的载荷字节从此无法被任何人认证,于是这次
// 清空是不可逆的;反过来先删载荷、密钥仍在,则任何能把那些字节放回去的人都能让被收回的
// 授权复活,而恢复的全部意义就是收回授权。
void testAuthorityIsDestroyedBeforeThePayload(Fixture &fixture)
{
    QSettings settings(fixture.settingsPath(), QSettings::IniFormat);
    ExtensionEnablementLedgerStore store(&fixture.secure, &settings);
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    if (!expect(store.replace({grant(QStringLiteral("one"), QLatin1Char('a'))},
                              0, &acknowledged, &errorCode),
                "the fixture could not record a grant to withdraw")) {
        return;
    }
    const QByteArray keyBeforeRecovery = fixture.secure.bytes;
    fixture.secure.invalid = true;

    const ExtensionRecoveryView view = ExtensionRecoveryController::assess(&store);
    // 从这里开始观察写入顺序:恢复之前的写入属于 fixture 的准备工作。
    fixture.secure.observed = &settings;
    fixture.secure.recordKey = ExtensionEnablementLedgerStore::recordSettingsKey();
    ExtensionRecoveryRequest request;
    request.acknowledgedNeed = ExtensionRecoveryNeed::ClearGrants;
    request.expectedGeneration = view.generation;
    request.operatorConfirmed = true;
    const ExtensionRecoveryResult result =
        ExtensionRecoveryController::apply(&store, request);
    if (!expect(result.outcome == ExtensionRecoveryOutcome::Cleared,
                "the fixture recovery did not complete, so ordering is unobservable")) {
        return;
    }
    if (!expect(!fixture.secure.payloadPresentAtWrite.isEmpty(),
                "the recovery destroyed no authority at all")) {
        return;
    }
    // 第一次授权写入必须发生在载荷字节还在的时候。反过来则说明载荷先被删掉,而旧密钥仍在,
    // 于是任何能把那些字节放回去的人都能让被收回的授权复活。
    expect(fixture.secure.payloadPresentAtWrite.first(),
           "the payload was deleted before the authority key was destroyed");
    // 而且写进去的必须是一把新的密钥,不是原来那把:保留旧密钥意味着残留字节仍然可被认证。
    expect(fixture.secure.authorityAtWrite.first() != keyBeforeRecovery,
           "the recovery kept the old authority key, leaving residual bytes authentic");
    // 载荷字节最终必须消失。
    expect(!settings.contains(ExtensionEnablementLedgerStore::recordSettingsKey()),
           "a completed recovery left the grant payload bytes behind");
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testRecoveryOnlyWithdraws(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testAcknowledgedWriteIsNotEvidence(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testHealthyLedgerIsNeverCleared(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testUnreadableLedgerIsNeverCleared(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testStaleAcknowledgementIsRefused(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testUnknownOutcomeIsNotWrittenOver(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testRefusedWriteKeepsTheStoreDiagnostic(fixture);
    }
    {
        Fixture fixture;
        if (!expect(fixture.valid(), "temporary directory unavailable")) return 1;
        testAuthorityIsDestroyedBeforeThePayload(fixture);
    }
    testAbsentStoreIsUnreadableNotEmpty();
    if (failures != 0) {
        QTextStream(stderr) << failures
                            << " extension recovery controller guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "extension recovery controller guards passed\n";
    return 0;
}
