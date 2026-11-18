#include "DesktopConfig.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
QString writeFixture(const QString& path, const QByteArray& data) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly), "open fixture for writing");
    check(f.write(data) == data.size(), "write fixture");
    return path;
}
template <typename Error = std::runtime_error, typename Operation>
void rejected(Operation operation, const QString& expectedSubstring = {}) {
    try {
        operation();
    } catch (const Error& ex) {
        const auto message = QString::fromUtf8(ex.what());
        if (expectedSubstring.isEmpty())
            check(message.size() > 8, "readable error");
        else
            check(message.contains(expectedSubstring), "error did not contain the expected text");
        return;
    }
    throw std::runtime_error(expectedSubstring.isEmpty() ? "invalid config accepted"
                                                         : "expected std::runtime_error");
}
void checkRealConfig(const DesktopConfig& config) {
    check(config.runRoot() == "/tmp/real-runs" &&
              config.importEntry().materialMap == "material-map.json" &&
              config.precomputed().runDir == "/tmp/precomputed" &&
              config.launcherConfig().mpi == "custom-mpi",
          "CLI precedence and metadata");
}
void schemaPathTests(const QTemporaryDir& dir, const QString& configPath) {
    const QString marker = "case-1.0.schema.json";
    const QString schemaOption = "--schema-dir";
    const QString schemaVariable = "CHIPCAE_SCHEMA_DIR";
    const QStringList args{"desktop"};
    const QProcessEnvironment emptyEnv;
    const auto makeDir = [&](const QString& relativePath) {
        const auto path = dir.filePath(relativePath);
        check(QDir().mkpath(path), "create schema fixture directory");
        return path;
    };
    const auto makeSchema = [&](const QString& relativePath) {
        const auto path = makeDir(relativePath);
        writeFixture(QDir(path).filePath(marker), "{}");
        return path;
    };
    const auto canonicalDir = [](const QString& path) {
        const QFileInfo info(path);
        const auto canonical = info.canonicalFilePath();
        return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
    };

    const auto cli = makeSchema("cli/schema");
    const auto envSchema = makeSchema("env/schema");
    const auto macOS = makeDir("app/MyApp.app/Contents/MacOS");
    const auto bundle = makeSchema("app/MyApp.app/Contents/Resources/schema");
    makeSchema("app/MyApp.app/Contents/share/chipcae/schema");
    makeSchema("app/MyApp.app/Contents/MacOS/schema");
    const auto bin = makeDir("prefix/bin");
    const auto share = makeSchema("prefix/share/chipcae/schema");
    makeSchema("prefix/bin/schema");
    const auto cwdrel = makeDir("cwdrel");
    const auto adjacent = makeSchema("cwdrel/schema");
    const auto fallback = makeSchema("fallback/schema");

    const auto missing = dir.filePath("missing-schema");
    const auto unmarked = makeDir("unmarked/schema");
    const auto missBin = makeDir("miss/bin");
    const auto markerDirectory = makeDir("miss/share/chipcae/schema");
    makeDir("miss/share/chipcae/schema/" + marker);
#ifdef Q_OS_LINUX
    const auto wrongCase = makeDir("miss/Resources/schema");
    writeFixture(QDir(wrongCase).filePath("Case-1.0.schema.json"), "{}");
#endif
    const auto alias = dir.filePath("schema-alias");
    check(QFile::link(cli, alias), "create schema directory symlink");

    const auto originalCwd = QDir::currentPath();
    const auto restoreCwd = qScopeGuard([&] { QDir::setCurrent(originalCwd); });
    check(QDir::setCurrent(cwdrel), "set schema fixture working directory");

    // Run each check so the TDD stubs expose every failing scenario in CTest.
    int failures = 0;
    const auto run = [&](const char* scenario, auto operation) {
        try {
            operation();
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << scenario << ": " << ex.what() << '\n';
        }
    };
    const auto expectDir = [&](const char* scenario,
                               const QString& expected,
                               const QStringList& argv,
                               const QProcessEnvironment& env,
                               const QString& executableDir,
                               const QString& fallbackDir) {
        run(scenario, [&] {
            check(DesktopConfig::locateSchemaDir(argv, env, executableDir, fallbackDir) == expected,
                  "schema directory differs from expected path");
        });
    };
    const auto expectFile = [&](const char* scenario,
                                const QString& expected,
                                const QString& fileName,
                                const QStringList& argv,
                                const QProcessEnvironment& env,
                                const QString& executableDir,
                                const QString& fallbackDir) {
        run(scenario, [&] {
            check(DesktopConfig::locateSchemaFile(
                      fileName, argv, env, executableDir, fallbackDir) == expected,
                  "schema file differs from expected path");
        });
    };

    QProcessEnvironment schemaEnv;
    schemaEnv.insert(schemaVariable, envSchema);
    const auto canonicalCli = canonicalDir(cli);
    const auto canonicalMacOS = canonicalDir(macOS);
    const QStringList cliArgs{"desktop", schemaOption, cli};
    expectDir("CLI takes precedence", canonicalCli, cliArgs, schemaEnv, canonicalMacOS, fallback);
    expectFile("CLI marker lookup",
               canonicalCli + "/" + marker,
               marker,
               cliArgs,
               schemaEnv,
               canonicalMacOS,
               fallback);

    const auto rejectCli = [&](const char* scenario, const QStringList& argv) {
        run(scenario, [&] {
            rejected(
                [&] { DesktopConfig::locateSchemaDir(argv, schemaEnv, canonicalMacOS, fallback); },
                schemaOption);
        });
    };
    rejectCli("duplicate CLI option", cliArgs + QStringList{schemaOption, cli});
    rejectCli("CLI option without value", {"desktop", schemaOption});
    rejectCli("CLI option before another option", {"desktop", schemaOption, "--platform"});
    rejectCli("empty CLI value", {"desktop", schemaOption, ""});
    rejectCli("whitespace-only CLI value", {"desktop", schemaOption, " \t "});
    rejectCli("missing CLI directory", {"desktop", schemaOption, missing});
    rejectCli("unmarked CLI directory", {"desktop", schemaOption, unmarked});
    rejectCli("CLI marker is a directory", {"desktop", schemaOption, markerDirectory});

    expectDir("environment takes precedence",
              canonicalDir(envSchema),
              args,
              schemaEnv,
              canonicalMacOS,
              fallback);
    const auto rejectEnv = [&](const char* scenario, const QString& value) {
        QProcessEnvironment invalidEnv;
        invalidEnv.insert(schemaVariable, value);
        run(scenario, [&] {
            rejected(
                [&] { DesktopConfig::locateSchemaDir(args, invalidEnv, canonicalMacOS, fallback); },
                schemaVariable);
        });
    };
    rejectEnv("empty environment override", "");
    rejectEnv("missing environment directory", missing);
    rejectEnv("unmarked environment directory", unmarked);

    expectDir(
        "bundle takes precedence", canonicalDir(bundle), args, emptyEnv, canonicalMacOS, fallback);
    expectDir(
        "install prefix takes precedence", canonicalDir(share), args, emptyEnv, bin, fallback);
    expectDir(
        "adjacent schema directory", canonicalDir(adjacent), args, emptyEnv, cwdrel, fallback);
    expectDir("fallback after invalid implicit candidates",
              canonicalDir(fallback),
              args,
              emptyEnv,
              missBin,
              fallback);
#ifdef Q_OS_LINUX
    QProcessEnvironment lowercaseEnv;
    lowercaseEnv.insert("chipcae_schema_dir", envSchema);
    expectDir("lowercase environment variable is ignored",
              canonicalDir(fallback),
              args,
              lowercaseEnv,
              missBin,
              fallback);
#endif
    expectDir("invalid fallback", {}, args, emptyEnv, missBin, unmarked);
    expectDir("all inputs empty", {}, {}, emptyEnv, {}, {});
    expectDir("cwd-relative CLI directory with trailing slash",
              canonicalCli,
              {"desktop", schemaOption, "../cli/schema/"},
              emptyEnv,
              {},
              {});
    expectDir("CLI symlink resolves to canonical directory",
              canonicalCli,
              {"desktop", schemaOption, alias},
              emptyEnv,
              {},
              {});
    expectDir("cwd-relative executable directory",
              canonicalDir(share),
              args,
              emptyEnv,
              "../prefix/bin",
              fallback);
    expectDir("cwd-relative fallback directory",
              canonicalDir(fallback),
              args,
              emptyEnv,
              {},
              "../fallback/schema");

    const auto rejectFile = [&](const char* scenario, const QString& fileName) {
        run(scenario, [&] {
            rejected([&] { DesktopConfig::locateSchemaFile(fileName, cliArgs, emptyEnv); },
                     "fileName");
        });
        run(scenario, [&] {
            rejected([&] { DesktopConfig::locateSchemaFile(fileName, {}, emptyEnv); }, "fileName");
        });
    };
    rejectFile("empty fileName", "");
    rejectFile("parent traversal in fileName", "../escape");
    rejectFile("slash in fileName", "sub/file");
    rejectFile("backslash in fileName", "sub\\file");
    rejectFile("parent directory as fileName", "..");
    rejectFile("double dot within fileName", "a..b");
    rejectFile("current directory as fileName", ".");
    const QString missingFile = "missing.schema.json";
    expectFile("file lookup without schema directory", {}, missingFile, args, emptyEnv, {}, {});
    expectFile("file lookup only requires the marker",
               canonicalCli + "/" + missingFile,
               missingFile,
               cliArgs,
               emptyEnv,
               {},
               {});

    const QStringList sharedArgs{
        "desktop", "--config", configPath, schemaOption, cli, "--platform", "offscreen"};
    run("load ignores the schema and Qt options",
        [&] { checkRealConfig(DesktopConfig::load(sharedArgs, emptyEnv)); });
    expectDir(
        "lookup ignores the config and Qt options", canonicalCli, sharedArgs, emptyEnv, {}, {});
    run("load ignores trailing schema option", [&] {
        checkRealConfig(
            DesktopConfig::load({"desktop", "--config", configPath, schemaOption}, emptyEnv));
    });
    check(failures == 0, "schema path acceptance checks failed");
}
void tests() {
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    QProcessEnvironment env;
    const QStringList args{"desktop"};
    auto defaults = DesktopConfig::load(args, env);
    check(defaults.importEntry().program == "chipcae-import" &&
              defaults.importEntry().prefix.isEmpty(),
          "import defaults");
    check(defaults.previewEntry().program == "chipcae-preview" &&
              defaults.previewEntry().source == "real",
          "preview defaults");
    check(defaults.launcherConfig().solver == "chipcae-solve" &&
              defaults.launcherConfig().mpi == "mpirun" && !defaults.launcherConfig().mock,
          "real defaults");
    check(defaults.eventsFile() == "events.bin" &&
              defaults.runRoot() == QDir::current().absoluteFilePath("runs"),
          "path defaults");
    const auto rejectLoad = [&](const QStringList& argv, const QProcessEnvironment& e) {
        rejected<std::exception>([&] { DesktopConfig::load(argv, e); });
    };
    QJsonObject real =
        QJsonDocument::fromJson(
            R"({"schema":"chipcae.desktop.config/0.1","run_root":"/tmp/real-runs","entrypoints":{"import":{"source":"real","program":"chipcae-import","prefix":[],"material_map":"material-map.json"},"preview":{"source":"real","program":"chipcae-preview","prefix":[]},"solve":{"source":"real","program":"chipcae-solve","mpi":"custom-mpi","events_file":"events.bin"}},"precomputed":{"run_dir":"/tmp/precomputed","manifest_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})")
            .object();
    auto save = [&](QString name, QJsonObject obj) {
        return writeFixture(dir.filePath(name), QJsonDocument(obj).toJson());
    };
    auto rp = save("real.json", real);
    auto mock = real;
    auto entries = mock["entrypoints"].toObject();
    entries["import"] = QJsonObject{{"source", "mock"},
                                    {"program", "python3"},
                                    {"prefix", QJsonArray{"desktop/tools/mock_import.py"}}};
    mock["entrypoints"] = entries;
    auto mixed = DesktopConfig::load({"desktop", "--config", save("mixed.json", mock)}, env);
    check(mixed.importEntry().source == "mock" && mixed.previewEntry().source == "real" &&
              !mixed.launcherConfig().mock,
          "mixed sources");
    entries["preview"] = QJsonObject{{"source", "mock"},
                                     {"program", "python3"},
                                     {"prefix", QJsonArray{"desktop/tools/mock_preview.py"}}};
    entries["solve"] = QJsonObject{{"source", "mock"},
                                   {"program", "desktop/demo/bin/mock-solve"},
                                   {"mpi", "mpirun"},
                                   {"events_file", "events.caelog"}};
    mock["entrypoints"] = entries;
    mock["run_root"] = "/tmp/mock-runs";
    auto mp = save("mock.json", mock);
    env.insert("CHIPCAE_DESKTOP_CONFIG", mp);
    auto m = DesktopConfig::load(args, env);
    check(m.previewEntry().prefix == QStringList{"desktop/tools/mock_preview.py"} &&
              m.previewEntry().program == "python3",
          "preview mapping");
    check(m.importEntry().prefix == QStringList{"desktop/tools/mock_import.py"}, "import mapping");
    check(m.launcherConfig().mock && m.launcherConfig().solver == "desktop/demo/bin/mock-solve" &&
              m.launcherConfig().runRoot == "/tmp/mock-runs" &&
              m.launcherConfig().mpi == "mpirun" && m.eventsFile() == "events.caelog",
          "launcher mapping");
    auto r = DesktopConfig::load({"desktop", "--config", rp}, env);
    checkRealConfig(r);
    DesktopConfig::setCurrent(r);
    check(DesktopConfig::current().runRoot() == r.runRoot(), "process config");
    rejectLoad({"desktop", "--config", dir.filePath("missing")}, env);
    rejectLoad({"desktop", "--config"}, env);
    rejectLoad({"desktop", "--config", writeFixture(dir.filePath("broken"), "{")}, env);
    auto bad = real;
    bad["schema"] = "wrong";
    rejectLoad({"desktop", "--config", save("schema", bad)}, env);
    bad = real;
    bad.remove("run_root");
    rejectLoad({"desktop", "--config", save("missing-field", bad)}, env);
    bad = mock;
    entries = bad["entrypoints"].toObject();
    auto s = entries["solve"].toObject();
    s["source"] = "real";
    entries["solve"] = s;
    bad["entrypoints"] = entries;
    rejectLoad({"desktop", "--config", save("mismatch", bad)}, env);
    bad = real;
    entries = bad["entrypoints"].toObject();
    s = entries["preview"].toObject();
    s["source"] = "mock";
    entries["preview"] = s;
    bad["entrypoints"] = entries;
    rejectLoad({"desktop", "--config", save("reverse", bad)}, env);
    env = QProcessEnvironment();
    env.insert("CHIPCAE_SOLVE", "mock-solve");
    rejectLoad(args, env);
    env.insert("CHIPCAE_SOLVE_SOURCE", "mock");
    check(DesktopConfig::load(args, env).launcherConfig().mock, "override source");
    env = QProcessEnvironment();
    env.insert("CHIPCAE_MPIRUN", "custom-mpi");
    rejectLoad(args, env);
    env.insert("CHIPCAE_SOLVE_SOURCE", "real");
    check(DesktopConfig::load(args, env).launcherConfig().mpi == "custom-mpi", "mpi override");
    for (const auto& key : {QString("CHIPCAE_IMPORT"), QString("CHIPCAE_PREVIEW")}) {
        env = QProcessEnvironment();
        env.insert(key, "python3");
        rejectLoad(args, env);
        env.insert(key + "_SOURCE", "mock");
        auto overridden = DesktopConfig::load(args, env);
        check((key == "CHIPCAE_IMPORT" ? overridden.importEntry() : overridden.previewEntry())
                      .program == "python3",
              "entry override");
    }
    env = QProcessEnvironment();
    bad = mock;
    entries = bad["entrypoints"].toObject();
    s = entries["preview"].toObject();
    s["source"] = "real";
    entries["preview"] = s;
    bad["entrypoints"] = entries;
    rejectLoad({"desktop", "--config", save("prefix-mismatch", bad)}, env);
    for (const auto& field : {QString("source"), QString("program"), QString("prefix")}) {
        bad = real;
        entries = bad["entrypoints"].toObject();
        s = entries["import"].toObject();
        s.remove(field);
        entries["import"] = s;
        bad["entrypoints"] = entries;
        rejectLoad({"desktop", "--config", save("required-" + field, bad)}, env);
    }
    env.insert("CHIPCAE_DESKTOP_CONFIG", dir.filePath("missing-env-file"));
    rejectLoad(args, env);
    QFile contract(CONFIG_CONTRACT);
    check(contract.open(QIODevice::ReadOnly), "contract exists");
    auto text = contract.readAll();
    for (auto word : {"chipcae.desktop.config/0.1",
                      "--config",
                      "CHIPCAE_DESKTOP_CONFIG",
                      "CHIPCAE_IMPORT_SOURCE",
                      "CHIPCAE_PREVIEW_SOURCE",
                      "CHIPCAE_SOLVE_SOURCE",
                      "CHIPCAE_MPIRUN",
                      "events_file",
                      "mock_*.py",
                      "run_root"})
        check(text.contains(word), "contract covers rules");
    schemaPathTests(dir, rp);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        tests();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
    std::cout << "desktop configuration checks passed\n";
}
