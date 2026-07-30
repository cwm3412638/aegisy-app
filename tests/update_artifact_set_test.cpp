#include "update_artifact_set.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <openssl/evp.h>

#include <cstdio>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr qint64 kNowMs = 1700000000000LL;
constexpr int kMaximumEnvelopeBytes = 256 * 1024;

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

QString hashString(char character)
{
    return QString(64, QChar::fromLatin1(character));
}

QString sparkleSignature()
{
    return QString::fromLatin1(QByteArray(64, 's').toBase64());
}

QJsonObject application(const QString &version, const QString &platform,
                        const QString &architecture)
{
    return {
        {QStringLiteral("version"), version},
        {QStringLiteral("platform"), platform},
        {QStringLiteral("architecture"), architecture},
    };
}

QJsonObject component(const QString &id, const QString &version)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("version"), version},
    };
}

QJsonObject manifest(char hashCharacter, const QString &runtimeVersion,
                     const QString &adapterVersion)
{
    return {
        {QStringLiteral("sha256"), hashString(hashCharacter)},
        {QStringLiteral("runtime"), component(
             QStringLiteral("aegisy-agentd"), runtimeVersion)},
        {QStringLiteral("adapter"), component(
             QStringLiteral("codex-app-server"), adapterVersion)},
    };
}

QJsonObject manifest(const QString &sha256, const QString &runtimeVersion,
                     const QString &adapterVersion)
{
    return {
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("runtime"), component(
             QStringLiteral("aegisy-agentd"), runtimeVersion)},
        {QStringLiteral("adapter"), component(
             QStringLiteral("codex-app-server"), adapterVersion)},
    };
}

QJsonObject sourceArtifactSet(const QString &platform,
                              const QString &architecture)
{
    return {
        {QStringLiteral("release_sequence"), 41.0},
        {QStringLiteral("channel"), QStringLiteral("stable")},
        {QStringLiteral("application"), application(
             QStringLiteral("2.5.2"), platform, architecture)},
        {QStringLiteral("manifest"), manifest(
             '1', QStringLiteral("0.1.0"),
             QStringLiteral("codex-cli 0.144.5"))},
    };
}

QString installerFileName(const QString &platform)
{
    return platform == QStringLiteral("macos")
        ? QStringLiteral("AegisyClient-2.6.0.zip")
        : QStringLiteral("AegisyClientSetup-2.6.0.exe");
}

QJsonObject candidateEnvelope(const QString &platform = QStringLiteral("macos"),
                              const QString &architecture = QStringLiteral("arm64"))
{
    const QString fileName = installerFileName(platform);
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("aegisy-update-artifact-set/0.1")},
        {QStringLiteral("release_sequence"), 42.0},
        {QStringLiteral("published_at_ms"),
         static_cast<double>(kNowMs - 1000)},
        {QStringLiteral("channel"), QStringLiteral("stable")},
        {QStringLiteral("application"), application(
             QStringLiteral("2.6.0"), platform, architecture)},
        {QStringLiteral("installer"), QJsonObject{
             {QStringLiteral("url"),
              QStringLiteral("https://downloads.aegisy.cc/releases/") + fileName},
             {QStringLiteral("file_name"), fileName},
             {QStringLiteral("size_bytes"), 1234567.0},
             {QStringLiteral("sha256"), hashString('3')},
             {QStringLiteral("sparkle_ed_signature"), sparkleSignature()},
         }},
        {QStringLiteral("target_manifest"), manifest(
             '2', QStringLiteral("0.2.0"),
             QStringLiteral("codex-cli 0.145.0"))},
        {QStringLiteral("compatible_sources"), QJsonArray{
             sourceArtifactSet(platform, architecture),
         }},
        {QStringLiteral("signature"), QString()},
    };
}

UpdateArtifactSet::InstalledArtifactSet installedArtifactSet(
    const QString &platform = QStringLiteral("macos"),
    const QString &architecture = QStringLiteral("arm64"))
{
    UpdateArtifactSet::InstalledArtifactSet installed;
    installed.releaseSequence = 41;
    installed.channel = QStringLiteral("stable");
    installed.applicationVersion = QStringLiteral("2.5.2");
    installed.platform = platform;
    installed.architecture = architecture;
    installed.manifestSha256 = hashString('1');
    installed.runtimeId = QStringLiteral("aegisy-agentd");
    installed.runtimeVersion = QStringLiteral("0.1.0");
    installed.adapterId = QStringLiteral("codex-app-server");
    installed.adapterVersion = QStringLiteral("codex-cli 0.144.5");
    return installed;
}

QByteArray encodedEnvelope(const QJsonObject &envelope)
{
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

class SigningKey
{
public:
    ~SigningKey()
    {
        EVP_PKEY_free(m_key);
    }

    bool initialize()
    {
        EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!context) return false;
        const bool generated = EVP_PKEY_keygen_init(context) == 1
            && EVP_PKEY_keygen(context, &m_key) == 1;
        EVP_PKEY_CTX_free(context);
        if (!generated || !m_key) return false;

        QByteArray publicKey(32, '\0');
        size_t publicKeySize = static_cast<size_t>(publicKey.size());
        if (EVP_PKEY_get_raw_public_key(
                m_key,
                reinterpret_cast<unsigned char *>(publicKey.data()),
                &publicKeySize) != 1
            || publicKeySize != 32) {
            return false;
        }
        m_publicKeyBase64 = publicKey.toBase64();
        return true;
    }

    bool sign(QJsonObject *envelope) const
    {
        envelope->insert(QStringLiteral("signature"), QString());
        QString errorCode;
        const QByteArray payload = UpdateArtifactSet::signaturePayload(
            *envelope, &errorCode);
        if (payload.isEmpty() || !errorCode.isEmpty()) return false;

        EVP_MD_CTX *context = EVP_MD_CTX_new();
        size_t signatureSize = 0;
        const bool measured = context
            && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, m_key) == 1
            && EVP_DigestSign(
                   context, nullptr, &signatureSize,
                   reinterpret_cast<const unsigned char *>(payload.constData()),
                   static_cast<size_t>(payload.size())) == 1;
        if (!measured || signatureSize != 64) {
            EVP_MD_CTX_free(context);
            return false;
        }
        QByteArray signature(static_cast<int>(signatureSize), '\0');
        const bool signedPayload = EVP_DigestSign(
            context,
            reinterpret_cast<unsigned char *>(signature.data()),
            &signatureSize,
            reinterpret_cast<const unsigned char *>(payload.constData()),
            static_cast<size_t>(payload.size())) == 1;
        EVP_MD_CTX_free(context);
        if (!signedPayload || signatureSize != 64) return false;
        envelope->insert(QStringLiteral("signature"),
                         QString::fromLatin1(signature.toBase64()));
        return true;
    }

    const QByteArray &publicKeyBase64() const
    {
        return m_publicKeyBase64;
    }

private:
    EVP_PKEY *m_key = nullptr;
    QByteArray m_publicKeyBase64;
};

QString authorityPlatform()
{
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#else
    return QStringLiteral("macos");
#endif
}

QString authorityArchitecture()
{
#ifdef Q_OS_WIN
    return QStringLiteral("x86_64");
#else
    return QStringLiteral("arm64");
#endif
}

QString authorityRuntimeFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("aegisy-agentd.exe");
#else
    return QStringLiteral("aegisy-agentd");
#endif
}

QString authorityAdapterFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("codex.exe");
#else
    return QStringLiteral("codex");
#endif
}

QString authorityInstallerFileName(const QString &version)
{
#ifdef Q_OS_WIN
    return QStringLiteral("AegisyClientSetup-") + version + QStringLiteral(".exe");
#else
    return QStringLiteral("AegisyClient-") + version + QStringLiteral(".zip");
#endif
}

QByteArray writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        return {};
    }
    file.close();
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool copyExecutable(const QString &source, const QString &destination)
{
    if (!QFile::copy(source, destination)) return false;
    const QFileDevice::Permissions permissions = QFile::permissions(destination)
        | QFileDevice::ExeOwner | QFileDevice::ExeUser
        | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    return QFile::setPermissions(destination, permissions);
}

bool createHardLink(const QString &existingPath, const QString &linkPath)
{
#ifdef Q_OS_WIN
    const QString nativeExisting = QDir::toNativeSeparators(existingPath);
    const QString nativeLink = QDir::toNativeSeparators(linkPath);
    return CreateHardLinkW(
        reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        reinterpret_cast<LPCWSTR>(nativeExisting.utf16()), nullptr) != 0;
#else
    return ::link(QFile::encodeName(existingPath).constData(),
                  QFile::encodeName(linkPath).constData()) == 0;
#endif
}

QJsonObject signedSetEnvelope(
    quint64 releaseSequence,
    const QString &applicationVersion,
    const QString &manifestSha256,
    const QString &runtimeVersion,
    const QString &adapterVersion,
    quint64 sourceSequence,
    const QString &sourceApplicationVersion,
    const QString &sourceManifestSha256,
    const QString &sourceRuntimeVersion,
    const QString &sourceAdapterVersion)
{
    const QString platform = authorityPlatform();
    const QString architecture = authorityArchitecture();
    const QString fileName = authorityInstallerFileName(applicationVersion);
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("aegisy-update-artifact-set/0.1")},
        {QStringLiteral("release_sequence"), static_cast<double>(releaseSequence)},
        {QStringLiteral("published_at_ms"),
         static_cast<double>(kNowMs - 1000)},
        {QStringLiteral("channel"), QStringLiteral("stable")},
        {QStringLiteral("application"), application(
             applicationVersion, platform, architecture)},
        {QStringLiteral("installer"), QJsonObject{
             {QStringLiteral("url"),
              QStringLiteral("https://downloads.aegisy.cc/releases/") + fileName},
             {QStringLiteral("file_name"), fileName},
             {QStringLiteral("size_bytes"), 1234567.0},
             {QStringLiteral("sha256"), hashString('3')},
             {QStringLiteral("sparkle_ed_signature"), sparkleSignature()},
         }},
        {QStringLiteral("target_manifest"), manifest(
             manifestSha256, runtimeVersion, adapterVersion)},
        {QStringLiteral("compatible_sources"), QJsonArray{
             QJsonObject{
                 {QStringLiteral("release_sequence"),
                  static_cast<double>(sourceSequence)},
                 {QStringLiteral("channel"), QStringLiteral("stable")},
                 {QStringLiteral("application"), application(
                      sourceApplicationVersion, platform, architecture)},
                 {QStringLiteral("manifest"), manifest(
                      sourceManifestSha256, sourceRuntimeVersion,
                      sourceAdapterVersion)},
             },
         }},
        {QStringLiteral("signature"), QString()},
    };
}

int currentInstallationChildMain(const QStringList &arguments)
{
    if (arguments.size() != 4) return 2;
    QCoreApplication::setApplicationVersion(QStringLiteral("2.5.2"));
    const QByteArray publicKey = arguments.at(2).toLatin1();
    const UpdateArtifactSet::InstalledAuthorityResult installed =
        UpdateArtifactSet::verifyCurrentInstallationAuthority(
            publicKey, kNowMs, QStringLiteral("stable"));
    if (!installed.ok) {
        std::fprintf(stderr, "current installation authority failed: %s\n",
                     installed.errorCode.toUtf8().constData());
        return 1;
    }

    QFile candidateFile(arguments.at(3));
    if (!candidateFile.open(QIODevice::ReadOnly)
        || candidateFile.size() <= 0
        || candidateFile.size() > kMaximumEnvelopeBytes) {
        return 1;
    }
    const QByteArray candidate = candidateFile.read(kMaximumEnvelopeBytes + 1);
    if (candidate.size() != candidateFile.size() || !candidateFile.atEnd()) {
        return 1;
    }
    const UpdateArtifactSet::Decision decision = UpdateArtifactSet::verifyCandidate(
        candidate, publicKey, kNowMs, installed.authority,
        QStringLiteral("stable"), 41);
    return decision.state == UpdateArtifactSet::State::Compatible
            && decision.candidateCompatible
            && !decision.downloadAuthorized
            && !decision.installAuthorized
            && decision.installedAuthorityIdentity
                == installed.authority.authorityIdentity()
        ? 0 : 1;
}

bool productionInstallationLayoutTest(
    const SigningKey &key,
    const QByteArray &receiptBytes,
    const QByteArray &manifestBytes,
    const QByteArray &runtimeBytes,
    const QByteArray &adapterBytes,
    const QByteArray &candidateBytes)
{
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    Q_UNUSED(key);
    Q_UNUSED(receiptBytes);
    Q_UNUSED(manifestBytes);
    Q_UNUSED(runtimeBytes);
    Q_UNUSED(adapterBytes);
    Q_UNUSED(candidateBytes);
    return true;
#else
    QTemporaryDir directory(
        QDir::tempPath()
        + QStringLiteral("/aegisy-current-install-\u9a8c\u8bc1-XXXXXX"));
    if (!expect(directory.isValid(),
                "production installation fixture directory is unavailable")) {
        return false;
    }

    QString artifactRoot = directory.path();
#ifdef Q_OS_MACOS
    artifactRoot = QDir(directory.path()).filePath(
        QStringLiteral("AegisyClient.app/Contents/MacOS"));
#endif
    if (!expect(QDir().mkpath(artifactRoot),
                "production installation fixture layout could not be created")) {
        return false;
    }
#ifdef Q_OS_WIN
    const QString applicationName = QStringLiteral("AegisyClient.exe");
#else
    const QString applicationName = QStringLiteral("AegisyClient");
#endif
    const QString applicationPath = QDir(artifactRoot).filePath(applicationName);
    const QString candidatePath = QDir(artifactRoot).filePath(
        QStringLiteral("candidate-update.json"));
    if (!expect(copyExecutable(QCoreApplication::applicationFilePath(),
                               applicationPath),
                "production AegisyClient fixture could not be copied")
        || !expect(!writeBytes(
                        QDir(artifactRoot).filePath(authorityRuntimeFileName()),
                        runtimeBytes).isEmpty(),
                   "production Runtime fixture could not be written")
        || !expect(!writeBytes(
                        QDir(artifactRoot).filePath(authorityAdapterFileName()),
                        adapterBytes).isEmpty(),
                   "production adapter fixture could not be written")
        || !expect(!writeBytes(
                        QDir(artifactRoot).filePath(
                            QStringLiteral("aegisy-agentd.manifest.json")),
                        manifestBytes).isEmpty(),
                   "production Manifest fixture could not be written")
        || !expect(!writeBytes(
                        QDir(artifactRoot).filePath(
                            QStringLiteral("aegisy-update-artifact-set.json")),
                        receiptBytes).isEmpty(),
                   "production receipt fixture could not be written")
        || !expect(!writeBytes(candidatePath, candidateBytes).isEmpty(),
                   "production candidate fixture could not be written")) {
        return false;
    }

    QProcess child;
    child.setProgram(applicationPath);
    child.setArguments({
        QStringLiteral("--verify-current-installation"),
        QString::fromLatin1(key.publicKeyBase64()),
        candidatePath,
    });
#ifdef Q_OS_WIN
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("PATH"),
        QFileInfo(QCoreApplication::applicationFilePath()).absolutePath()
            + QLatin1Char(';') + environment.value(QStringLiteral("PATH")));
    child.setProcessEnvironment(environment);
#endif
    child.setProcessChannelMode(QProcess::SeparateChannels);
    child.start();
    const bool started = child.waitForStarted(10000);
    const bool finished = started && child.waitForFinished(30000);
    if (!finished) {
        child.kill();
        child.waitForFinished();
    }
    if (!started || !finished || child.exitStatus() != QProcess::NormalExit
        || child.exitCode() != 0) {
        const QByteArray error = child.readAllStandardError();
        if (!error.isEmpty()) {
            std::fprintf(stderr, "production fixture stderr: %s\n",
                         error.constData());
        }
        return expect(false,
                      "fixed production installation authority was rejected");
    }
    return true;
#endif
}

bool expectInvalid(const QJsonObject &envelope, const SigningKey &key,
                   const UpdateArtifactSet::InstalledArtifactSet &installed,
                   const QString &expectedCode, const char *message)
{
    const UpdateArtifactSet::Decision decision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(envelope), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    return expect(decision.state == UpdateArtifactSet::State::Invalid
                      && !decision.candidateCompatible
                      && !decision.downloadAuthorized
                      && !decision.installAuthorized
                      && decision.errorCode == expectedCode
                      && decision.artifactSetIdentity.isEmpty()
                      && decision.installedArtifactSetIdentity.isEmpty()
                      && decision.compatibilityEvaluationIdentity.isEmpty(),
                  message);
}

bool expectIncompatible(const QJsonObject &envelope, const SigningKey &key,
                        const UpdateArtifactSet::InstalledArtifactSet &installed,
                        const QString &selectedChannel,
                        const QString &expectedCode, const char *message)
{
    const UpdateArtifactSet::Decision decision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(envelope), key.publicKeyBase64(), kNowMs, installed,
            selectedChannel,
            installed.releaseSequence);
    return expect(decision.state == UpdateArtifactSet::State::Incompatible
                      && !decision.candidateCompatible
                      && !decision.downloadAuthorized
                      && !decision.installAuthorized
                      && decision.errorCode == expectedCode
                      && decision.artifactSetIdentity.startsWith(
                          QStringLiteral("update-artifact-set:sha256:"))
                      && decision.installedArtifactSetIdentity.startsWith(
                          QStringLiteral("installed-artifact-set:sha256:"))
                      && decision.compatibilityEvaluationIdentity.startsWith(
                          QStringLiteral("update-artifact-set-evaluation:sha256:"))
                      && decision.evaluatedSelectedChannel == selectedChannel
                      && decision.evaluatedAcceptedReleaseSequenceHighWater
                          == installed.releaseSequence
                      && decision.evaluatedAtMs
                          == static_cast<quint64>(kNowMs),
                  message);
}

bool expectRawInvalid(const QByteArray &envelopeJson, const SigningKey &key,
                      const QString &expectedCode, const char *message)
{
    const UpdateArtifactSet::Decision decision =
        UpdateArtifactSet::verifyCandidate(
            envelopeJson, key.publicKeyBase64(), kNowMs, installedArtifactSet(),
            QStringLiteral("stable"), 41);
    return expect(decision.state == UpdateArtifactSet::State::Invalid
                      && !decision.candidateCompatible
                      && !decision.downloadAuthorized
                      && !decision.installAuthorized
                      && decision.errorCode == expectedCode
                      && decision.artifactSetIdentity.isEmpty()
                      && decision.installedArtifactSetIdentity.isEmpty()
                      && decision.compatibilityEvaluationIdentity.isEmpty(),
                  message);
}

void replaceInstaller(QJsonObject *envelope, const QString &key,
                      const QJsonValue &value)
{
    QJsonObject installer = envelope->value(QStringLiteral("installer")).toObject();
    installer.insert(key, value);
    envelope->insert(QStringLiteral("installer"), installer);
}

void replaceSource(QJsonObject *envelope, int index, const QJsonObject &source)
{
    QJsonArray sources = envelope->value(
        QStringLiteral("compatible_sources")).toArray();
    sources.replace(index, source);
    envelope->insert(QStringLiteral("compatible_sources"), sources);
}

bool validCandidateTests(const SigningKey &key)
{
    bool ok = true;
    QJsonObject macos = candidateEnvelope();
    ok = expect(key.sign(&macos), "could not sign macOS candidate") && ok;
    const UpdateArtifactSet::Decision macosDecision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(macos), key.publicKeyBase64(), kNowMs,
            installedArtifactSet(),
            QStringLiteral("stable"), 41);
    ok = expect(macosDecision.state == UpdateArtifactSet::State::Compatible
                    && macosDecision.candidateCompatible
                    && !macosDecision.downloadAuthorized
                    && !macosDecision.installAuthorized
                    && macosDecision.errorCode.isEmpty()
                    && macosDecision.evaluatedSelectedChannel
                        == QStringLiteral("stable")
                    && macosDecision.evaluatedAcceptedReleaseSequenceHighWater == 41
                    && macosDecision.evaluatedAtMs
                        == static_cast<quint64>(kNowMs)
                    && macosDecision.targetReleaseSequence == 42
                    && macosDecision.matchedSourceReleaseSequence == 41
                    && macosDecision.platform == QStringLiteral("macos")
                    && macosDecision.architecture == QStringLiteral("arm64")
                    && macosDecision.installerFileName.endsWith(
                        QStringLiteral(".zip"))
                    && macosDecision.targetManifestSha256 == hashString('2')
                    && macosDecision.targetRuntimeId
                        == QStringLiteral("aegisy-agentd")
                    && macosDecision.targetRuntimeVersion
                        == QStringLiteral("0.2.0")
                    && macosDecision.targetAdapterId
                        == QStringLiteral("codex-app-server")
                    && macosDecision.targetAdapterVersion
                        == QStringLiteral("codex-cli 0.145.0"),
                "valid macOS candidate was not accepted exactly") && ok;

    QJsonObject windows = candidateEnvelope(
        QStringLiteral("windows"), QStringLiteral("x86_64"));
    ok = expect(key.sign(&windows), "could not sign Windows candidate") && ok;
    const UpdateArtifactSet::Decision windowsDecision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(windows), key.publicKeyBase64(), kNowMs,
            installedArtifactSet(QStringLiteral("windows"),
                                 QStringLiteral("x86_64")),
            QStringLiteral("stable"), 41);
    ok = expect(windowsDecision.state == UpdateArtifactSet::State::Compatible
                    && windowsDecision.candidateCompatible
                    && !windowsDecision.downloadAuthorized
                    && !windowsDecision.installAuthorized
                    && windowsDecision.installerFileName.endsWith(
                        QStringLiteral(".exe"))
                    && windowsDecision.platform == QStringLiteral("windows")
                    && windowsDecision.architecture == QStringLiteral("x86_64"),
                "valid Windows candidate was not accepted exactly") && ok;
    return ok;
}

bool canonicalPayloadTests(const SigningKey &key)
{
    QJsonObject envelope = candidateEnvelope();
    if (!expect(key.sign(&envelope), "could not sign canonical candidate")) {
        return false;
    }
    QString errorCode;
    const QByteArray payload = UpdateArtifactSet::signaturePayload(
        envelope, &errorCode);
    QByteArray expected = QByteArrayLiteral(
        "aegisy-update-artifact-set/0.1\n"
        "release_sequence=42\n"
        "published_at_ms=1699999999000\n"
        "channel=stable\n"
        "application.version=2.6.0\n"
        "application.platform=macos\n"
        "application.architecture=arm64\n"
        "installer.url=https://downloads.aegisy.cc/releases/AegisyClient-2.6.0.zip\n"
        "installer.file_name=AegisyClient-2.6.0.zip\n"
        "installer.size_bytes=1234567\n"
        "installer.sha256=");
    expected += hashString('3').toLatin1();
    expected += QByteArrayLiteral("\ninstaller.sparkle_ed_signature=");
    expected += sparkleSignature().toLatin1();
    expected += QByteArrayLiteral("\ntarget_manifest.sha256=");
    expected += hashString('2').toLatin1();
    expected += QByteArrayLiteral(
        "\ntarget_manifest.runtime.id=aegisy-agentd\n"
        "target_manifest.runtime.version=0.2.0\n"
        "target_manifest.adapter.id=codex-app-server\n"
        "target_manifest.adapter.version=codex-cli 0.145.0\n"
        "compatible_sources.count=1\n"
        "compatible_sources.0.release_sequence=41\n"
        "compatible_sources.0.channel=stable\n"
        "compatible_sources.0.application.version=2.5.2\n"
        "compatible_sources.0.application.platform=macos\n"
        "compatible_sources.0.application.architecture=arm64\n"
        "compatible_sources.0.manifest.sha256=");
    expected += hashString('1').toLatin1();
    expected += QByteArrayLiteral(
        "\ncompatible_sources.0.manifest.runtime.id=aegisy-agentd\n"
        "compatible_sources.0.manifest.runtime.version=0.1.0\n"
        "compatible_sources.0.manifest.adapter.id=codex-app-server\n"
        "compatible_sources.0.manifest.adapter.version=codex-cli 0.144.5\n");

    bool ok = expect(errorCode.isEmpty() && payload == expected,
                     "canonical artifact-set payload drifted");
    QJsonObject differentSignature = envelope;
    differentSignature.insert(QStringLiteral("signature"),
                              QStringLiteral("not-a-signature"));
    ok = expect(UpdateArtifactSet::signaturePayload(differentSignature)
                    == expected,
                "outer signature changed the signed payload") && ok;

    const UpdateArtifactSet::Decision decision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(envelope), key.publicKeyBase64(), kNowMs,
            installedArtifactSet(),
            QStringLiteral("stable"), 41);
    const QString expectedIdentity = QStringLiteral("update-artifact-set:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            expected, QCryptographicHash::Sha256).toHex()));
    QJsonObject fixedVectorEnvelope = candidateEnvelope();
    fixedVectorEnvelope.insert(
        QStringLiteral("signature"),
        QStringLiteral(
            "++Uw/0759BqsMC+vEZlvGRiwdAiCFLvN60R+6PQFWn78V/"
            "dRyyg95y/yuIZL8uqxbXTfy9cjRHoO036CjiINCA=="));
    const UpdateArtifactSet::Decision fixedVectorDecision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(fixedVectorEnvelope),
            QByteArrayLiteral("11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="),
            kNowMs, installedArtifactSet(), QStringLiteral("stable"), 41);
    ok = expect(fixedVectorDecision.state
                        == UpdateArtifactSet::State::Compatible
                    && fixedVectorDecision.candidateCompatible
                    && !fixedVectorDecision.downloadAuthorized
                    && !fixedVectorDecision.installAuthorized
                    && fixedVectorDecision.artifactSetIdentity == expectedIdentity,
                "external fixed Ed25519 vector was rejected") && ok;
    QByteArray installedPayload = QByteArrayLiteral(
        "aegisy-installed-artifact-set/0.1\n"
        "release_sequence=41\n"
        "channel=stable\n"
        "application.version=2.5.2\n"
        "application.platform=macos\n"
        "application.architecture=arm64\n"
        "manifest.sha256=");
    installedPayload += hashString('1').toLatin1();
    installedPayload += QByteArrayLiteral(
        "\nmanifest.runtime.id=aegisy-agentd\n"
        "manifest.runtime.version=0.1.0\n"
        "manifest.adapter.id=codex-app-server\n"
        "manifest.adapter.version=codex-cli 0.144.5\n");
    const QString expectedInstalledIdentity = QStringLiteral(
        "installed-artifact-set:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            installedPayload, QCryptographicHash::Sha256).toHex()));
    QByteArray evaluationPayload = QByteArrayLiteral(
        "aegisy-update-artifact-set-evaluation/0.1\n"
        "artifact_set.identity=");
    evaluationPayload += expectedIdentity.toUtf8();
    evaluationPayload += QByteArrayLiteral("\ninstalled.identity=");
    evaluationPayload += expectedInstalledIdentity.toUtf8();
    evaluationPayload += QByteArrayLiteral("\nverification_key.sha256=");
    evaluationPayload += QCryptographicHash::hash(
        QByteArray::fromBase64(key.publicKeyBase64()),
        QCryptographicHash::Sha256).toHex();
    evaluationPayload += QByteArrayLiteral(
        "\nselected_channel=stable\n"
        "accepted_release_sequence_high_water=41\n"
        "evaluated_at_ms=1700000000000\n");
    const QString expectedEvaluationIdentity = QStringLiteral(
        "update-artifact-set-evaluation:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            evaluationPayload, QCryptographicHash::Sha256).toHex()));
    ok = expect(decision.artifactSetIdentity == expectedIdentity
                    && decision.installedArtifactSetIdentity
                        == expectedInstalledIdentity
                    && decision.compatibilityEvaluationIdentity
                        == expectedEvaluationIdentity,
                "compatibility evaluation identity did not bind every input") && ok;

    const UpdateArtifactSet::Decision laterDecision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(envelope), key.publicKeyBase64(), kNowMs + 1,
            installedArtifactSet(), QStringLiteral("stable"), 41);
    ok = expect(laterDecision.state == UpdateArtifactSet::State::Compatible
                    && laterDecision.compatibilityEvaluationIdentity
                        != decision.compatibilityEvaluationIdentity,
                "evaluation identity did not bind evaluation time") && ok;
    return ok;
}

bool signatureAndFieldTamperTests(const SigningKey &key)
{
    QJsonObject envelope = candidateEnvelope();
    if (!expect(key.sign(&envelope), "could not sign tamper candidate")) return false;
    const UpdateArtifactSet::InstalledArtifactSet installed = installedArtifactSet();
    bool ok = true;

    QJsonObject tampered = envelope;
    tampered.insert(QStringLiteral("channel"), QStringLiteral("beta"));
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-signature-invalid"),
                       "channel tamper was not rejected by signature") && ok;

    tampered = envelope;
    replaceInstaller(&tampered, QStringLiteral("sha256"), hashString('4'));
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-signature-invalid"),
                       "installer tamper was not rejected by signature") && ok;

    tampered = envelope;
    QJsonObject source = tampered.value(
        QStringLiteral("compatible_sources")).toArray().at(0).toObject();
    QJsonObject sourceManifest = source.value(QStringLiteral("manifest")).toObject();
    sourceManifest.insert(QStringLiteral("sha256"), hashString('4'));
    source.insert(QStringLiteral("manifest"), sourceManifest);
    replaceSource(&tampered, 0, source);
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-signature-invalid"),
                       "source manifest tamper was not rejected by signature") && ok;

    tampered = envelope;
    QByteArray signature = QByteArray::fromBase64(
        tampered.value(QStringLiteral("signature")).toString().toLatin1());
    signature[0] = static_cast<char>(signature.at(0) ^ 0x01);
    tampered.insert(QStringLiteral("signature"),
                    QString::fromLatin1(signature.toBase64()));
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-signature-invalid"),
                       "outer signature tamper was not rejected") && ok;

    tampered = envelope;
    tampered.insert(QStringLiteral("unexpected"), true);
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-fields-invalid"),
                       "unknown outer field was accepted") && ok;

    QByteArray duplicateKeyJson = encodedEnvelope(envelope);
    duplicateKeyJson.insert(1, QByteArrayLiteral("\"channel\":\"stable\","));
    const UpdateArtifactSet::Decision duplicateKeyDecision =
        UpdateArtifactSet::verifyCandidate(
            duplicateKeyJson, key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(duplicateKeyDecision.state == UpdateArtifactSet::State::Invalid
                    && !duplicateKeyDecision.candidateCompatible
                    && !duplicateKeyDecision.downloadAuthorized
                    && !duplicateKeyDecision.installAuthorized
                    && duplicateKeyDecision.errorCode
                        == QStringLiteral("artifact-set-json-invalid"),
                "duplicate decoded JSON key was accepted") && ok;

    tampered = envelope;
    QJsonObject installer = tampered.value(QStringLiteral("installer")).toObject();
    installer.insert(QStringLiteral("unexpected"), true);
    tampered.insert(QStringLiteral("installer"), installer);
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "unknown installer field was accepted") && ok;

    tampered = envelope;
    QJsonObject targetManifest = tampered.value(
        QStringLiteral("target_manifest")).toObject();
    QJsonObject runtime = targetManifest.value(QStringLiteral("runtime")).toObject();
    runtime.insert(QStringLiteral("unexpected"), true);
    targetManifest.insert(QStringLiteral("runtime"), runtime);
    tampered.insert(QStringLiteral("target_manifest"), targetManifest);
    ok = expectInvalid(tampered, key, installed,
                       QStringLiteral("artifact-set-component-invalid"),
                       "unknown component field was accepted") && ok;
    return ok;
}

bool compatibilityTests(const SigningKey &key)
{
    const UpdateArtifactSet::InstalledArtifactSet installed = installedArtifactSet();
    bool ok = true;

    QJsonObject candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("channel"), QStringLiteral("beta"));
    ok = expect(key.sign(&candidate), "could not sign channel candidate") && ok;
    ok = expectIncompatible(candidate, key, installed, QStringLiteral("stable"),
                            QStringLiteral("artifact-set-channel-incompatible"),
                            "channel mismatch was accepted") && ok;

    candidate = candidateEnvelope(QStringLiteral("windows"),
                                  QStringLiteral("x86_64"));
    ok = expect(key.sign(&candidate), "could not sign platform candidate") && ok;
    ok = expectIncompatible(candidate, key, installed, QStringLiteral("stable"),
                            QStringLiteral("artifact-set-platform-incompatible"),
                            "platform mismatch was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("release_sequence"), 41.0);
    QJsonObject olderSource = sourceArtifactSet(
        QStringLiteral("macos"), QStringLiteral("arm64"));
    olderSource.insert(QStringLiteral("release_sequence"), 40.0);
    olderSource.insert(QStringLiteral("application"), application(
        QStringLiteral("2.4.0"), QStringLiteral("macos"),
        QStringLiteral("arm64")));
    candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{olderSource});
    ok = expect(key.sign(&candidate), "could not sign non-newer candidate") && ok;
    ok = expectIncompatible(candidate, key, installed, QStringLiteral("stable"),
                            QStringLiteral("artifact-set-not-newer"),
                            "non-newer candidate was accepted") && ok;

    QVector<QJsonObject> sourceMismatches;
    QJsonObject source = sourceArtifactSet(
        QStringLiteral("macos"), QStringLiteral("arm64"));
    source.insert(QStringLiteral("channel"), QStringLiteral("beta"));
    sourceMismatches.append(source);

    source = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    source.insert(QStringLiteral("application"), application(
        QStringLiteral("2.5.1"), QStringLiteral("macos"),
        QStringLiteral("arm64")));
    sourceMismatches.append(source);

    source = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    QJsonObject sourceManifest = source.value(QStringLiteral("manifest")).toObject();
    sourceManifest.insert(QStringLiteral("sha256"), hashString('4'));
    source.insert(QStringLiteral("manifest"), sourceManifest);
    sourceMismatches.append(source);

    source = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    sourceManifest = source.value(QStringLiteral("manifest")).toObject();
    sourceManifest.insert(QStringLiteral("runtime"), component(
        QStringLiteral("aegisy-agentd"), QStringLiteral("0.1.1")));
    source.insert(QStringLiteral("manifest"), sourceManifest);
    sourceMismatches.append(source);

    source = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    sourceManifest = source.value(QStringLiteral("manifest")).toObject();
    sourceManifest.insert(QStringLiteral("adapter"), component(
        QStringLiteral("codex-app-server"),
        QStringLiteral("codex-cli 0.144.4")));
    source.insert(QStringLiteral("manifest"), sourceManifest);
    sourceMismatches.append(source);

    for (const QJsonObject &mismatch : sourceMismatches) {
        candidate = candidateEnvelope();
        candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{mismatch});
        ok = expect(key.sign(&candidate), "could not sign source mismatch") && ok;
        ok = expectIncompatible(candidate, key, installed, QStringLiteral("stable"),
                                QStringLiteral("artifact-set-source-incompatible"),
                                "source identity mismatch was accepted") && ok;
    }

    candidate = candidateEnvelope();
    ok = expect(key.sign(&candidate), "could not sign installed-state candidate") && ok;
    QVector<UpdateArtifactSet::InstalledArtifactSet> installedMismatches;
    UpdateArtifactSet::InstalledArtifactSet installedMismatch = installed;
    installedMismatch.releaseSequence = 40;
    installedMismatches.append(installedMismatch);
    installedMismatch = installed;
    installedMismatch.channel = QStringLiteral("beta");
    installedMismatches.append(installedMismatch);
    installedMismatch = installed;
    installedMismatch.applicationVersion = QStringLiteral("2.5.1");
    installedMismatches.append(installedMismatch);
    installedMismatch = installed;
    installedMismatch.manifestSha256 = hashString('4');
    installedMismatches.append(installedMismatch);
    installedMismatch = installed;
    installedMismatch.runtimeVersion = QStringLiteral("0.1.1");
    installedMismatches.append(installedMismatch);
    installedMismatch = installed;
    installedMismatch.adapterVersion = QStringLiteral("codex-cli 0.144.4");
    installedMismatches.append(installedMismatch);
    for (const UpdateArtifactSet::InstalledArtifactSet &mismatch
         : installedMismatches) {
        ok = expectIncompatible(
                 candidate, key, mismatch, QStringLiteral("stable"),
                 QStringLiteral("artifact-set-source-incompatible"),
                 "installed artifact-set field mismatch was accepted") && ok;
    }

    UpdateArtifactSet::InstalledArtifactSet invalidInstalled = installed;
    invalidInstalled.runtimeId = QStringLiteral("other-runtime");
    ok = expectInvalid(candidate, key, invalidInstalled,
                       QStringLiteral("installed-artifact-set-invalid"),
                       "invalid installed artifact set was accepted") && ok;

    const UpdateArtifactSet::Decision invalidChannel =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("nightly"), installed.releaseSequence);
    ok = expect(invalidChannel.state == UpdateArtifactSet::State::Invalid
                    && invalidChannel.errorCode
                        == QStringLiteral("selected-update-channel-invalid"),
                "invalid selected channel was accepted") && ok;

    const UpdateArtifactSet::Decision replay =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), 42);
    ok = expect(replay.state == UpdateArtifactSet::State::Incompatible
                    && !replay.candidateCompatible
                    && !replay.downloadAuthorized
                    && !replay.installAuthorized
                    && replay.errorCode
                        == QStringLiteral("artifact-set-sequence-replay"),
                "candidate at the accepted high-water mark was accepted") && ok;

    const UpdateArtifactSet::Decision invalidHighWater =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), 40);
    ok = expect(invalidHighWater.state == UpdateArtifactSet::State::Invalid
                    && !invalidHighWater.candidateCompatible
                    && !invalidHighWater.downloadAuthorized
                    && !invalidHighWater.installAuthorized
                    && invalidHighWater.errorCode
                        == QStringLiteral("artifact-set-high-water-invalid"),
                "high-water below the installed release was accepted") && ok;
    return ok;
}

bool sourceStructureTests(const SigningKey &key)
{
    const UpdateArtifactSet::InstalledArtifactSet installed = installedArtifactSet();
    bool ok = true;
    QJsonObject candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-sources-invalid"),
                       "empty source set was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("release_sequence"), 100.0);
    QJsonArray tooManySources;
    for (int index = 1; index <= 65; ++index) {
        QJsonObject source = sourceArtifactSet(
            QStringLiteral("macos"), QStringLiteral("arm64"));
        source.insert(QStringLiteral("release_sequence"),
                      static_cast<double>(index));
        tooManySources.append(source);
    }
    candidate.insert(QStringLiteral("compatible_sources"), tooManySources);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-sources-invalid"),
                       "over-limit source set was accepted") && ok;

    candidate = candidateEnvelope();
    QJsonObject first = sourceArtifactSet(
        QStringLiteral("macos"), QStringLiteral("arm64"));
    QJsonObject duplicate = first;
    candidate.insert(QStringLiteral("compatible_sources"),
                     QJsonArray{first, duplicate});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-source-sequence-invalid"),
                       "duplicate source sequence was accepted") && ok;

    candidate = candidateEnvelope();
    first.insert(QStringLiteral("release_sequence"), 40.0);
    duplicate.insert(QStringLiteral("release_sequence"), 39.0);
    candidate.insert(QStringLiteral("compatible_sources"),
                     QJsonArray{first, duplicate});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-source-sequence-invalid"),
                       "out-of-order source sequence was accepted") && ok;

    candidate = candidateEnvelope();
    first = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    first.insert(QStringLiteral("release_sequence"), 42.0);
    candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{first});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-source-sequence-invalid"),
                       "target-or-newer source sequence was accepted") && ok;

    candidate = candidateEnvelope();
    first = sourceArtifactSet(QStringLiteral("windows"), QStringLiteral("x86_64"));
    candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{first});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-source-target-invalid"),
                       "cross-platform source entry was accepted") && ok;

    candidate = candidateEnvelope();
    first = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    first.insert(QStringLiteral("application"), application(
        QStringLiteral("2.6.0"), QStringLiteral("macos"),
        QStringLiteral("arm64")));
    candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{first});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-source-target-invalid"),
                       "source with target application version was accepted") && ok;

    candidate = candidateEnvelope();
    first = sourceArtifactSet(QStringLiteral("macos"), QStringLiteral("arm64"));
    first.insert(QStringLiteral("unexpected"), true);
    candidate.insert(QStringLiteral("compatible_sources"), QJsonArray{first});
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-source-invalid"),
                       "unknown source field was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("release_sequence"), 65.0);
    QJsonArray maximumSources;
    for (int sequence = 1; sequence <= 64; ++sequence) {
        QJsonObject source = sourceArtifactSet(
            QStringLiteral("macos"), QStringLiteral("arm64"));
        source.insert(QStringLiteral("release_sequence"),
                      static_cast<double>(sequence));
        if (sequence != 41) {
            source.insert(QStringLiteral("application"), application(
                QStringLiteral("1.0.%1").arg(sequence),
                QStringLiteral("macos"), QStringLiteral("arm64")));
        }
        maximumSources.append(source);
    }
    candidate.insert(QStringLiteral("compatible_sources"), maximumSources);
    ok = expect(key.sign(&candidate), "could not sign maximum-source candidate") && ok;
    const UpdateArtifactSet::Decision maximumSourceDecision =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(maximumSourceDecision.state
                        == UpdateArtifactSet::State::Compatible
                    && maximumSourceDecision.candidateCompatible
                    && !maximumSourceDecision.downloadAuthorized
                    && !maximumSourceDecision.installAuthorized
                    && maximumSourceDecision.matchedSourceReleaseSequence == 41,
                "maximum compatible source set was rejected") && ok;
    return ok;
}

bool rawJsonBoundaryTests(const SigningKey &key)
{
    QJsonObject candidate = candidateEnvelope();
    if (!expect(key.sign(&candidate), "could not sign raw JSON candidate")) {
        return false;
    }
    const QByteArray encoded = encodedEnvelope(candidate);
    bool ok = true;

    QByteArray bom = QByteArray::fromHex("efbbbf");
    bom += encoded;
    ok = expectRawInvalid(bom, key, QStringLiteral("artifact-set-json-invalid"),
                          "UTF-8 BOM was accepted") && ok;

    QByteArray invalidUtf8 = QByteArrayLiteral("{\"value\":\"");
    invalidUtf8.append(static_cast<char>(0xff));
    invalidUtf8 += QByteArrayLiteral("\"}");
    ok = expectRawInvalid(invalidUtf8, key,
                          QStringLiteral("artifact-set-json-invalid"),
                          "invalid UTF-8 was accepted") && ok;

    QByteArray escapedDuplicate = encoded;
    escapedDuplicate.insert(1, QByteArrayLiteral(
        "\"\\u0063hannel\":\"stable\","));
    ok = expectRawInvalid(escapedDuplicate, key,
                          QStringLiteral("artifact-set-json-invalid"),
                          "escaped duplicate JSON key was accepted") && ok;

    QByteArray loneSurrogate = encoded;
    loneSurrogate.insert(1, QByteArrayLiteral("\"value\":\"\\uD800\","));
    ok = expectRawInvalid(loneSurrogate, key,
                          QStringLiteral("artifact-set-json-invalid"),
                          "lone surrogate was accepted") && ok;

    QByteArray excessiveDepth = QByteArrayLiteral("{\"value\":");
    excessiveDepth.append(140, '[');
    excessiveDepth.append('0');
    excessiveDepth.append(140, ']');
    excessiveDepth.append('}');
    ok = expectRawInvalid(excessiveDepth, key,
                          QStringLiteral("artifact-set-json-invalid"),
                          "excessive JSON depth was accepted") && ok;

    QByteArray excessiveNodes = QByteArrayLiteral("{\"value\":[");
    for (int index = 0; index < 65536; ++index) {
        if (index != 0) excessiveNodes.append(',');
        excessiveNodes.append('0');
    }
    excessiveNodes += QByteArrayLiteral("]}");
    ok = expect(excessiveNodes.size() < kMaximumEnvelopeBytes,
                "node-limit fixture exceeded raw JSON size limit") && ok;
    ok = expectRawInvalid(excessiveNodes, key,
                          QStringLiteral("artifact-set-json-invalid"),
                          "excessive JSON node count was accepted") && ok;

    QByteArray exactBoundary = encoded;
    exactBoundary.append(kMaximumEnvelopeBytes - exactBoundary.size(), ' ');
    const UpdateArtifactSet::Decision exactBoundaryDecision =
        UpdateArtifactSet::verifyCandidate(
            exactBoundary, key.publicKeyBase64(), kNowMs,
            installedArtifactSet(), QStringLiteral("stable"), 41);
    ok = expect(exactBoundaryDecision.state
                        == UpdateArtifactSet::State::Compatible
                    && exactBoundaryDecision.candidateCompatible
                    && !exactBoundaryDecision.downloadAuthorized
                    && !exactBoundaryDecision.installAuthorized,
                "exact 256 KiB candidate was rejected") && ok;
    exactBoundary.append(' ');
    ok = expectRawInvalid(exactBoundary, key,
                          QStringLiteral("artifact-set-json-size-invalid"),
                          "candidate above 256 KiB was accepted") && ok;
    return ok;
}

bool installerAndValueTests(const SigningKey &key)
{
    const UpdateArtifactSet::InstalledArtifactSet installed = installedArtifactSet();
    const QString fileName = installerFileName(QStringLiteral("macos"));
    const QVector<QString> invalidUrls{
        QStringLiteral("http://downloads.aegisy.cc/releases/") + fileName,
        QStringLiteral("https://user@downloads.aegisy.cc/releases/") + fileName,
        QStringLiteral("https://downloads.aegisy.cc:444/releases/") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/") + fileName
            + QStringLiteral("?token=value"),
        QStringLiteral("https://downloads.aegisy.cc/releases/") + fileName
            + QStringLiteral("?"),
        QStringLiteral("https://downloads.aegisy.cc/releases/") + fileName
            + QStringLiteral("#fragment"),
        QStringLiteral("https://downloads.aegisy.cc/releases/") + fileName
            + QStringLiteral("#"),
        QStringLiteral("https://downloads.aegisy.cc/releases//") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/./") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/%2e%2e/") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/%252E%252E/")
            + fileName,
        QStringLiteral("https://downloads.aegisy.cc/%72eleases/") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases%2f") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases%5c/") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/%00/") + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/%E2%80%AE/")
            + fileName,
        QStringLiteral("https://downloads.aegisy.cc/releases/other.zip"),
    };
    bool ok = true;
    for (const QString &url : invalidUrls) {
        QJsonObject candidate = candidateEnvelope();
        replaceInstaller(&candidate, QStringLiteral("url"), url);
        ok = expectInvalid(candidate, key, installed,
                           QStringLiteral("artifact-set-installer-url-invalid"),
                           "ambiguous or unsafe installer URL was accepted") && ok;
    }

    QJsonObject candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("url"),
                     QStringLiteral("https://downloads.aegisy.cc:443/releases/")
                         + fileName);
    ok = expect(key.sign(&candidate), "could not sign explicit-port candidate") && ok;
    const UpdateArtifactSet::Decision explicitPort =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(explicitPort.state == UpdateArtifactSet::State::Compatible
                    && explicitPort.candidateCompatible
                    && !explicitPort.downloadAuthorized
                    && !explicitPort.installAuthorized,
                "explicit HTTPS port 443 was rejected") && ok;

    candidate = candidateEnvelope(
        QStringLiteral("windows"), QStringLiteral("x86_64"));
    replaceInstaller(&candidate, QStringLiteral("file_name"),
                     QStringLiteral("CON.exe"));
    replaceInstaller(&candidate, QStringLiteral("url"),
                     QStringLiteral("https://downloads.aegisy.cc/releases/CON.exe"));
    ok = expectInvalid(candidate, key,
                       installedArtifactSet(QStringLiteral("windows"),
                                            QStringLiteral("x86_64")),
                       QStringLiteral("artifact-set-installer-invalid"),
                       "Windows reserved installer file name was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("file_name"),
                     QStringLiteral("AegisyClient-2.6.0.dmg"));
    replaceInstaller(&candidate, QStringLiteral("url"),
                     QStringLiteral("https://downloads.aegisy.cc/releases/"
                                    "AegisyClient-2.6.0.dmg"));
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "wrong installer extension was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("sha256"), hashString('A'));
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "uppercase installer hash was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("sha256"),
                     hashString('3') + QLatin1Char('\n'));
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "newline-terminated installer hash was accepted") && ok;

    candidate = candidateEnvelope();
    QJsonObject targetApplication = candidate.value(
        QStringLiteral("application")).toObject();
    targetApplication.insert(QStringLiteral("version"),
                             QStringLiteral("2.6.0\n"));
    candidate.insert(QStringLiteral("application"), targetApplication);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-application-invalid"),
                       "newline-terminated application version was accepted") && ok;

    candidate = candidateEnvelope();
    QJsonObject newlineManifest = candidate.value(
        QStringLiteral("target_manifest")).toObject();
    QJsonObject newlineAdapter = newlineManifest.value(
        QStringLiteral("adapter")).toObject();
    newlineAdapter.insert(QStringLiteral("version"),
                          QStringLiteral("codex-cli 0.145.0\n"));
    newlineManifest.insert(QStringLiteral("adapter"), newlineAdapter);
    candidate.insert(QStringLiteral("target_manifest"), newlineManifest);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-component-invalid"),
                       "newline-terminated adapter version was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("sparkle_ed_signature"),
                     QStringLiteral("AAAA"));
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "wrong-length Sparkle signature was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("size_bytes"), 0.0);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "zero-size installer was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("size_bytes"), 1.0);
    ok = expect(key.sign(&candidate), "could not sign minimum-size candidate") && ok;
    const UpdateArtifactSet::Decision minimumSize =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(minimumSize.state == UpdateArtifactSet::State::Compatible
                    && minimumSize.installerSizeBytes == 1
                    && !minimumSize.downloadAuthorized
                    && !minimumSize.installAuthorized,
                "minimum installer size boundary was rejected") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("size_bytes"),
                     static_cast<double>(2ULL * 1024 * 1024 * 1024 + 1));
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-installer-invalid"),
                       "over-limit installer was accepted") && ok;

    candidate = candidateEnvelope();
    replaceInstaller(&candidate, QStringLiteral("size_bytes"),
                     static_cast<double>(2ULL * 1024 * 1024 * 1024));
    ok = expect(key.sign(&candidate), "could not sign max-size candidate") && ok;
    const UpdateArtifactSet::Decision maximumSize =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(maximumSize.state == UpdateArtifactSet::State::Compatible
                    && maximumSize.installerSizeBytes
                        == 2ULL * 1024 * 1024 * 1024,
                "maximum installer size boundary was rejected") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("release_sequence"), 42.5);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-json-invalid"),
                       "fractional release sequence was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("release_sequence"), 9007199254740992.0);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-json-invalid"),
                       "non-JSON-safe release sequence was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("release_sequence"), 9007199254740991.0);
    ok = expect(key.sign(&candidate), "could not sign safe-integer candidate") && ok;
    const UpdateArtifactSet::Decision safeInteger =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(safeInteger.state == UpdateArtifactSet::State::Compatible
                    && safeInteger.targetReleaseSequence == 9007199254740991ULL,
                "maximum JSON-safe release sequence was rejected") && ok;

    const UpdateArtifactSet::Decision oversizedJson =
        UpdateArtifactSet::verifyCandidate(
            QByteArray(256 * 1024 + 1, ' '), key.publicKeyBase64(), kNowMs,
            installed, QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(oversizedJson.state == UpdateArtifactSet::State::Invalid
                    && oversizedJson.errorCode
                        == QStringLiteral("artifact-set-json-size-invalid"),
                "oversized raw candidate JSON was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("published_at_ms"),
                     static_cast<double>(kNowMs + 5LL * 60 * 1000 + 1));
    ok = expect(key.sign(&candidate), "could not sign future candidate") && ok;
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-published-time-invalid"),
                       "future-dated candidate was accepted") && ok;

    candidate = candidateEnvelope();
    candidate.insert(QStringLiteral("published_at_ms"),
                     static_cast<double>(kNowMs + 5LL * 60 * 1000));
    ok = expect(key.sign(&candidate), "could not sign clock-boundary candidate") && ok;
    const UpdateArtifactSet::Decision clockBoundary =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), kNowMs, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(clockBoundary.state == UpdateArtifactSet::State::Compatible
                    && clockBoundary.candidateCompatible,
                "exact future clock-skew boundary was rejected") && ok;

    candidate = candidateEnvelope();
    ok = expect(key.sign(&candidate), "could not sign time-bound candidate") && ok;
    const UpdateArtifactSet::Decision invalidNow =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), key.publicKeyBase64(), 0, installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(invalidNow.state == UpdateArtifactSet::State::Invalid
                    && invalidNow.errorCode
                        == QStringLiteral("artifact-set-clock-invalid"),
                "invalid local time was accepted") && ok;

    const UpdateArtifactSet::Decision invalidKey =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), QByteArrayLiteral("AAAA"), kNowMs,
            installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(invalidKey.state == UpdateArtifactSet::State::Invalid
                    && invalidKey.errorCode
                        == QStringLiteral("artifact-set-public-key-invalid"),
                "invalid update public key was accepted") && ok;

    const UpdateArtifactSet::Decision oversizedKey =
        UpdateArtifactSet::verifyCandidate(
            encodedEnvelope(candidate), QByteArray(1024 * 1024, 'A'), kNowMs,
            installed,
            QStringLiteral("stable"), installed.releaseSequence);
    ok = expect(oversizedKey.state == UpdateArtifactSet::State::Invalid
                    && oversizedKey.errorCode
                        == QStringLiteral("artifact-set-public-key-invalid"),
                "oversized update public key was accepted") && ok;

    candidate.insert(QStringLiteral("signature"), QStringLiteral("not-base64"));
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-signature-encoding-invalid"),
                       "invalid outer signature encoding was accepted") && ok;

    candidate = candidateEnvelope();
    QJsonObject targetManifest = candidate.value(
        QStringLiteral("target_manifest")).toObject();
    targetManifest.insert(QStringLiteral("adapter"), component(
        QStringLiteral("other-adapter"),
        QStringLiteral("codex-cli 0.145.0")));
    candidate.insert(QStringLiteral("target_manifest"), targetManifest);
    ok = expectInvalid(candidate, key, installed,
                       QStringLiteral("artifact-set-component-invalid"),
                       "wrong target adapter identity was accepted") && ok;
    return ok;
}

bool installedAuthorityTests(const SigningKey &key)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "installed authority temporary directory is unavailable")) {
        return false;
    }
    const QString runtimePath = QDir(directory.path()).filePath(
        authorityRuntimeFileName());
    const QString adapterPath = QDir(directory.path()).filePath(
        authorityAdapterFileName());
    const QString applicationPath = QDir(directory.path()).filePath(
        QStringLiteral("AegisyClient.test-image"));
    const QByteArray runtimeBytes = QByteArrayLiteral("installed runtime bytes");
    const QByteArray adapterBytes = QByteArrayLiteral("installed adapter bytes");
    const QByteArray applicationBytes = QByteArrayLiteral(
        "installed application bytes");
    const QByteArray runtimeSha256 = writeBytes(runtimePath, runtimeBytes);
    const QByteArray adapterSha256 = writeBytes(adapterPath, adapterBytes);
    const QByteArray applicationSha256 = writeBytes(
        applicationPath, applicationBytes);
    if (!expect(runtimeSha256.size() == 64 && adapterSha256.size() == 64
                    && applicationSha256.size() == 64,
                "installed authority artifacts could not be written")) {
        return false;
    }

    const QJsonObject localManifest{
        {QStringLiteral("schema_version"),
         QStringLiteral("aegisy-artifact-manifest/0.1")},
        {QStringLiteral("runtime"), QJsonObject{
             {QStringLiteral("id"), QStringLiteral("aegisy-agentd")},
             {QStringLiteral("version"), QStringLiteral("0.1.0")},
             {QStringLiteral("path"), authorityRuntimeFileName()},
             {QStringLiteral("sha256"), QString::fromLatin1(runtimeSha256)},
         }},
        {QStringLiteral("adapter"), QJsonObject{
             {QStringLiteral("id"), QStringLiteral("codex-app-server")},
             {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
             {QStringLiteral("path"), authorityAdapterFileName()},
             {QStringLiteral("sha256"), QString::fromLatin1(adapterSha256)},
         }},
    };
    const QByteArray manifestBytes = QJsonDocument(localManifest).toJson(
        QJsonDocument::Compact);
    const QString manifestPath = QDir(directory.path()).filePath(
        QStringLiteral("aegisy-agentd.manifest.json"));
    const QByteArray manifestSha256 = writeBytes(manifestPath, manifestBytes);
    if (!expect(manifestSha256.size() == 64,
                "installed authority manifest could not be written")) {
        return false;
    }

    QJsonObject receipt = signedSetEnvelope(
        41, QStringLiteral("2.5.2"), QString::fromLatin1(manifestSha256),
        QStringLiteral("0.1.0"), QStringLiteral("codex-cli 0.144.5"),
        40, QStringLiteral("2.5.1"), hashString('0'),
        QStringLiteral("0.0.9"), QStringLiteral("codex-cli 0.143.0"));
    if (!expect(key.sign(&receipt),
                "installed artifact-set receipt could not be signed")) {
        return false;
    }
    const QByteArray receiptBytes = encodedEnvelope(receipt);
    const QString receiptPath = QDir(directory.path()).filePath(
        QStringLiteral("aegisy-update-artifact-set.json"));
    if (!expect(writeBytes(receiptPath, receiptBytes).size() == 64,
                "installed artifact-set receipt could not be written")) {
        return false;
    }

    const auto loadAuthority = [&]() {
        return UpdateArtifactSet::Testing::verifyInstalledAuthorityAtRoot(
            directory.path(), key.publicKeyBase64(), kNowMs,
            QStringLiteral("2.5.2"), QStringLiteral("stable"),
            authorityPlatform(), authorityArchitecture());
    };
    UpdateArtifactSet::InstalledAuthorityResult authorityResult = loadAuthority();
    bool ok = expect(authorityResult.ok && authorityResult.errorCode.isEmpty()
                         && authorityResult.authority.isValid()
                         && authorityResult.authority.authorityIdentity().startsWith(
                             QStringLiteral(
                                 "installed-artifact-set-authority:sha256:")),
                     "valid installed artifact authority was rejected");

    const UpdateArtifactSet::InstalledAuthorityResult productionFactoryFromTest =
        UpdateArtifactSet::verifyCurrentInstallationAuthority(
            key.publicKeyBase64(), kNowMs, QStringLiteral("stable"));
    ok = expect(!productionFactoryFromTest.ok
                    && productionFactoryFromTest.errorCode
                        == QStringLiteral(
                            "installed-authority-application-invalid"),
                "production authority factory accepted a non-Aegisy test executable")
        && ok;

    QJsonObject candidate = signedSetEnvelope(
        42, QStringLiteral("2.6.0"), hashString('2'),
        QStringLiteral("0.2.0"), QStringLiteral("codex-cli 0.145.0"),
        41, QStringLiteral("2.5.2"), QString::fromLatin1(manifestSha256),
        QStringLiteral("0.1.0"), QStringLiteral("codex-cli 0.144.5"));
    ok = expect(key.sign(&candidate),
                "authority-bound candidate could not be signed") && ok;
    const QByteArray candidateBytes = encodedEnvelope(candidate);
    const UpdateArtifactSet::Decision compatible =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(compatible.state == UpdateArtifactSet::State::Compatible
                    && compatible.candidateCompatible
                    && !compatible.downloadAuthorized
                    && !compatible.installAuthorized
                    && compatible.installedAuthorityIdentity
                        == authorityResult.authority.authorityIdentity()
                    && compatible.compatibilityEvaluationIdentity.startsWith(
                        QStringLiteral("update-artifact-set-evaluation:sha256:")),
                "candidate was not bound to the verified installed authority") && ok;
    ok = productionInstallationLayoutTest(
             key, receiptBytes, manifestBytes, runtimeBytes, adapterBytes,
             candidateBytes)
        && ok;

    writeBytes(applicationPath, QByteArrayLiteral("replaced application bytes"));
    const UpdateArtifactSet::Decision replacedApplicationAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(replacedApplicationAuthority.state
                        == UpdateArtifactSet::State::Invalid
                    && replacedApplicationAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid"),
                "cached authority survived application image replacement") && ok;
    writeBytes(applicationPath, applicationBytes);

    const QString hardLinkedApplicationPath = QDir(directory.path()).filePath(
        QStringLiteral("hard-linked-application-image"));
    ok = expect(createHardLink(applicationPath, hardLinkedApplicationPath),
                "application image hard link could not be created") && ok;
    const UpdateArtifactSet::InstalledAuthorityResult hardLinkedApplication =
        loadAuthority();
    ok = expect(!hardLinkedApplication.ok
                    && hardLinkedApplication.errorCode
                        == QStringLiteral("installed-authority-layout-invalid"),
                "multiply linked application image was accepted") && ok;
    ok = expect(QFile::remove(hardLinkedApplicationPath),
                "application image hard link could not be removed") && ok;

    UpdateArtifactSet::InstalledAuthorityResult rejected =
        UpdateArtifactSet::Testing::verifyInstalledAuthorityAtRoot(
            directory.path(), key.publicKeyBase64(), kNowMs,
            QStringLiteral("2.5.2"), QStringLiteral("beta"),
            authorityPlatform(), authorityArchitecture());
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-receipt-target-mismatch"),
                "wrong installed channel expectation was accepted") && ok;

#ifdef Q_OS_WIN
    const QString otherPlatform = QStringLiteral("macos");
    const QString otherArchitecture = QStringLiteral("arm64");
#else
    const QString otherPlatform = QStringLiteral("windows");
    const QString otherArchitecture = QStringLiteral("x86_64");
#endif
    rejected = UpdateArtifactSet::Testing::verifyInstalledAuthorityAtRoot(
        directory.path(), key.publicKeyBase64(), kNowMs,
        QStringLiteral("2.5.2"), QStringLiteral("stable"),
        otherPlatform, otherArchitecture);
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-receipt-target-mismatch"),
                "wrong installed platform expectation was accepted") && ok;

    QJsonObject replacedReceipt = signedSetEnvelope(
        40, QStringLiteral("2.5.2"), QString::fromLatin1(manifestSha256),
        QStringLiteral("0.1.0"), QStringLiteral("codex-cli 0.144.5"),
        39, QStringLiteral("2.5.1"), hashString('0'),
        QStringLiteral("0.0.9"), QStringLiteral("codex-cli 0.143.0"));
    ok = expect(key.sign(&replacedReceipt),
                "replacement installed receipt could not be signed") && ok;
    writeBytes(receiptPath, encodedEnvelope(replacedReceipt));
    const UpdateArtifactSet::Decision replacedReceiptAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(replacedReceiptAuthority.state
                        == UpdateArtifactSet::State::Invalid
                    && replacedReceiptAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid"),
                "cached authority survived a valid receipt sequence replacement")
        && ok;
    writeBytes(receiptPath, receiptBytes);

    const UpdateArtifactSet::Decision missingAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            UpdateArtifactSet::InstalledArtifactSetAuthority{},
            QStringLiteral("stable"), 41);
    ok = expect(missingAuthority.state == UpdateArtifactSet::State::Invalid
                    && missingAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid")
                    && missingAuthority.artifactSetIdentity.isEmpty()
                    && !missingAuthority.candidateCompatible
                    && !missingAuthority.downloadAuthorized
                    && !missingAuthority.installAuthorized,
                "missing installed authority did not fail closed") && ok;

    QJsonObject tamperedReceipt = receipt;
    tamperedReceipt.insert(QStringLiteral("channel"), QStringLiteral("beta"));
    writeBytes(receiptPath, encodedEnvelope(tamperedReceipt));
    rejected = loadAuthority();
    ok = expect(!rejected.ok && !rejected.authority.isValid()
                    && rejected.errorCode
                        == QStringLiteral(
                            "installed-receipt-artifact-set-signature-invalid"),
                "tampered installed receipt was accepted") && ok;
    const UpdateArtifactSet::Decision staleReceiptAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(staleReceiptAuthority.state == UpdateArtifactSet::State::Invalid
                    && staleReceiptAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid"),
                "cached authority survived installed receipt drift") && ok;
    writeBytes(receiptPath, receiptBytes);

    writeBytes(manifestPath, manifestBytes + QByteArrayLiteral("\n"));
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-manifest-mismatch"),
                "installed manifest byte drift was accepted") && ok;
    const UpdateArtifactSet::Decision staleManifestBytesAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(staleManifestBytesAuthority.state
                        == UpdateArtifactSet::State::Invalid
                    && staleManifestBytesAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid"),
                "cached authority survived installed manifest replacement") && ok;
    writeBytes(manifestPath, manifestBytes);

    QJsonObject mismatchedReceipt = receipt;
    QJsonObject mismatchedTarget = mismatchedReceipt.value(
        QStringLiteral("target_manifest")).toObject();
    mismatchedTarget.insert(QStringLiteral("runtime"), component(
        QStringLiteral("aegisy-agentd"), QStringLiteral("0.1.1")));
    mismatchedReceipt.insert(QStringLiteral("target_manifest"), mismatchedTarget);
    ok = expect(key.sign(&mismatchedReceipt),
                "runtime-version mismatch receipt could not be signed") && ok;
    writeBytes(receiptPath, encodedEnvelope(mismatchedReceipt));
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-manifest-mismatch"),
                "receipt runtime-version mismatch was accepted") && ok;

    mismatchedReceipt = receipt;
    mismatchedTarget = mismatchedReceipt.value(
        QStringLiteral("target_manifest")).toObject();
    mismatchedTarget.insert(QStringLiteral("adapter"), component(
        QStringLiteral("codex-app-server"),
        QStringLiteral("codex-cli 0.144.4")));
    mismatchedReceipt.insert(QStringLiteral("target_manifest"), mismatchedTarget);
    ok = expect(key.sign(&mismatchedReceipt),
                "adapter-version mismatch receipt could not be signed") && ok;
    writeBytes(receiptPath, encodedEnvelope(mismatchedReceipt));
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-manifest-mismatch"),
                "receipt adapter-version mismatch was accepted") && ok;
    writeBytes(receiptPath, receiptBytes);

    writeBytes(adapterPath, adapterBytes + QByteArrayLiteral("tamper"));
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-manifest-invalid"),
                "installed adapter byte drift was accepted") && ok;
    const UpdateArtifactSet::Decision staleManifestAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(staleManifestAuthority.state == UpdateArtifactSet::State::Invalid
                    && staleManifestAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid"),
                "cached authority survived installed adapter drift") && ok;
    writeBytes(adapterPath, adapterBytes);

    writeBytes(runtimePath, runtimeBytes + QByteArrayLiteral("tamper"));
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-manifest-invalid"),
                "installed runtime byte drift was accepted") && ok;
    const UpdateArtifactSet::Decision staleRuntimeAuthority =
        UpdateArtifactSet::verifyCandidate(
            candidateBytes, key.publicKeyBase64(), kNowMs,
            authorityResult.authority, QStringLiteral("stable"), 41);
    ok = expect(staleRuntimeAuthority.state == UpdateArtifactSet::State::Invalid
                    && staleRuntimeAuthority.errorCode
                        == QStringLiteral("installed-artifact-authority-invalid"),
                "cached authority survived installed runtime drift") && ok;
    writeBytes(runtimePath, runtimeBytes);

    rejected = UpdateArtifactSet::Testing::verifyInstalledAuthorityAtRoot(
        directory.path(), key.publicKeyBase64(), kNowMs,
        QStringLiteral("2.5.3"), QStringLiteral("stable"),
        authorityPlatform(), authorityArchitecture());
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-receipt-target-mismatch"),
                "wrong running application version was accepted") && ok;

    const QString wrongReceiptPath = QDir(directory.path()).filePath(
        QStringLiteral("receipt.json"));
    ok = expect(QFile::rename(receiptPath, wrongReceiptPath),
                "installed receipt could not be moved to a noncanonical name") && ok;
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-authority-path-invalid"),
                "noncanonical installed receipt path was accepted") && ok;
    ok = expect(QFile::rename(wrongReceiptPath, receiptPath),
                "installed receipt could not be restored") && ok;

    const QString hardLinkedReceiptPath = QDir(directory.path()).filePath(
        QStringLiteral("hard-linked-installed-receipt.json"));
    ok = expect(createHardLink(receiptPath, hardLinkedReceiptPath),
                "installed receipt hard link could not be created") && ok;
    rejected = loadAuthority();
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-authority-path-invalid"),
                "multiply linked installed receipt was accepted") && ok;
    ok = expect(QFile::remove(hardLinkedReceiptPath),
                "installed receipt hard link could not be removed") && ok;

    QTemporaryDir otherDirectory;
    ok = expect(otherDirectory.isValid(),
                "second installed authority directory is unavailable") && ok;
    const QString separatedReceiptPath = QDir(otherDirectory.path()).filePath(
        QStringLiteral("aegisy-update-artifact-set.json"));
    writeBytes(separatedReceiptPath, receiptBytes);
    writeBytes(QDir(otherDirectory.path()).filePath(
                   QStringLiteral("AegisyClient.test-image")),
               applicationBytes);
    rejected = UpdateArtifactSet::Testing::verifyInstalledAuthorityAtRoot(
        otherDirectory.path(), key.publicKeyBase64(), kNowMs,
        QStringLiteral("2.5.2"), QStringLiteral("stable"),
        authorityPlatform(), authorityArchitecture());
    ok = expect(!rejected.ok
                    && rejected.errorCode
                        == QStringLiteral("installed-authority-path-invalid"),
                "incomplete alternate installation root was accepted") && ok;

    SigningKey otherKey;
    ok = expect(otherKey.initialize(), "second Ed25519 test key unavailable") && ok;
    rejected = UpdateArtifactSet::Testing::verifyInstalledAuthorityAtRoot(
        directory.path(), otherKey.publicKeyBase64(), kNowMs,
        QStringLiteral("2.5.2"), QStringLiteral("stable"),
        authorityPlatform(), authorityArchitecture());
    ok = expect(!rejected.ok && !rejected.authority.isValid()
                    && rejected.errorCode
                        == QStringLiteral(
                            "installed-receipt-artifact-set-signature-invalid"),
                "installed receipt verified under the wrong release key") && ok;

    const QFileInfo rootInfo(directory.path());
    QDir parentDirectory(rootInfo.absolutePath());
    const QString preservedRootName = rootInfo.fileName()
        + QStringLiteral("-preserved");
    const QString preservedRootPath = parentDirectory.filePath(preservedRootName);
    ok = expect(parentDirectory.rename(rootInfo.fileName(), preservedRootName),
                "installed root could not be preserved for replacement test") && ok;
    bool replacementCreated = false;
    if (QFileInfo::exists(preservedRootPath)) {
        replacementCreated = QDir().mkpath(directory.path())
            && !writeBytes(applicationPath, applicationBytes).isEmpty()
            && !writeBytes(runtimePath, runtimeBytes).isEmpty()
            && !writeBytes(adapterPath, adapterBytes).isEmpty()
            && !writeBytes(manifestPath, manifestBytes).isEmpty()
            && !writeBytes(receiptPath, receiptBytes).isEmpty();
        ok = expect(replacementCreated,
                    "exact-byte replacement installation root could not be built")
            && ok;
        if (replacementCreated) {
            const UpdateArtifactSet::Decision replacedRootAuthority =
                UpdateArtifactSet::verifyCandidate(
                    candidateBytes, key.publicKeyBase64(), kNowMs,
                    authorityResult.authority, QStringLiteral("stable"), 41);
            ok = expect(replacedRootAuthority.state
                            == UpdateArtifactSet::State::Invalid
                        && replacedRootAuthority.errorCode
                            == QStringLiteral(
                                "installed-artifact-authority-invalid"),
                        "cached authority survived installation-root replacement")
                && ok;
        }
        ok = expect(QDir(directory.path()).removeRecursively(),
                    "replacement installation root could not be removed") && ok;
        ok = expect(parentDirectory.rename(preservedRootName,
                                           rootInfo.fileName()),
                    "preserved installation root could not be restored") && ok;
    }
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() > 1
        && application.arguments().at(1)
            == QStringLiteral("--verify-current-installation")) {
        return currentInstallationChildMain(application.arguments());
    }
    SigningKey key;
    if (!expect(key.initialize(), "could not create Ed25519 test key")) return 1;

    bool ok = true;
    ok = validCandidateTests(key) && ok;
    ok = canonicalPayloadTests(key) && ok;
    ok = signatureAndFieldTamperTests(key) && ok;
    ok = compatibilityTests(key) && ok;
    ok = sourceStructureTests(key) && ok;
    ok = rawJsonBoundaryTests(key) && ok;
    ok = installerAndValueTests(key) && ok;
    ok = installedAuthorityTests(key) && ok;
    return ok ? 0 : 1;
}
