#ifndef EXTENSION_RECOVERY_CONTROLLER_H
#define EXTENSION_RECOVERY_CONTROLLER_H

#include "extension_enablement_ledger_store.h"
#include "extension_recovery_gate.h"

// 把恢复门禁的判定接到带认证的授权账本上。`ExtensionRecoveryGate` 已经能判定一份损坏的
// 账本应当怎么处理,但判定本身不改动任何持久状态,于是"自相矛盾的账本只能被重建为空集合"
// 这条结论到此为止没有任何东西去执行它。这一层执行它。
//
// **恢复的执行部分只会减少授权,永不增加。** 一份自相矛盾的账本无法被"修复"成它大概曾经
// 持有的授权集合——那是伪造同意。唯一诚实的重建是空集合,而空集合是收回授权。因此这一层
// 提交的载荷永远是空的,并且它是一个恒定值而不是一个从判定里推导出来的结果:任何能产出
// 非空授权集合的恢复路径,都比它试图修复的损坏更危险。
//
// **一次被确认的写入不是证据。** 结论只能来自重新读出来的字节。一个确认了写入却没有真的
// 持久化的后端,会让"授权已全部收回"成为一句谎报,而在恢复这条路径上那句谎报的后果是:
// 操作者以为损坏已经清理完毕,于是不再回来看,而账本里仍然留着一份没人能解释的授权。
//
// **部分完成的恢复不能被当成已完成的恢复。** 重新读取得到 `Ready` 说明仍有授权残留,
// `Invalid` 说明损坏依旧,`Unavailable` 与 `OutcomeUnknown` 说明当下无从判断——这三类都必须
// 让事务保持打开。乐观地关掉事务会让一次没做完的恢复看起来做完了。
//
// **恢复不碰复核账本。** 复核记录是事后审计唯一的证据来源,而清掉它并不减少任何授权:
// 注册表的双重门禁下没有授权就不会启用,所以删复核记录只会销毁证据而不改变安全结论。
// 这与移除相反,而两者都对:移除是人针对一个具体目标做出的决定,恢复是在无法知道过去发生
// 过什么的情况下把授权归零。
//
// 这一层不安装、不下载、不解压、不执行任何东西,也不写复核账本。它读取、判定、提交空集合,
// 再重新读取以确认。
enum class ExtensionRecoveryOutcome {
    // 判定拒绝,或账本可读因而不需要恢复。授权账本一个字节都没动。
    Refused,
    // 授权已全部收回,并且重新读取确认了这一点。
    Cleared,
    // 提交被接受但重新读取无法确认。事务保持打开,必须有人回来看。
    Incomplete,
};

struct ExtensionRecoveryResult {
    ExtensionRecoveryOutcome outcome = ExtensionRecoveryOutcome::Refused;
    // 界面看到的结论。拒绝时也带上,因为操作者需要知道当下真实的损坏是什么。
    ExtensionRecoveryNeed need = ExtensionRecoveryNeed::Blocked;
    // 恒为真:这一层提交的每一个结果都只收回授权,从不授予。
    bool withdrawsAuthorityOnly = true;
    // 恒为假:这一层从不改动复核账本。
    bool reviewLedgerTouched = false;
    // 恒为假:事务只能在重新读取确认之后由更上层清除,不能由这一层乐观关掉。
    bool clearsTransaction = false;
    // 重新读取到的授权账本状态。这是结论的唯一依据。
    ExtensionEnablementLedgerStoreState grantState =
        ExtensionEnablementLedgerStoreState::Invalid;
    qint64 grantGeneration = 0;
    // 重新读取后仍然残留的授权条数。完成时必须是 0。
    int survivingGrants = 0;
    QString errorCode;
};

// 一次只读评估的结果。代号必须与结论一起交给界面,因为恢复请求要把两者一起回传:一份
// 自相矛盾的账本没有可信的代号,读出来就是 0,而界面若自己记着损坏之前那个代号并回传它,
// 判定层会以为界面看的是另一份账本而拒绝这次恢复。代号是"我看到的是哪一份"的证明,不是
// 一个可以从别处推算出来的数字。
struct ExtensionRecoveryView {
    ExtensionRecoveryAssessment assessment;
    qint64 generation = 0;
    ExtensionEnablementLedgerStoreState grantState =
        ExtensionEnablementLedgerStoreState::Invalid;
    // 当下能读到的授权条数。读不出来时为 0,因为那个数字本身是编出来的。
    int visibleGrants = 0;
};

class ExtensionRecoveryController
{
public:
    // 只读地评估一次:界面要先把当下真实的损坏摆给操作者看,才能要求确认。
    static ExtensionRecoveryView assess(
        ExtensionEnablementLedgerStore *grantStore);

    // 执行一次恢复。`request` 必须回传界面当时看到的结论与代号:不一致说明操作者看的是
    // 过期状态,而恢复决定必须针对当下真实的损坏做出。
    static ExtensionRecoveryResult apply(
        ExtensionEnablementLedgerStore *grantStore,
        const ExtensionRecoveryRequest &request);
};

#endif // EXTENSION_RECOVERY_CONTROLLER_H
