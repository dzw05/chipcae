#include "PreviewController.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <vtkDataObject.h>
using namespace chipcae::desktop;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "write fixture");
}
// Exercise the test executable as a black box. The probe keeps the existing mock
// assertions and exercises the optional real-preview branch with controlled environments.
void checkOptionalPreviewAcceptance(const QString& fixtures, const QDir& dir) {
    enum class Expected { Unexecuted, InvalidPath, FailedProgram, RealSuccess };
    const QByteArray unexecuted = "CHIPCAE_PREVIEW: 未执行";
    const QByteArray realSuccess = "D10 real preview tests passed";
    const auto inherited = QProcessEnvironment::systemEnvironment();
    auto environment = inherited;
    environment.remove("CHIPCAE_PREVIEW");
    environment.remove("chipcae_preview");
    const auto failedProgram = dir.filePath("failed preview");
    write(failedProgram, "#!/bin/sh\nprintf 'called\\n' >> \"${0}.calls\"\nexit 23\n");
    check(QFile::setPermissions(failedProgram,
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner),
          "executable failure fixture");
    const auto nonexecutable = dir.filePath("nonexecutable preview");
    write(nonexecutable, "not an executable\n");
    check(QFile::setPermissions(nonexecutable, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
          "nonexecutable fixture permissions");
    int failures = 0;
    const auto run = [&](const char* name, const QProcessEnvironment& env, Expected expected) {
        QProcess probe;
        probe.setProcessEnvironment(env);
        probe.setWorkingDirectory(dir.path());
        probe.start(QCoreApplication::applicationFilePath(),
                    {fixtures, "--d10b0-acceptance-probe"});
        try {
            check(probe.waitForStarted(), "acceptance probe launch");
            check(probe.waitForFinished(55000), "acceptance probe timed out");
            const auto output = probe.readAllStandardOutput();
            const auto error = probe.readAllStandardError();
            const auto lines = output.split('\n');
            check(probe.exitStatus() == QProcess::NormalExit, "acceptance probe crashed");
            if (expected == Expected::Unexecuted) {
                check(probe.exitCode() == 0, "unset/blank CHIPCAE_PREVIEW must succeed");
                check(lines.count("D10 preview tests passed") == 1, "existing mock suite passes");
                check(lines.count(unexecuted) == 1 && output.count(unexecuted + '\n') == 1,
                      "missing byte-exact CHIPCAE_PREVIEW: 未执行 line");
                check(!output.contains(realSuccess),
                      "unexecuted preview cannot claim real success");
                check(!QFileInfo::exists(failedProgram + ".calls"),
                      "unset/blank/lowercase CHIPCAE_PREVIEW must not launch real program");
            } else {
                check(!output.contains(unexecuted),
                      "configured CHIPCAE_PREVIEW must not report 未执行");
                if (expected == Expected::RealSuccess) {
                    check(probe.exitCode() == 0, "real CHIPCAE_PREVIEW must succeed");
                    check(lines.count("D10 preview tests passed") == 1,
                          "existing mock suite passes");
                    check(lines.count(realSuccess) == 1,
                          "missing distinct real-preview success line");
                } else {
                    check(probe.exitCode() != 0, "invalid CHIPCAE_PREVIEW must fail");
                    check(!output.contains(realSuccess),
                          "failed preview cannot claim real success");
                    if (expected == Expected::InvalidPath)
                        check(error.contains("CHIPCAE_PREVIEW"),
                              "path failure diagnostic must contain CHIPCAE_PREVIEW");
                    else {
                        check(error.toLower().contains("preview"),
                              "failed preview must print error()");
                        QFile calls(failedProgram + ".calls");
                        check(calls.open(QIODevice::ReadOnly) && calls.readAll() == "called\n",
                              "configured executable must run exactly once");
                    }
                }
            }
            std::cout << "D10b-0 acceptance: " << name << " passed\n";
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "D10b-0 acceptance: " << name << ": " << e.what() << '\n';
        }
    };
    run("unset", environment, Expected::Unexecuted);
    for (const auto& value : {QString(""), QString(" \t\n ")}) {
        auto env = environment;
        env.insert("CHIPCAE_PREVIEW", value);
        run(value.isEmpty() ? "empty" : "whitespace", env, Expected::Unexecuted);
    }
    auto lowercase = environment;
    lowercase.insert("chipcae_preview", failedProgram);
    run("lowercase only", lowercase, Expected::Unexecuted);
    for (const auto& path : {dir.filePath("missing preview"), dir.path(), nonexecutable}) {
        auto env = environment;
        env.insert("CHIPCAE_PREVIEW", path);
        run(qPrintable(path), env, Expected::InvalidPath);
    }
    auto failing = environment;
    failing.insert("CHIPCAE_PREVIEW", failedProgram);
    run("executable exits 23", failing, Expected::FailedProgram);

    const auto program = inherited.value("CHIPCAE_PREVIEW");
    if (!program.trimmed().isEmpty()) {
        const auto absolute = QFileInfo(program).absoluteFilePath();
        check(QFileInfo(absolute).isFile() && QFileInfo(absolute).isExecutable(),
              "CHIPCAE_PREVIEW real acceptance fixture must be an executable file");
        const auto spaced = dir.filePath(" real preview ");
        check(QFile::link(absolute, spaced), "real executable link with spaces");
        for (const auto& path : {absolute, dir.relativeFilePath(absolute), spaced}) {
            auto env = environment;
            env.insert("CHIPCAE_PREVIEW", path);
            run(qPrintable(path), env, Expected::RealSuccess);
        }
    } else {
        std::cout << "D10b-0 acceptance: real executable 未执行\n";
    }
    check(failures == 0, "D10b-0 optional real preview acceptance failed");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary;
        check(temporary.isValid(), "temporary directory");
        QDir dir(temporary.path());
        const auto stack = dir.filePath("stack with spaces.json");
        const auto poly = dir.filePath("poly set.json");
        write(poly, "{}");
        auto hash = QCryptographicHash::hash("{}", QCryptographicHash::Sha256).toHex();
        auto fixture = QDir(argv[1]).filePath("legacy.vtkhdf");
        PreviewOptions options;
        check(options.maxRatio == 10, "preview default ratio");
        options.nx = 8;
        check(!options.validate().isEmpty(), "unpaired nx rejected");
        options.ny = 9;
        options.levels = 3;
        options.maxRatio = 12;
        options.surface = dir.filePath("exact eb.vtkhdf");
        check(options.save(dir.filePath("options.json")), "save sidecar");
        auto loaded = PreviewOptions::load(dir.filePath("options.json"));
        check(loaded.nx == 8 && loaded.ny == 9 && loaded.levels == 3 && loaded.maxRatio == 12,
              "sidecar round trip");
        for (const auto& mode : {"success",
                                 "illegal",
                                 "bad_progress",
                                 "empty",
                                 "cancel",
                                 "empty_file",
                                 "single_parse",
                                 "nonzero",
                                 "hash",
                                 "bad_digest",
                                 "pdc",
                                 "automatic",
                                 "manifest",
                                 "bad_manifest"}) {
            const bool automatic = QString(mode) == "automatic";
            options.levels = automatic ? 0 : 3;
            options.nx = automatic ? std::optional<int>{} : std::optional<int>{8};
            options.ny = automatic ? std::optional<int>{} : std::optional<int>{9};
            const auto selectedFixture = QString(mode) == "pdc"
                                             ? QDir(argv[1]).filePath("default/out/step-0.vtkhdf")
                                             : fixture;
            QString manifest;
            if (QString(mode).contains("manifest")) {
                manifest = dir.filePath("preview-manifest.json");
                QJsonObject digest{
                    {"nx0", 8},
                    {"ny0", 9},
                    {"L", 2},
                    {"padded_cells", 72},
                    {"nz", 2},
                    {"mesh_sha256", QString(64, QString(mode) == "bad_manifest" ? 'b' : 'a')}};
                QJsonObject m{
                    {"fields", QJsonArray{}},
                    {"mesh", digest},
                    {"steps",
                     QJsonArray{QJsonObject{{"n", 0}, {"time", 0}, {"file", "mesh out.vtkhdf"}}}}};
                write(manifest, QJsonDocument(m).toJson());
            }
            CaseDocument document;
            document.applyJson(Json::parse(R"({"mesh":{"levels":1,"cell_budget":50000}})"));
            auto before = document.toJson();
            QJsonObject config{
                {"mode", mode}, {"fixture", selectedFixture}, {"argv", dir.filePath("argv.json")}};
            Json input =
                Json::parse(QJsonDocument(QJsonObject{{"mock", config}}).toJson().toStdString());
            input["external_refs"] = Json::parse(
                "[{\"path\":\"poly set.json\",\"sha256\":\"" +
                (QString(mode) == "hash" ? std::string(64, '0') : hash.toStdString()) + "\"}]");
            write(stack, QByteArray::fromStdString(input.to_string()));
            PreviewController controller;
            QEventLoop loop;
            bool result = false, done = false;
            int successes = 0, events = 0, diagnostics = 0;
            QObject::connect(&controller, &PreviewController::finished, &loop, [&](bool ok) {
                done = true;
                result = ok;
                loop.quit();
            });
            QObject::connect(&controller,
                             &PreviewController::progress,
                             &loop,
                             [&](const QString& stage, double, const QString&) {
                                 ++events;
                                 if (stage == "success")
                                     ++successes;
                             });
            QObject::connect(&controller,
                             &PreviewController::diagnostic,
                             &loop,
                             [&](const QByteArray&) { ++diagnostics; });
            bool started = controller.start(&document,
                                            stack,
                                            dir.filePath("mesh out.vtkhdf"),
                                            options,
                                            PREVIEW_PYTHON,
                                            {PREVIEW_MOCK},
                                            manifest);
            if (QString(mode) == "hash") {
                check(!started && controller.error() == "INPUT_HASH_MISMATCH",
                      "hash blocks launch");
                continue;
            }
            check(started, "start preview");
            if (QString(mode) == "cancel")
                QTimer::singleShot(100, &controller, &PreviewController::cancel);
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(5000);
            loop.exec();
            check(done, "preview completion timed out");
            if (QString(mode) == "success" || QString(mode) == "pdc" || automatic ||
                QString(mode) == "manifest") {
                if (!result)
                    std::cerr << controller.error().toStdString() << '\n';
                check(result && successes == 1 && events == 2 && diagnostics > 0,
                      "fragmented JSONL, single success, independent stderr");
                auto mesh = document.meshOptions();
                check(mesh.levels == (automatic ? 1 : 3) && mesh.nx == (automatic ? 4 : 8) &&
                          mesh.ny == (automatic ? 5 : 9) && mesh.maxRatio == 12,
                      "effective options write-back L+1");
                check(controller.mesh() && !controller.blocks().isEmpty(),
                      "ResultSession inspection");
                check(controller.digest().has_value() && controller.digest()->nz == 2,
                      "success digest extraction");
                QFile args(dir.filePath("argv.json"));
                check(args.open(QIODevice::ReadOnly), "argv");
                auto o = QJsonDocument::fromJson(args.readAll()).object();
                check(o["polyset"].toString() == poly && o["nx"].toInt() == (automatic ? 0 : 8) &&
                          o["ny"].toInt() == (automatic ? 0 : 9) &&
                          o["levels"].toInt() == options.levels &&
                          o["max_ratio"].toDouble() == 12 &&
                          o["surface"].toString() == options.surface,
                      "real CLI arguments with spaces");
            } else {
                check(!result && !controller.error().isEmpty(), "invalid run rejected");
                check(before == document.toJson(), "failed run must not change case");
            }
        }
        const auto program = QProcessEnvironment::systemEnvironment().value("CHIPCAE_PREVIEW");
        if (program.trimmed().isEmpty()) {
            std::cout << "CHIPCAE_PREVIEW: 未执行\n";
        } else {
            const auto absoluteProgram = QFileInfo(program).absoluteFilePath();
            const QFileInfo executable(absoluteProgram);
            check(executable.exists() && executable.isFile() && executable.isExecutable(),
                  "CHIPCAE_PREVIEW must name an existing executable file");
            const auto realStack = dir.filePath("real-stack.json");
            const auto realOutput = dir.filePath("real-mesh.vtkhdf");
            write(realStack,
                  R"({"id":"d10b0_tiny","unit":"m","outline":{"box":[0,0,0.007,0.005]},
                      "symmetry":"none","package_type":"FCBGA",
                      "layers":[{"name":"solid","thickness":0.001,"kind":"Homogenized",
                                 "material":"solid_eq","n_elem_min":1,"grading":1}]})");
            CaseDocument document;
            document.applyJson(Json::parse(R"({"mesh":{"levels":1,"cell_budget":50000}})"));
            PreviewOptions realOptions;
            realOptions.nx = 4;
            realOptions.ny = 4;
            realOptions.levels = 1;
            realOptions.maxRatio = 10;
            PreviewController controller;
            QEventLoop loop;
            bool done = false, result = false, timedOut = false;
            QObject::connect(&controller, &PreviewController::finished, &loop, [&](bool ok) {
                done = true;
                result = ok;
                loop.quit();
            });
            const bool started = controller.start(
                &document, realStack, realOutput, realOptions, absoluteProgram, {});
            if (!started)
                std::cerr << controller.error().toStdString() << '\n';
            check(started, "CHIPCAE_PREVIEW real preview launch failed");
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
                timedOut = true;
                controller.cancel();
                loop.quit();
            });
            timer.start(50000);
            if (!done)
                loop.exec();
            timer.stop();
            if (!result)
                std::cerr << controller.error().toStdString() << '\n';
            check(!timedOut, "CHIPCAE_PREVIEW real preview timed out");
            check(done, "CHIPCAE_PREVIEW real preview did not finish");
            check(result, "CHIPCAE_PREVIEW real preview finished unsuccessfully");
            check(controller.error().isEmpty(), "real preview error must be empty");
            check(controller.mesh() != nullptr, "real preview mesh must exist");
            check(controller.digest().has_value(), "real preview digest must exist");
            const auto& digest = *controller.digest();
            const auto meshHash = QString::fromStdString(digest.mesh_sha256);
            check(meshHash.size() == 64 &&
                      QRegularExpression("^[0-9a-f]{64}$").match(meshHash).hasMatch(),
                  "real preview digest hash must be 64 lowercase hexadecimal characters");
            check(digest.nx0 >= 1 && digest.ny0 >= 1 && digest.nz >= 1 && digest.L >= 0 &&
                      digest.padded_cells >= 0,
                  "real preview digest integer bounds");
            std::cout << "D10 real preview tests passed\n";
        }
        if (!app.arguments().contains("--d10b0-acceptance-probe"))
            checkOptionalPreviewAcceptance(QFileInfo(argv[1]).absoluteFilePath(), dir);
        std::cout << "D10 preview tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
