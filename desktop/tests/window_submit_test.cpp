#include "DesktopConfig.hpp"
#include "LocalJobService.hpp"
#include "MainWindow.hpp"
#include "PreviewController.hpp"
#include "ResultView.hpp"
#include "SubmitDialog.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVTKOpenGLNativeWidget.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vtkDataObject.h>
using namespace chipcae::desktop;
namespace {
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "fixture write");
}
QByteArray read(const QString& path) {
    QFile f(path);
    check(f.open(QIODevice::ReadOnly), "fixture read");
    return f.readAll();
}
bool waitFor(const std::function<bool()>& predicate, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return predicate();
}
void copyTree(const QString& from, const QString& to) {
    check(QDir().mkpath(to), "fixture directory");
    for (const auto& entry : QDir(from).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        auto target = to + '/' + entry.fileName();
        if (entry.isDir())
            copyTree(entry.absoluteFilePath(), target);
        else
            check(QFile::copy(entry.absoluteFilePath(), target), "copy result fixture");
    }
}
QJsonObject profile() {
    return {{"schema", "chipcae.run_profile/1.0"},
            {"ranks", 1},
            {"backend", "cpu"},
            {"gpus", 0},
            {"threads_per_rank", 1},
            {"launcher", "direct"}};
}
QStringList runDirectories(const QString& root) {
    QStringList result;
    QDirIterator it(root, {"job.pb"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        result << QFileInfo(it.next()).absolutePath();
    return result;
}
// PNG chunk parser: compare raw UTF-8 bytes, never pixels or OCR.
void checkWatermark(const QString& file, const QByteArray& expected) {
    auto bytes = read(file);
    check(bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")), "PNG signature");
    bool found = false;
    for (qsizetype pos = 8; pos + 12 <= bytes.size();) {
        quint32 length = 0;
        for (int i = 0; i < 4; ++i)
            length = (length << 8) | static_cast<unsigned char>(bytes[pos + i]);
        check(length <= static_cast<quint64>(bytes.size() - pos - 12), "PNG chunk bounds");
        if (bytes.mid(pos + 4, 4) == "tEXt") {
            auto text = bytes.mid(pos + 8, length);
            auto separator = text.indexOf('\0');
            if (separator >= 0 && text.left(separator) == "chipcae.watermark") {
                check(text.mid(separator + 1) == expected, "watermark UTF-8 equality");
                found = true;
            }
        }
        pos += length + 12;
    }
    check(found, "missing chipcae.watermark tEXt chunk");
}
struct Fixture {
    QTemporaryDir temp;
    QString samples;
    Fixture(QString input) : samples(std::move(input)) {
        check(temp.isValid(), "temporary directory");
    }
    QString root() const { return temp.filePath("runs"); }
    void configure(const QString& fallback = {}, bool fail = false) {
        // Self-hosted solver waits for release; it never acknowledges STOP.
        auto script = temp.filePath("mock-solve");
        auto exe = QCoreApplication::applicationFilePath();
        check(!exe.contains('\''), "fixture executable quoting");
        write(script, ("#!/bin/sh\nexec '" + exe + "' --fixture-solver \"$@\"\n").toUtf8());
        check(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
              "solver permissions");
        QJsonObject config{{"schema", "chipcae.desktop.config/0.1"},
                           {"run_root", root()},
                           {"entrypoints",
                            QJsonObject{{"import",
                                         QJsonObject{{"source", "real"},
                                                     {"program", "chipcae-import"},
                                                     {"prefix", QJsonArray{}}}},
                                        {"preview",
                                         QJsonObject{{"source", "mock"},
                                                     {"program", PYTHON_EXECUTABLE},
                                                     {"prefix", QJsonArray{PREVIEW_MOCK}}}},
                                        {"solve",
                                         QJsonObject{{"source", "mock"},
                                                     {"program", script},
                                                     {"mpi", "mpirun"},
                                                     {"events_file", "fixture.caelog"}}}}}};
        if (!fallback.isEmpty())
            config["precomputed"] =
                QJsonObject{{"run_dir", fallback}, {"manifest_sha256", QString(64, 'a')}};
        auto path = temp.filePath("config.json");
        write(path, QJsonDocument(config).toJson());
        DesktopConfig::setCurrent(
            DesktopConfig::load({"desktop", "--config", path}, QProcessEnvironment{}));
        qputenv("D14B_FIXTURE_RESULTS", (samples + "/default").toUtf8());
        qputenv("D14B_FIXTURE_GATE", temp.filePath("release").toUtf8());
        qputenv("D14B_FIXTURE_FAIL", fail ? "1" : "0");
    }
    void release() { write(temp.filePath("release"), "go"); }
    QString result(const QString& source) {
        auto dir = temp.filePath("result-" + source);
        copyTree(samples + "/default", dir);
        if (source != "missing")
            write(dir + "/provenance.json",
                  QJsonDocument(QJsonObject{{"schema", "chipcae.provenance/0.1"},
                                            {"source", source},
                                            {"generated_at", "2026-11-17T01:02:03Z"}})
                      .toJson());
        return dir;
    }
    void openCase(MainWindow& w) {
        check(w.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
              "open case fixture");
    }
};
jobs::Job persistedJob(const QString& root) {
    auto dirs = runDirectories(root);
    check(dirs.size() == 1, "exactly one service job persisted");
    jobs::Job job;
    check(job.ParseFromString(read(dirs.first() + "/job.pb").toStdString()),
          "valid persisted service Job");
    return job;
}
QPushButton* button(MainWindow& w, const QString& text) {
    for (auto* b : w.findChildren<QPushButton*>())
        if (b->text().compare(text, Qt::CaseInsensitive) == 0)
            return b;
    return nullptr;
}
void sourceChecks(MainWindow& w, const QString& source) {
    auto badge = w.sourceBadgeText(), banner = w.sourceBannerText();
    auto watermark = w.provenanceWatermark();
    if (source == "real") {
        check(badge.contains("真实计算"), "real badge");
        check(banner.isEmpty() && watermark.isEmpty(), "pure real has no banner/watermark");
    } else {
        auto label = source == "mock"          ? QString("MOCK")
                     : source == "precomputed" ? QString("预计算")
                                               : QString("来源未知");
        check(badge.contains(label), "non-real source badge");
        check(!banner.isEmpty(), "non-real persistent banner");
        check(watermark == (source == "precomputed" ? "预计算" : "MOCK"), "source watermark");
        check(!(badge + banner + w.monitorStatusText()).contains("真实计算"),
              "non-real must not claim real computation");
    }
    for (auto name : {"warpage_pv", "U_mag_max", "T_max"}) {
        auto original = w.resultView()->summary(name);
        check(!original.isEmpty(), "original summary available");
        auto summary = w.summaryWithSource(name);
        check(summary.contains(original), "source summary preserves ResultView text");
        if (source == "real")
            check(summary == original, "real summary unchanged");
        else if (source == "mock" || source == "precomputed")
            check(summary.endsWith(source == "mock" ? "（MOCK）" : "（预计算）"),
                  "source summary suffix");
    }
}
} // namespace
int main(int argc, char** argv) {
    // A real child process gives the service observable completion/failure without mutation seams.
    if (argc > 1 && QByteArray(argv[1]) == "--fixture-solver") {
        QCoreApplication app(argc, argv);
        auto args = app.arguments();
        auto out = args.value(args.indexOf("--out") + 1);
        QElapsedTimer deadline;
        deadline.start();
        while (!QFile::exists(qEnvironmentVariable("D14B_FIXTURE_GATE")) &&
               deadline.elapsed() < 60000)
            QThread::msleep(10);
        if (qEnvironmentVariable("D14B_FIXTURE_FAIL") == "1")
            return 9;
        copyTree(qEnvironmentVariable("D14B_FIXTURE_RESULTS"), out);
        write(out + "/mock-events.jsonl",
              "{\"exit\":{\"exit_code\":0,\"reason\":\"end_reached\"}}\n");
        return 0;
    }
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    if (argc != 2)
        return 2;
    int failures = 0;
    auto test = [&](const char* name, const std::function<void()>& run) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("solver fixture self-check", [&] {
        Fixture f(argv[1]);
        f.configure();
        auto config = DesktopConfig::current().launcherConfig();
        config.eventsFile = "fixture.caelog";
        LocalJobService broker(config);
        auto path = f.temp.filePath("fixture-profile.json");
        write(path, QJsonDocument(profile()).toJson());
        jobs::SubmitRequest request;
        request.set_client_request_id(
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        auto job = broker.Submit(
            request, QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json", path);
        check(!terminal(job.state()), "fixture service can start solver");
        jobs::JobRef ref;
        ref.set_run_id(job.run_id());
        f.release();
        check(waitFor([&] { return terminal(broker.Get(ref).state()); }), "fixture process exits");
        check(broker.Get(ref).state() == jobs::JOB_STATE_COMPLETED,
              "fixture emits successful KernelExit");
        ResultSession session;
        check(session.open(QString::fromStdString(job.result_dir()) + "/manifest.json"),
              "solver fixture produces readable results");
    });
    test("Preview and Submit actions", [&] {
        MainWindow w;
        QStringList labels;
        for (auto* action : w.findChildren<QAction*>())
            labels << action->text();
        auto text = labels.join('\n');
        check(text.contains("Preview") && text.contains("Submit"),
              "main window exposes Preview and Submit actions");
    });
    test("dialog defaults", [&] {
        SubmitDialog d;
        check(d.profile() == profile(), "default run-profile schema/ranks/cpu/direct fields");
        check(d.meshNx() == 4, "meshNx defaults to four");
        check(!d.profile().contains("np") && !d.profile().contains("nx"), "profile excludes np/nx");
    });
    test("profile persistence and atomic validation", [&] {
        Fixture f(argv[1]);
        SubmitDialog d;
        auto custom = profile();
        custom["threads_per_rank"] = 2;
        d.setProfile(custom);
        d.setMeshNx(8);
        check(d.profile() == custom && d.meshNx() == 8, "profile and mesh setters");
        auto file = f.temp.filePath("profile.json");
        check(d.saveProfile(file), "save valid profile");
        SubmitDialog loaded;
        check(loaded.loadProfile(file) && loaded.profile() == custom, "profile round trip");
        for (auto bytes : {QByteArray("{"),
                           QByteArray("{}"),
                           QByteArray("{\"schema\":\"wrong\",\"ranks\":1}"),
                           QByteArray("{\"schema\":\"chipcae.run_profile/1.0\",\"ranks\":0}")}) {
            write(file, bytes);
            check(!loaded.loadProfile(file) && loaded.profile() == custom,
                  "invalid profile leaves current value unchanged");
        }
    });
    test("missing case and invalid profiles have no submit side effects", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        check(!w.caseIsOpen() && !w.submitCurrentCase(), "missing case rejected");
        check(!QFile::exists(f.root() + "/submit-profile.json") &&
                  runDirectories(f.root()).isEmpty(),
              "no writes or jobs without case");
        f.openCase(w);
        check(w.caseIsOpen(), "opened document is a case");
        for (auto bad : {QJsonObject{},
                         QJsonObject{{"schema", "wrong"}, {"ranks", 1}},
                         QJsonObject{{"schema", "chipcae.run_profile/1.0"}},
                         QJsonObject{{"schema", "chipcae.run_profile/1.0"}, {"ranks", 1.5}}}) {
            w.setSubmitProfile(bad);
            check(!w.submitCurrentCase(), "invalid profile rejected");
            check(!QFile::exists(f.root() + "/submit-profile.json") &&
                      runDirectories(f.root()).isEmpty(),
                  "invalid profile has no side effects");
        }
        check(runDirectories(f.root()).isEmpty(), "service has no persisted jobs");
    });
    test("preview finish and busy guards", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        // Adapt the CLI fixture outside the schema-validated document.
        auto previewScript = f.temp.filePath("mock_window_preview.py");
        auto fixtureStack = f.temp.filePath("preview-input.json");
        write(fixtureStack,
              QJsonDocument(
                  QJsonObject{
                      {"mock", QJsonObject{{"fixture", QString(argv[1]) + "/legacy.vtkhdf"}}}})
                  .toJson());
        auto encoded = [](const QString& value) {
            auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
            return json.mid(1, json.size() - 2);
        };
        write(previewScript,
              "import sys, runpy\nsys.argv[1] = " + encoded(fixtureStack) + "\nrunpy.run_path(" +
                  encoded(PREVIEW_MOCK) + ", run_name='__main__')\n");
        bool finished = false, success = false;
        // Controllers are QObject children; subscribe before start, including lazily created
        // controllers.
        auto connectController = [&] {
            for (auto* controller : w.findChildren<PreviewController*>())
                QObject::connect(controller, &PreviewController::finished, &w, [&](bool ok) {
                    finished = true;
                    success = ok;
                });
        };
        connectController();
        bool started = w.previewMesh(PYTHON_EXECUTABLE, {previewScript});
        check(started, "previewMesh must start valid fixture");
        connectController();
        check(!w.previewMesh(PYTHON_EXECUTABLE, {previewScript}) && !w.submitCurrentCase(),
              "preview excludes duplicate preview and submit");
        check(waitFor([&] { return finished; }) && success,
              "preview finished(true) within timeout");
        auto controllers = w.findChildren<PreviewController*>();
        check(!controllers.isEmpty() && controllers.first()->mesh() != nullptr,
              "completed preview has mesh");
    });
    test("submit auto monitor and completed results", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        check(w.submitProfile() == profile(), "window default profile");
        check(w.submitCurrentCase(), "valid case submits");
        check(!w.submitCurrentCase() && !w.previewMesh(), "nonterminal job excludes starts");
        auto job = persistedJob(f.root());
        check(!job.run_id().empty() && !terminal(job.state()), "service created nonterminal job");
        auto submitted = QJsonDocument::fromJson(
                             read(QString::fromStdString(job.result_dir()) + "/inputs/case.json"))
                             .object();
        check(submitted["mesh"].toObject()["nx"].toInt() == 4,
              "dialog default meshNx reaches submitted case");
        check(QJsonDocument::fromJson(read(f.root() + "/submit-profile.json")).object() ==
                  profile(),
              "persisted submit profile");
        check(waitFor([&] { return w.monitorStatusText().contains("fixture.caelog"); }),
              "monitor configured event filename");
        check(w.monitorStatusText().contains("CAELOG1") &&
                  !w.monitorStatusText().contains("Legacy CAELOG1"),
              "uppercase monitor format");
        f.release();
        check(waitFor([&] { return persistedJob(f.root()).state() == jobs::JOB_STATE_COMPLETED; }),
              "fixture completed enum");
        check(waitFor([&] { return w.resultView() != nullptr; }),
              "completion automatically opens manifest");
        auto* view = w.resultView();
        waitFor([] { return false; }, 600);
        check(w.resultView() == view, "result opened once");
        check(!w.openResultsPath(f.temp.filePath("missing.json")) && w.resultView() == view,
              "failed manifest preserves view");
    });
    for (auto source : {"real", "mock", "precomputed", "missing", "REAL"}) {
        test((QString("provenance and screenshot ") + source).toUtf8().constData(), [&] {
            Fixture f(argv[1]);
            MainWindow w;
            auto dir = f.result(source);
            check(w.openResultsPath(dir + "/manifest.json"), "open provenance fixture");
            sourceChecks(w, source);
            auto png = f.temp.filePath("capture.png");
            check(w.captureResults(png), "captureResults wrapper succeeds");
            checkWatermark(png, w.provenanceWatermark().toUtf8());
        });
    }
    for (auto source : {"precomputed", "missing", "unconfigured", "absent-manifest"}) {
        test((QString("failed job fallback ") + source).toUtf8().constData(), [&] {
            Fixture f(argv[1]);
            QString fallback;
            if (QString(source) != "unconfigured")
                fallback = QString(source) == "absent-manifest" ? f.temp.filePath("absent")
                                                                : f.result(source);
            f.configure(fallback, true);
            MainWindow w;
            f.openCase(w);
            check(w.submitCurrentCase(), "failure fixture submits");
            check(!persistedJob(f.root()).run_id().empty(), "failure job created");
            f.release();
            check(waitFor([&] { return persistedJob(f.root()).state() == jobs::JOB_STATE_FAILED; }),
                  "fixture failed enum");
            if (QString(source) == "unconfigured" || QString(source) == "absent-manifest") {
                waitFor([] { return false; }, 600);
                check(!w.resultView(), "missing fallback never opens results");
            } else {
                check(waitFor([&] { return w.resultView() != nullptr; }),
                      "failed job automatically opens fallback");
                check(w.sourceBannerText().contains("现场求解失败，显示预计算结果"),
                      "failure banner");
                check(!w.sourceBannerText().contains("真实计算"),
                      "fallback never presented as real");
                if (QString(source) == "precomputed") {
                    check(w.provenanceWatermark() == "预计算" &&
                              w.sourceBadgeText().contains("预计算"),
                          "fallback retains provenance");
                    check(w.sourceBannerText().contains("2026-11-17") &&
                              w.sourceBannerText().contains("09:02:03"),
                          "generated_at in Shanghai time");
                } else
                    check(w.sourceBadgeText().contains("来源未知") &&
                              w.provenanceWatermark() == "MOCK",
                          "missing fallback provenance");
            }
        });
    }
    test("monitor status format, filename and source", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        auto dir = f.result("mock");
        check(w.openResultsPath(dir + "/manifest.json"), "open monitored result");
        w.monitorRun(dir, EventFormat::LegacyCaelog1, nullptr, {}, "custom-events.caelog");
        check(waitFor([&] { return w.monitorStatusText().contains("custom-events.caelog"); }),
              "status contains configured event filename");
        auto status = w.monitorStatusText();
        check(status.contains("CAELOG1") && !status.contains("Legacy CAELOG1"),
              "monitor format label");
        check(status.contains("MOCK") && !status.contains("真实计算"), "monitor source label");
    });
    test("screenshot wrapper writes provenance text chunk", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        check(w.openResultsPath(f.result("mock") + "/manifest.json"), "open screenshot fixture");
        auto png = f.temp.filePath("mock.png");
        check(w.captureResults(png), "captureResults wrapper succeeds");
        check(w.provenanceWatermark() == "MOCK", "mock screenshot watermark");
        checkWatermark(png, w.provenanceWatermark().toUtf8());
    });
    test("manual stack preserves real source", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        check(w.openResultsPath(f.result("real") + "/manifest.json"), "open real results");
        check(w.loadHandwrittenStack(), "load handwritten template");
        auto text = w.sourceBadgeText() + w.sourceBannerText();
        check(text.contains("叠层为手写模板") || text.contains("manual_stack"),
              "manual stack degradation label");
        check(w.sourceBadgeText().contains("真实计算") && w.provenanceWatermark().isEmpty(),
              "manual stack source remains real");
    });
    test("STOP and Cancel after thirty seconds without ACK", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        check(w.submitCurrentCase(), "control fixture submits");
        check(!persistedJob(f.root()).run_id().empty(), "control job created");
        check(waitFor([&] { return persistedJob(f.root()).state() == jobs::JOB_STATE_RUNNING; }),
              "control job running before STOP");
        auto dirs = runDirectories(f.root());
        check(dirs.size() == 1, "one run directory");
        auto* stop = button(w, "STOP");
        check(stop && stop->isEnabled(), "STOP UI available");
        stop->click();
        check(waitFor([&] {
                  auto lines = read(dirs.first() + "/control.jsonl").split('\n');
                  for (const auto& line : lines)
                      if (QJsonDocument::fromJson(line).object()["action"] == "STOP")
                          return true;
                  return false;
              }),
              "STOP reaches Control API file");
        auto* cancel = button(w, "Cancel");
        check(!cancel || !cancel->isEnabled() || cancel->isHidden(),
              "Cancel unavailable before timeout");
        QElapsedTimer elapsed;
        elapsed.start();
        check(waitFor(
                  [&] {
                      cancel = button(w, "Cancel");
                      return cancel && cancel->isEnabled() && !cancel->isHidden();
                  },
                  33000),
              "Cancel exposed after ACK timeout");
        check(elapsed.elapsed() >= 29000, "Cancel waits thirty seconds");
        cancel->click();
        check(waitFor([&] { return persistedJob(f.root()).state() == jobs::JOB_STATE_CANCELLED; }),
              "Cancel API changes job state");
    });
    std::cout << "Acceptance failures: " << failures << '\n';
    return failures ? 1 : 0;
}
