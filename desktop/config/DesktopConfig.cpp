#include "DesktopConfig.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <optional>
#include <stdexcept>

namespace chipcae::desktop {
namespace {
std::optional<DesktopConfig> processConfig;
constexpr auto schemaMarker = "case-1.0.schema.json";
[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(("Desktop config: " + message).toStdString());
}
QString checkedSchemaDir(const QString& candidate) {
    if (candidate.isEmpty())
        return {};
    const auto path = QDir::current().filePath(candidate);
    const QFileInfo directory(path);
    if (!directory.isDir() || !QFileInfo(QDir(path).filePath(schemaMarker)).isFile())
        return {};
    const auto canonical = directory.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(directory.absoluteFilePath()) : canonical;
}
QString string(const QJsonObject& obj, const QString& key) {
    const auto value = obj.value(key);
    if (!value.isString() || value.toString().trimmed().isEmpty())
        fail(key + " must be a non-empty string");
    return value.toString();
}
QJsonObject object(const QJsonObject& obj, const QString& key) {
    if (!obj.value(key).isObject())
        fail(key + " must be an object");
    return obj.value(key).toObject();
}
DesktopEntrypoint entry(const QJsonObject& entries, const QString& name, bool prefix) {
    const auto obj = object(entries, name);
    DesktopEntrypoint result;
    result.source = string(obj, "source");
    result.program = string(obj, "program");
    if (prefix) {
        if (!obj.value("prefix").isArray())
            fail(name + ".prefix must be a string array");
        for (const auto& arg : obj.value("prefix").toArray()) {
            if (!arg.isString())
                fail(name + ".prefix must contain strings");
            result.prefix.append(arg.toString());
        }
    }
    if (name == "import" && obj.contains("material_map"))
        result.materialMap = string(obj, "material_map");
    return result;
}
bool mockTool(const QString& path) {
    const auto name = QFileInfo(path).fileName();
    return (name.startsWith("mock_") && name.endsWith(".py")) || name == "mock-solve";
}
void validate(const DesktopEntrypoint& e, const QString& name) {
    if (e.source != "real" && e.source != "mock")
        fail(name + ".source must be real or mock");
    bool mock = mockTool(e.program);
    for (const auto& arg : e.prefix)
        mock = mock || mockTool(arg);
    const auto binary = QFileInfo(e.program).fileName();
    const bool knownReal =
        binary == "chipcae-import" || binary == "chipcae-preview" || binary == "chipcae-solve";
    if ((e.source == "real" && mock) || (e.source == "mock" && knownReal))
        fail(name + ": source/program mismatch");
}
void overrideEntry(DesktopEntrypoint& e,
                   const QProcessEnvironment& env,
                   const QString& key,
                   bool extraProgram = false) {
    const auto sourceKey = key + "_SOURCE";
    if ((env.contains(key) || extraProgram) && !env.contains(sourceKey))
        fail(sourceKey + " is required for a program override");
    if (env.contains(key)) {
        e.program = env.value(key);
        if (e.program.trimmed().isEmpty())
            fail(key + " must not be empty");
    }
    if (env.contains(sourceKey))
        e.source = env.value(sourceKey);
}
} // namespace
DesktopConfig::DesktopConfig() : runRoot_(QDir::current().absoluteFilePath("runs")) {
    import_.program = "chipcae-import";
    preview_.program = "chipcae-preview";
    solve_.program = "chipcae-solve";
}
QString DesktopConfig::locateSchemaDir(const QStringList& args,
                                       const QProcessEnvironment& env,
                                       const QString& executableDir,
                                       const QString& compileTimeFallback) {
    QString path;
    for (qsizetype i = 1; i < args.size(); ++i) {
        if (args[i] != "--schema-dir")
            continue;
        if (!path.isEmpty())
            fail("--schema-dir may only be specified once");
        if (++i >= args.size() || args[i].startsWith("--") || args[i].trimmed().isEmpty())
            fail("--schema-dir requires a directory path");
        path = args[i];
    }

    QString source;
    if (!path.isEmpty()) {
        source = "--schema-dir";
    } else if (env.contains("CHIPCAE_SCHEMA_DIR")) {
        source = "CHIPCAE_SCHEMA_DIR";
        path = env.value(source);
    }
    if (!source.isEmpty()) {
        const auto dir = checkedSchemaDir(path);
        if (dir.isEmpty())
            fail(source + " must point to a directory containing " + schemaMarker);
        return dir;
    }

    if (!executableDir.isEmpty()) {
        const QDir executable(executableDir);
        for (const auto* relative : {"../Resources/schema", "../share/chipcae/schema", "schema"}) {
            const auto dir = checkedSchemaDir(executable.filePath(relative));
            if (!dir.isEmpty())
                return dir;
        }
    }
    return checkedSchemaDir(compileTimeFallback);
}
QString DesktopConfig::locateSchemaFile(const QString& fileName,
                                        const QStringList& args,
                                        const QProcessEnvironment& env,
                                        const QString& executableDir,
                                        const QString& compileTimeFallback) {
    if (fileName.isEmpty() || fileName == "." || fileName.contains('/') ||
        fileName.contains('\\') || fileName.contains("..") || QDir::isAbsolutePath(fileName))
        fail("fileName must be a single schema file name");

    const auto dir = locateSchemaDir(args, env, executableDir, compileTimeFallback);
    return dir.isEmpty() ? QString() : QDir::cleanPath(QDir(dir).filePath(fileName));
}
DesktopConfig DesktopConfig::load(const QStringList& args, const QProcessEnvironment& env) {
    DesktopConfig result;
    QString path;
    bool explicitPath = false;
    for (qsizetype i = 1; i < args.size(); ++i) {
        if (args[i] != "--config")
            continue;
        if (explicitPath)
            fail("--config may only be specified once");
        if (++i >= args.size() || args[i].startsWith("--") || args[i].trimmed().isEmpty())
            fail("--config requires a file path");
        path = args[i];
        explicitPath = true;
    }
    if (!explicitPath && env.contains("CHIPCAE_DESKTOP_CONFIG")) {
        path = env.value("CHIPCAE_DESKTOP_CONFIG");
        explicitPath = true;
    }
    if (explicitPath) {
        if (path.trimmed().isEmpty())
            fail("configuration path must not be empty");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            fail(path + ": " + file.errorString());
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError)
            fail(path + ": " + error.errorString());
        if (!doc.isObject())
            fail(path + ": root must be an object");
        const auto root = doc.object();
        if (string(root, "schema") != "chipcae.desktop.config/0.1")
            fail("unsupported schema");
        result.runRoot_ = string(root, "run_root");
        const auto entries = object(root, "entrypoints");
        result.import_ = entry(entries, "import", true);
        result.preview_ = entry(entries, "preview", true);
        result.solve_ = entry(entries, "solve", false);
        const auto solve = object(entries, "solve");
        result.mpi_ = string(solve, "mpi");
        result.eventsFile_ = string(solve, "events_file");
        if (root.contains("precomputed")) {
            const auto precomputed = object(root, "precomputed");
            result.precomputed_.runDir = string(precomputed, "run_dir");
            result.precomputed_.manifestSha256 = string(precomputed, "manifest_sha256");
            if (!QRegularExpression("^[0-9a-fA-F]{64}$")
                     .match(result.precomputed_.manifestSha256)
                     .hasMatch())
                fail("precomputed.manifest_sha256 must contain 64 hex digits");
        }
        // Validate the file before applying overrides; an invalid file never falls back.
        validate(result.import_, "import");
        validate(result.preview_, "preview");
        validate(result.solve_, "solve");
    }
    overrideEntry(result.import_, env, "CHIPCAE_IMPORT");
    overrideEntry(result.preview_, env, "CHIPCAE_PREVIEW");
    overrideEntry(result.solve_, env, "CHIPCAE_SOLVE", env.contains("CHIPCAE_MPIRUN"));
    if (env.contains("CHIPCAE_MPIRUN")) {
        result.mpi_ = env.value("CHIPCAE_MPIRUN");
        if (result.mpi_.trimmed().isEmpty())
            fail("CHIPCAE_MPIRUN must not be empty");
    }
    validate(result.import_, "import");
    validate(result.preview_, "preview");
    validate(result.solve_, "solve");
    return result;
}
LauncherConfig DesktopConfig::launcherConfig() const {
    LauncherConfig result;
    result.solver = solve_.program;
    result.mpi = mpi_;
    result.mock = solve_.source == "mock";
    result.runRoot = runRoot_;
    return result;
}
void DesktopConfig::setCurrent(const DesktopConfig& config) {
    processConfig = config;
}
const DesktopConfig& DesktopConfig::current() {
    if (!processConfig)
        processConfig = DesktopConfig();
    return *processConfig;
}
} // namespace chipcae::desktop
