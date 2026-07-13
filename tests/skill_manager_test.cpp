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

    QString error;
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
