#include "agent_runtime_client.h"

#include <QCoreApplication>
#include <QProcessEnvironment>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    QProcessEnvironment input;
    input.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
    input.insert(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"),
                QStringLiteral("/tmp/aegisy-workbench"));
    input.insert(QStringLiteral("OPENAI_API_KEY"),
                QStringLiteral("sentinel-openai-api-key"));
    input.insert(QStringLiteral("Aegisy_Auth_Token"),
                QStringLiteral("sentinel-login-jwt"));
    input.insert(QStringLiteral("MY_REFRESH_TOKEN"),
                QStringLiteral("sentinel-refresh-token"));
    input.insert(QStringLiteral("AWS_SECRET_ACCESS_KEY"),
                QStringLiteral("sentinel-cloud-secret"));
    input.insert(QStringLiteral("HTTPS_PROXY"),
                QStringLiteral("https://user:password@example.invalid"));
    input.insert(QStringLiteral("MODEL_NAME"), QStringLiteral("aegisy-coding"));

    const QProcessEnvironment sanitized =
        AgentRuntimeClient::sanitizedSidecarEnvironment(input);
    const bool ok = expect(sanitized.value(QStringLiteral("PATH")) == QStringLiteral("/usr/bin"),
                           "safe environment values were not preserved")
        && expect(sanitized.value(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"))
                      == QStringLiteral("/tmp/aegisy-workbench"),
                  "workbench data root was removed")
        && expect(!sanitized.contains(QStringLiteral("OPENAI_API_KEY")),
                  "OpenAI API key reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("Aegisy_Auth_Token")),
                  "Aegisy login token reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("MY_REFRESH_TOKEN")),
                  "refresh token reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("AWS_SECRET_ACCESS_KEY")),
                  "cloud secret reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("HTTPS_PROXY")),
                  "authenticated proxy reached the sidecar environment")
        && expect(sanitized.value(QStringLiteral("MODEL_NAME")) == QStringLiteral("aegisy-coding"),
                  "ordinary model setting was removed");
    return ok ? 0 : 1;
}
