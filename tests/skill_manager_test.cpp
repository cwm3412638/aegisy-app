#include "skill_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>

namespace {

int fail(const char *message)
{
    std::cerr << message << '\n';
    return 1;
}

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return fail("temporary directory unavailable");

    SkillManager manager(nullptr, temporary.path() + QStringLiteral("/installed"));
    if (manager.skills().size() != 2) return fail("built-in skills missing");
    if (manager.matchSkill(QStringLiteral("帮我生成一张产品图片")).executor
            != QStringLiteral("image")) return fail("image routing failed");
    if (manager.matchSkill(QStringLiteral("请制作一个公司介绍PPT")).executor
            != QStringLiteral("presentation")) return fail("presentation routing failed");
    if (!manager.matchSkill(QStringLiteral("解释 PPT 是什么")).id.isEmpty()) {
        return fail("presentation explanation should not trigger a skill");
    }
    if (!manager.matchSkill(QStringLiteral("不要生成图片，只解释提示词")).id.isEmpty()) {
        return fail("negative image request should not trigger a skill");
    }
    if (manager.catalogSkills().size() != 35) return fail("skill catalog entries missing");

    QString error;
    if (!manager.installCatalogSkill(QStringLiteral("aegisy.video.ffmpeg"), &error)) {
        return fail("catalog skill install failed");
    }
    const SkillInfo ffmpeg = manager.skill(QStringLiteral("aegisy.video.ffmpeg"));
    if (ffmpeg.id.isEmpty() || !ffmpeg.enabled || !ffmpeg.trusted
            || ffmpeg.executor != QStringLiteral("instruction")) {
        return fail("catalog skill manifest mismatch");
    }
    if (manager.matchSkill(QStringLiteral("帮我压缩并裁剪这个视频")).id
            != QStringLiteral("aegisy.video.ffmpeg")) {
        return fail("instruction skill routing failed");
    }
    if (!manager.skillInstructions(ffmpeg.id).contains(QStringLiteral("ffprobe"))) {
        return fail("instruction skill content missing");
    }
    if (!manager.installCatalogSkill(QStringLiteral("aegisy.design.ui-designer"), &error)) {
        return fail("generated catalog skill install failed");
    }
    const SkillInfo uiDesigner = manager.skill(QStringLiteral("aegisy.design.ui-designer"));
    if (uiDesigner.id.isEmpty() || !uiDesigner.enabled || !uiDesigner.trusted
            || !manager.skillInstructions(uiDesigner.id).contains(QStringLiteral("信息层级"))) {
        return fail("generated catalog skill content mismatch");
    }
    if (manager.matchSkill(QStringLiteral("帮我美化界面并建立设计系统")).id
            != QStringLiteral("aegisy.design.ui-designer")) {
        return fail("generated catalog skill routing failed");
    }
    if (manager.skill(QStringLiteral("aegisy.presentation.create")).version
            != QStringLiteral("1.1.0")) {
        return fail("built-in presentation skill was not upgraded");
    }
    if (!manager.setEnabled(QStringLiteral("aegisy.presentation.create"), false, &error)) {
        return fail("failed to disable presentation skill");
    }
    const QString presentationScript = manager.skill(QStringLiteral("aegisy.presentation.create")).path
        + QStringLiteral("/scripts/create_ppt.py");
    if (!writeFile(presentationScript, QByteArray("stale presentation runtime\n"))) {
        return fail("failed to create stale presentation fixture");
    }
    SkillManager upgradedManager(nullptr, manager.skillsRoot());
    const SkillInfo upgradedPresentation = upgradedManager.skill(
        QStringLiteral("aegisy.presentation.create"));
    QFile upgradedScript(presentationScript);
    if (!upgradedScript.open(QIODevice::ReadOnly)
            || upgradedPresentation.version != QStringLiteral("1.1.0")
            || upgradedPresentation.enabled
            || !upgradedScript.readAll().contains("render_comparison")) {
        return fail("built-in skill update did not preserve state or refresh files");
    }

    if (!manager.setEnabled(QStringLiteral("aegisy.image.generate"), false, &error)) {
        return fail("failed to disable image skill");
    }
    if (!manager.matchSkill(QStringLiteral("帮我生成图片")).id.isEmpty()) {
        return fail("disabled skill still matched");
    }

    const QString imported = temporary.path() + QStringLiteral("/sample-skill");
    if (!writeFile(imported + QStringLiteral("/SKILL.md"), QByteArray(
        "---\nname: sample-skill\ndescription: Sample imported skill\n---\n\n# Sample\n"))
            || !writeFile(imported + QStringLiteral("/scripts/run.js"),
                          QByteArray("console.log('sample');\n"))) {
        return fail("failed to create imported skill fixture");
    }
    if (!manager.installFromDirectory(imported, &error)) return fail("local import failed");
    const SkillInfo sample = manager.skill(QStringLiteral("sample-skill"));
    if (sample.id.isEmpty() || sample.executor != QStringLiteral("instruction")
            || sample.trusted || sample.enabled) {
        return fail("imported skill security defaults mismatch");
    }
    if (!QFileInfo::exists(sample.path + QStringLiteral("/scripts/run.js"))) {
        return fail("imported skill resources missing");
    }

    const QString remoteUrl = qEnvironmentVariable("AEGISY_TEST_SKILL_URL");
    if (!remoteUrl.isEmpty()) {
        if (!manager.installFromUrl(remoteUrl, &error)) return fail("remote skill install failed");
        const SkillInfo remote = manager.skill(QStringLiteral("image-gen"));
        if (remote.id.isEmpty() || remote.enabled || remote.trusted
                || !QFileInfo::exists(remote.path + QStringLiteral("/scripts/node/image-gen.js"))
                || !QFileInfo::exists(remote.path + QStringLiteral("/references/api-recipes.md"))) {
            return fail("remote skill package was incomplete or unsafe by default");
        }
    }
    return 0;
}
