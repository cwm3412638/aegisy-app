#include "aap_core_types_generated.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <iostream>

using namespace aegisy::aap::generated;

namespace {

constexpr auto kCorpusSchema = "aap-core-generated-corpus/0.1";

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    QSet<QString> actual;
    const QStringList objectKeys = object.keys();
    for (const QString &key : objectKeys) {
        actual.insert(key);
    }
    return actual == keys;
}

int emitCorpusIdentity(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "corpus is unreadable\n";
        return 6;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        std::cerr << "corpus is not a JSON object\n";
        return 7;
    }
    const QJsonObject corpus = document.object();
    if (!hasExactKeys(corpus, {QStringLiteral("schema_version"),
                               QStringLiteral("expected_decisions_sha256"),
                               QStringLiteral("cases")})
        || corpus.value(QStringLiteral("schema_version")).toString() != QString::fromLatin1(kCorpusSchema)) {
        std::cerr << "corpus contract is invalid\n";
        return 8;
    }
    static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression caseNamePattern(
        QStringLiteral("^[a-z0-9]+(?:-[a-z0-9]+)*$"));
    static const QRegularExpression definitionPattern(QStringLiteral("^[A-Za-z][A-Za-z0-9]*$"));
    const QString expectedDigest = corpus.value(QStringLiteral("expected_decisions_sha256")).toString();
    const QJsonValue casesValue = corpus.value(QStringLiteral("cases"));
    if (!shaPattern.match(expectedDigest).hasMatch() || !casesValue.isArray()
        || casesValue.toArray().isEmpty() || casesValue.toArray().size() > 128) {
        std::cerr << "corpus metadata is invalid\n";
        return 9;
    }

    const QJsonArray cases = casesValue.toArray();
    QSet<QString> names;
    QByteArray decisions = QByteArray(kCorpusSchema) + '\n';
    for (qsizetype index = 0; index < cases.size(); ++index) {
        if (!cases.at(index).isObject()) {
            std::cerr << "corpus case is not an object\n";
            return 10;
        }
        const QJsonObject entry = cases.at(index).toObject();
        if (!hasExactKeys(entry, {QStringLiteral("name"), QStringLiteral("definition"),
                                  QStringLiteral("valid"), QStringLiteral("value_json")})) {
            std::cerr << "corpus case fields are invalid\n";
            return 11;
        }
        const QString name = entry.value(QStringLiteral("name")).toString();
        const QString definition = entry.value(QStringLiteral("definition")).toString();
        if (name.size() > 96 || !caseNamePattern.match(name).hasMatch() || names.contains(name)
            || !definitionPattern.match(definition).hasMatch()
            || !entry.value(QStringLiteral("valid")).isBool()) {
            std::cerr << "corpus case metadata is invalid\n";
            return 12;
        }
        names.insert(name);

        const QJsonValue rawJsonValue = entry.value(QStringLiteral("value_json"));
        if (!rawJsonValue.isString() || rawJsonValue.toString().toUtf8().size() > 4 * 1024 * 1024) {
            std::cerr << "corpus case raw JSON is invalid\n";
            return 15;
        }
        const QByteArray wrapped = QByteArrayLiteral("[") + rawJsonValue.toString().toUtf8()
            + QByteArrayLiteral("]");
        QJsonParseError valueParseError;
        const QJsonDocument valueDocument = QJsonDocument::fromJson(wrapped, &valueParseError);
        bool accepted = false;
        if (valueParseError.error == QJsonParseError::NoError && valueDocument.isArray()
            && valueDocument.array().size() == 1) {
            QString validationError;
            accepted = validateCoreDefinition(definition, valueDocument.array().at(0), &validationError);
        }
        const bool expected = entry.value(QStringLiteral("valid")).toBool();
        if (accepted != expected) {
            std::cerr << "corpus case decision differs from expectation: "
                      << name.toLatin1().constData() << '\n';
            return 13;
        }
        decisions += name.toLatin1();
        decisions += '\t';
        decisions += definition.toLatin1();
        decisions += '\t';
        decisions += accepted ? "accept\n" : "reject\n";
    }

    const QByteArray digest = QCryptographicHash::hash(decisions, QCryptographicHash::Sha256).toHex();
    if (QString::fromLatin1(digest) != expectedDigest) {
        std::cerr << "corpus decision identity differs from golden: computed "
                  << digest.constData() << '\n';
        return 14;
    }
    std::cout << cases.size() << ' ' << digest.constData() << '\n';
    return 0;
}

int emitFixtureIdentity(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "fixture is unreadable\n";
        return 3;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        std::cerr << "fixture is not a JSON object\n";
        return 4;
    }

    CoreFixtureCatalog fixture;
    QString decodeError;
    if (!decodeCoreFixtureCatalog(document.object(), &fixture, &decodeError)) {
        std::cerr << "fixture does not match generated C++ types\n";
        return 5;
    }
    const QByteArray canonical = canonicalCoreFixtureCatalog(fixture);
    const QByteArray digest = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
    std::cout << canonical.size() << ' ' << digest.constData() << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--corpus")) {
        return emitCorpusIdentity(arguments.at(2));
    }
    if (arguments.size() == 2) {
        return emitFixtureIdentity(arguments.at(1));
    }
    std::cerr << "usage: AegisyAapGeneratedTypesTest [--corpus] <path>\n";
    return 2;
}
