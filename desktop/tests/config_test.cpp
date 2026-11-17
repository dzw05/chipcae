#include "DesktopConfig.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
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
    auto write = [&](QString name, QByteArray data) {
        QString path = dir.filePath(name);
        QFile f(path);
        check(f.open(QIODevice::WriteOnly), "write fixture");
        f.write(data);
        return path;
    };
    auto rejected = [&](QStringList argv, QProcessEnvironment e) {
        try {
            DesktopConfig::load(argv, e);
        } catch (const std::exception& ex) {
            check(QString::fromUtf8(ex.what()).size() > 8, "readable error");
            return;
        }
        throw std::runtime_error("invalid config accepted");
    };
    QJsonObject real =
        QJsonDocument::fromJson(
            R"({"schema":"chipcae.desktop.config/0.1","run_root":"/tmp/real-runs","entrypoints":{"import":{"source":"real","program":"chipcae-import","prefix":[],"material_map":"material-map.json"},"preview":{"source":"real","program":"chipcae-preview","prefix":[]},"solve":{"source":"real","program":"chipcae-solve","mpi":"custom-mpi","events_file":"events.bin"}},"precomputed":{"run_dir":"/tmp/precomputed","manifest_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})")
            .object();
    auto save = [&](QString name, QJsonObject obj) {
        return write(name, QJsonDocument(obj).toJson());
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
    check(r.runRoot() == "/tmp/real-runs" && r.importEntry().materialMap == "material-map.json" &&
              r.precomputed().runDir == "/tmp/precomputed" &&
              r.launcherConfig().mpi == "custom-mpi",
          "CLI precedence and metadata");
    DesktopConfig::setCurrent(r);
    check(DesktopConfig::current().runRoot() == r.runRoot(), "process config");
    rejected({"desktop", "--config", dir.filePath("missing")}, env);
    rejected({"desktop", "--config"}, env);
    rejected({"desktop", "--config", write("broken", "{")}, env);
    auto bad = real;
    bad["schema"] = "wrong";
    rejected({"desktop", "--config", save("schema", bad)}, env);
    bad = real;
    bad.remove("run_root");
    rejected({"desktop", "--config", save("missing-field", bad)}, env);
    bad = mock;
    entries = bad["entrypoints"].toObject();
    auto s = entries["solve"].toObject();
    s["source"] = "real";
    entries["solve"] = s;
    bad["entrypoints"] = entries;
    rejected({"desktop", "--config", save("mismatch", bad)}, env);
    bad = real;
    entries = bad["entrypoints"].toObject();
    s = entries["preview"].toObject();
    s["source"] = "mock";
    entries["preview"] = s;
    bad["entrypoints"] = entries;
    rejected({"desktop", "--config", save("reverse", bad)}, env);
    env = QProcessEnvironment();
    env.insert("CHIPCAE_SOLVE", "mock-solve");
    rejected(args, env);
    env.insert("CHIPCAE_SOLVE_SOURCE", "mock");
    check(DesktopConfig::load(args, env).launcherConfig().mock, "override source");
    env = QProcessEnvironment();
    env.insert("CHIPCAE_MPIRUN", "custom-mpi");
    rejected(args, env);
    env.insert("CHIPCAE_SOLVE_SOURCE", "real");
    check(DesktopConfig::load(args, env).launcherConfig().mpi == "custom-mpi", "mpi override");
    for (const auto& key : {QString("CHIPCAE_IMPORT"), QString("CHIPCAE_PREVIEW")}) {
        env = QProcessEnvironment();
        env.insert(key, "python3");
        rejected(args, env);
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
    rejected({"desktop", "--config", save("prefix-mismatch", bad)}, env);
    for (const auto& field : {QString("source"), QString("program"), QString("prefix")}) {
        bad = real;
        entries = bad["entrypoints"].toObject();
        s = entries["import"].toObject();
        s.remove(field);
        entries["import"] = s;
        bad["entrypoints"] = entries;
        rejected({"desktop", "--config", save("required-" + field, bad)}, env);
    }
    env.insert("CHIPCAE_DESKTOP_CONFIG", dir.filePath("missing-env-file"));
    rejected(args, env);
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
