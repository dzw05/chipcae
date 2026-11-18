#include "AssemblyImport.hpp"
#include "CaseTreeModel.hpp"
#include "CaseTreeView.hpp"
#include "DesktopConfig.hpp"
#include "ImportController.hpp"
#include "LocalJobService.hpp"
#include "MainWindow.hpp"
#include "PreviewController.hpp"
#include "ResultControls.hpp"
#include "ResultView.hpp"
#include "SchemaForm.hpp"
#include "StackEditor.hpp"
#include "SubmitDialog.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStringList>
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
// Inject a stale reference without exposing test-only setters in MainWindow's API.
template <typename Tag, typename Tag::Type Member>
struct PrivateMemberAccess {
    friend typename Tag::Type member(Tag) { return Member; }
};
struct RunIdMember {
    using Type = QString MainWindow::*;
    friend Type member(RunIdMember);
};
template struct PrivateMemberAccess<RunIdMember, &MainWindow::runId_>;

void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "fixture write");
}
// These fixtures exercise the window's locator inputs, not the process environment.
QString
schemaFixture(const QTemporaryDir& temp, const QString& relative, const QByteArray& schema = "{}") {
    check(temp.isValid(), "schema temporary directory");
    const auto directory = temp.filePath(relative);
    check(QDir().mkpath(directory), "schema fixture directory");
    write(directory + "/case-1.0.schema.json", schema);
    return directory;
}
void checkSchemaHit(const MainWindow& window,
                    const QString& expectedDirectory,
                    const QStringList& args,
                    const QProcessEnvironment& env,
                    const QString& executableDir = {},
                    const QString& fallback = {}) {
    const auto expected = QFileInfo(expectedDirectory).canonicalFilePath();
    check(!expected.isEmpty(), "expected schema fixture has a canonical directory");
    check(window.schemaDirectory() == expected,
          "window must use expected canonical schema directory");
    check(window.schemaDirectory() ==
              DesktopConfig::locateSchemaDir(args, env, executableDir, fallback),
          "window must preserve locator result exactly");
}
void checkSchemaError(const QStringList& args,
                      const QProcessEnvironment& env,
                      const QString& fallback,
                      const QString& overrideName) {
    bool locatorThrew = false, windowThrew = false;
    QByteArray locatorError, windowError;
    try {
        DesktopConfig::locateSchemaDir(args, env, {}, fallback);
    } catch (const std::runtime_error& error) {
        locatorThrew = true;
        locatorError = error.what();
    }
    try {
        MainWindow window(args, env, {}, fallback);
    } catch (const std::runtime_error& error) {
        windowThrew = true;
        windowError = error.what();
    }
    // Keep assertions outside the catches so a failed check cannot masquerade as rejection.
    check(locatorThrew, "invalid fixture must be rejected by locator");
    check(windowThrew, "window must propagate invalid schema override instead of falling back");
    check(windowError == locatorError, "window must preserve complete locator error bytes");
    check(QString::fromUtf8(windowError).contains(overrideName),
          "schema error must identify the invalid override");
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
    QString previewArguments() const { return temp.filePath("preview-argv.json"); }
    QString previewScript(const QString& gate = {}, bool fail = false) {
        // Adapt the CLI fixture outside the schema-validated document.
        auto previewScript = temp.filePath("mock_window_preview.py");
        auto fixtureStack = temp.filePath("preview-input.json");
        write(fixtureStack,
              QJsonDocument(QJsonObject{{"mock",
                                         QJsonObject{{"fixture", samples + "/legacy.vtkhdf"},
                                                     {"argv", previewArguments()},
                                                     {"mode", fail ? "nonzero" : "success"}}}})
                  .toJson());
        auto encoded = [](const QString& value) {
            auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
            return json.mid(1, json.size() - 2);
        };
        QByteArray wait;
        if (!gate.isEmpty())
            wait = "from pathlib import Path\nimport time\nPath(" + encoded(gate + ".ready") +
                   ").write_text('ready')\nwhile not Path(" + encoded(gate) +
                   ").exists():\n    time.sleep(0.01)\n";
        write(previewScript,
              "import sys, runpy\n" + wait + "sys.argv[1] = " + encoded(fixtureStack) +
                  "\nrunpy.run_path(" + encoded(PREVIEW_MOCK) + ", run_name='__main__')\n");
        return previewScript;
    }
    void openCase(MainWindow& w) {
        check(w.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
              "open case fixture");
    }
};
QString assemblyInput() {
    return QString(CHIPCAE_REPOSITORY) +
           "/schema/examples/assembly/package_bulk_parts.assembly.json";
}
QString assemblyMock() {
    return QString(CHIPCAE_REPOSITORY) + "/desktop/tools/mock_assembly_import.py";
}
void importAssembly(MainWindow& w,
                    const QString& input,
                    bool accepted = true,
                    const QString& script = assemblyMock()) {
    QObject receiver;
    int completions = 0;
    bool replaced = false;
    QObject::connect(
        w.importController(), &ImportController::finished, &receiver, [&](bool changed) {
            ++completions;
            replaced = changed;
        });
    check(w.importAssemblyPath(input, PYTHON_EXECUTABLE, {script}),
          "assembly import must start valid schema input");
    check(waitFor([&] { return completions != 0; }), "assembly emits finished within timeout");
    check(completions == 1 && replaced == accepted && !w.importController()->busy(),
          "assembly completion reports expected replacement and becomes idle");
    check(w.importController()->outcome() ==
              (accepted ? ImportOutcome::Accepted : ImportOutcome::Rejected),
          "assembly has expected Accepted or Rejected outcome");
}
CaseTreeModel* assemblyTree(MainWindow& w) {
    auto* tree = w.findChild<CaseTreeModel*>();
    check(tree != nullptr, "window owns case tree");
    return tree;
}
void checkAssembly(MainWindow& w, const QString& annotation) {
    const auto stack = w.document()->stackJson();
    check(stack.contains("layers") && stack.at("layers").is_array() &&
              stack.at("layers").size() == 1,
          "assembly mock installs exactly one layer");
    auto* editor = w.findChild<StackEditor*>();
    check(editor && editor->model()->rowCount() == 1, "assembly refreshes stack editor row");
    const auto& regions = stack.at("layers")[0].at("regions");
    check(regions.is_array() && regions.size() == 1 &&
              regions[0].at("name").as<std::string>() == "lid" &&
              regions[0].at("body").as<std::string>() == "lid",
          "imported layer contains lid region");
    auto* tree = assemblyTree(w);
    auto indexed = tree->locate("/layers/0/regions/0", "bodies-index");
    check(indexed.isValid() && indexed.parent().isValid(), "lid appears under Bodies index");
    check(indexed.parent().data(CaseTreeModel::BodyAnnotationRole) == annotation,
          "Bodies annotation equals top-level report part summary");
    auto displayed = tree->locate("/layers/0/regions/0", "stack");
    check(displayed.isValid() && displayed.parent().isValid() &&
              displayed.parent().data(CaseTreeModel::BodyAnnotationRole).toString() == QString(""),
          "stack display group has no body annotation");
}
// Keep the real importer and adapt only its report inside this test's temporary directory.
QString assemblyReportAdapter(Fixture& f, bool nonArray) {
    auto encoded = [](const QString& value) {
        const auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        return json.mid(1, json.size() - 2);
    };
    auto script = f.temp.filePath("assembly-report.py");
    write(script,
          "import json, runpy, sys\nfrom pathlib import Path\nfixture = runpy.run_path(" +
              encoded(assemblyMock()) +
              ")\ncode = fixture['main']()\nif code: sys.exit(code)\n"
              "path = Path(sys.argv[sys.argv.index('--report') + 1])\n"
              "report = json.loads(path.read_text())\n"
              "parts = report.pop('parts')\nreport['nested'] = {'parts': parts}\n" +
              (nonArray ? QByteArray("report['parts'] = {'lid': True}\n") : QByteArray{}) +
              "path.write_text(json.dumps(report))\n");
    return script;
}
// Use the real controller and subscribe before starting: an idle process alone is not success.
PreviewController* previewController(MainWindow& w) {
    auto controllers = w.findChildren<PreviewController*>();
    check(controllers.size() == 1, "window owns one preview controller");
    return controllers.first();
}
QString successfulPreview(MainWindow& w, Fixture& f, const PreviewOptions& options = {}) {
    auto* controller = previewController(w);
    QObject receiver;
    bool finished = false, success = false;
    QObject::connect(controller, &PreviewController::finished, &receiver, [&](bool ok) {
        finished = true;
        success = ok;
    });
    check(w.previewMesh(PYTHON_EXECUTABLE, {f.previewScript()}, options), "digest preview starts");
    check(waitFor([&] { return finished; }) && success, "digest preview finished(true)");
    check(controller->mesh() != nullptr && controller->digest().has_value(),
          "successful preview has mesh and digest");
    auto hash = QString::fromStdString(controller->digest()->mesh_sha256);
    check(QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch(),
          "preview digest is 64 lowercase hexadecimal characters");
    return hash;
}
QString differentHash(QString hash) {
    hash[0] = hash[0] == 'a' ? 'b' : 'a';
    return hash;
}
QString digestResult(Fixture& f, const QString& hash) {
    auto dir = f.result("mock");
    auto path = dir + "/manifest.json";
    auto manifest = QJsonDocument::fromJson(read(path)).object();
    manifest.remove("mesh_sha256");
    auto mesh = manifest["mesh"].toObject();
    if (hash.isNull())
        mesh.remove("mesh_sha256");
    else
        mesh["mesh_sha256"] = hash;
    manifest["mesh"] = mesh;
    write(path, QJsonDocument(manifest).toJson());
    ResultSession independent;
    check(independent.open(path), "copied digest manifest opens independently");
    check(independent.meshDigest()["mesh_sha256"].toString() == hash,
          "effective manifest digest equals nested fixture value");
    return path;
}
void checkComparison(MainWindow& w,
                     const QString& path,
                     const QString& hash,
                     MeshSha256Compare expected) {
    ResultSession independent;
    check(independent.open(path), "comparison oracle opens independently");
    QString detail;
    check(independent.comparePreviewMeshSha256(hash, &detail) == expected,
          "fixture exercises the intended comparator branch");
    check(w.lastMeshDigestCompare() == expected, "window uses ResultSession comparison enum");
    check(w.lastMeshDigestDetail().toUtf8() == detail.toUtf8(),
          "window retains comparator detail byte for byte");
}
struct ResultSnapshot {
    ResultSession* session;
    ResultView* view;
    QWidget* central;
    QString badge, banner, watermark;
    explicit ResultSnapshot(MainWindow& w)
        : session(w.resultSession()), view(w.resultView()), central(w.centralWidget()),
          badge(w.sourceBadgeText()), banner(w.sourceBannerText()),
          watermark(w.provenanceWatermark()) {}
    void preserved(MainWindow& w) const {
        check(w.resultSession() == session && w.resultView() == view &&
                  w.centralWidget() == central,
              "rejected open preserves session, view and central widget");
        check(w.sourceBadgeText() == badge && w.sourceBannerText() == banner &&
                  w.provenanceWatermark() == watermark,
              "rejected open preserves source text");
    }
    void replaced(MainWindow& w) const {
        check(w.resultSession() && w.resultView() && w.resultSession() != session &&
                  w.resultView() != view && w.centralWidget() == w.resultView(),
              "accepted open installs a new session and central result view");
    }
};
QAction* writeAction(MainWindow& w) {
    QAction* found = nullptr;
    for (auto* action : w.findChildren<QAction*>()) {
        if (action->text() == QStringLiteral("写入 case") && action->isVisible()) {
            check(!found, "one visible exact-text write action");
            found = action;
        }
    }
    check(found != nullptr, "visible exact-text 写入 case action exists");
    return found;
}
PreviewOptions specifiedOptions() {
    PreviewOptions options;
    options.nx = 8;
    options.ny = 9;
    options.levels = 3;
    options.maxRatio = 12;
    return options;
}
void checkPreviewDisplay(MainWindow& w) {
    PreviewView reference;
    check(reference.show(*previewController(w)), "reference PreviewView shows current controller");
    auto* view = w.previewView();
    check(view != nullptr, "successful preview must install a central PreviewView");
    check(w.centralWidget() == view, "PreviewView is the current central widget");
    check(view->displayed() != nullptr, "central preview displays mesh data");
    check(view->colorMode() == PreviewColorMode::Layer, "central preview defaults to Layer colors");
    check(!view->summaryText().isEmpty(), "central preview has a mesh summary");
    check(view->summaryText().toUtf8() == reference.summaryText().toUtf8(),
          "central summary matches reference PreviewView byte for byte");
    check(!w.previewDisplayStale(), "successful display resets stale flag");
    check(w.writePreviewToCaseEnabled() && writeAction(w)->isEnabled(),
          "successful display retains enabled write API and action");
}
void checkPreviewArguments(Fixture& f, const PreviewOptions& expected) {
    const auto args = QJsonDocument::fromJson(read(f.previewArguments())).object();
    check(args.contains("nx") && args.contains("ny") && args.contains("levels") &&
              args.contains("max_ratio") && args.contains("surface"),
          "preview CLI records every requested option field");
    if (expected.nx) {
        check(args["nx"].toInt(-1) == *expected.nx && args["ny"].toInt(-1) == *expected.ny,
              "MainWindow forwards requested nx and ny");
    } else {
        check(args["nx"].isNull() && args["ny"].isNull(), "MainWindow preserves absent nx and ny");
    }
    check(args["levels"].toInt(-1) == expected.levels, "MainWindow forwards requested levels");
    // Exact equality is the brief's forwarding contract (10 and 12 are exactly representable).
    check(args["max_ratio"].toDouble(-1) == expected.maxRatio,
          "MainWindow forwards requested max_ratio exactly");
    if (expected.surface.isEmpty())
        check(args["surface"].isNull(), "MainWindow preserves empty surface option");
    else
        check(args["surface"].toString() == expected.surface,
              "MainWindow forwards the complete surface path including spaces");
}
struct PreviewSnapshot {
    QPointer<PreviewView> view;
    QByteArray summary;
    vtkDataSet* displayed;
    explicit PreviewSnapshot(MainWindow& w)
        : view(w.previewView()), summary(view ? view->summaryText().toUtf8() : QByteArray()),
          displayed(view ? view->displayed() : nullptr) {
        check(view && displayed, "establish a displayed preview before preservation check");
    }
    void preserved(MainWindow& w) const {
        check(view && w.previewView() == view && w.centralWidget() == view,
              "pending or unsuccessful preview preserves the last central preview object");
        check(view->summaryText().toUtf8() == summary && view->displayed() == displayed,
              "pending or unsuccessful preview preserves displayed data and summary");
    }
};
void editThickness(MainWindow& w) {
    auto* editor = w.findChild<StackEditor*>();
    check(editor != nullptr, "window owns StackEditor");
    QObject receiver;
    bool invalidated = false;
    QObject::connect(
        editor, &StackEditor::previewInvalidated, &receiver, [&] { invalidated = true; });
    auto* model = editor->model();
    auto index = model->index(0, LayerModel::Thickness);
    check(index.isValid(), "fixture has an editable layer");
    const double thickness = model->data(index, Qt::EditRole).toDouble();
    check(thickness > 0 && model->setData(index, thickness * 1.1, Qt::EditRole),
          "legal layer thickness edit succeeds");
    check(waitFor([&] { return invalidated; }), "layer edit emits previewInvalidated");
}
void checkMeshOptions(const MeshOptions& actual, const MeshOptions& cached) {
    check(actual.nx == cached.nx && actual.ny == cached.ny && actual.levels == cached.levels &&
              actual.cellBudget == cached.cellBudget && actual.maxRatio == cached.maxRatio,
          "write restores all five cached MeshOptions fields exactly");
}
void editMeshOptions(MainWindow& w, const MeshOptions& cached) {
    auto edited = cached;
    edited.nx.reset();
    edited.ny.reset();
    edited.levels = 0;
    edited.cellBudget += 100;
    // The brief requires exact equality; fixture ratios 10 and 12 are exactly representable.
    edited.maxRatio = cached.maxRatio == 12 ? 10 : 12;
    w.document()->setMeshOptions(edited);
}
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
void checkCanRestart(MainWindow& w, Fixture& f) {
    auto controllers = w.findChildren<PreviewController*>();
    check(controllers.size() == 1, "window owns preview controller");
    bool finished = false, success = false;
    QObject receiver;
    QObject::connect(controllers.first(), &PreviewController::finished, &receiver, [&](bool ok) {
        finished = true;
        success = ok;
    });
    check(w.previewMesh(PYTHON_EXECUTABLE, {f.previewScript()}),
          "terminal job permits another preview");
    check(waitFor([&] { return finished; }) && success && controllers.first()->mesh(),
          "preview after terminal job produces mesh");
    check(w.submitCurrentCase(), "terminal job permits another submit");
    check(runDirectories(f.root()).size() == 2, "second submit creates a new service job");
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
    using Choice = MainWindow::MeshDigestDialogChoice;
    for (const auto* overrideName : {"--schema-dir", "CHIPCAE_SCHEMA_DIR"}) {
        test((QString("startup reports invalid schema override: ") + overrideName)
                 .toUtf8()
                 .constData(),
             [&] {
                 QTemporaryDir temp;
                 check(temp.isValid(), "startup schema temporary directory");
                 QProcess process;
                 auto env = QProcessEnvironment::systemEnvironment();
                 for (const auto& key : env.keys()) {
                     if (key.startsWith("CHIPCAE_"))
                         env.remove(key);
                 }
                 env.insert("QT_QPA_PLATFORM", "offscreen");
                 QStringList args;
                 const auto missing = temp.filePath("missing-schema");
                 if (QString(overrideName) == "--schema-dir")
                     args << overrideName << missing;
                 else
                     env.insert(overrideName, missing);
                 process.setProcessEnvironment(env);
                 process.setWorkingDirectory(temp.path());
                 process.start(
                     QDir(QCoreApplication::applicationDirPath()).filePath("chipcae-desktop"),
                     args);
                 check(process.waitForStarted(), "desktop subprocess starts");
                 check(process.waitForFinished(10000), "invalid schema startup exits promptly");
                 check(process.exitStatus() == QProcess::NormalExit,
                       "invalid schema startup must exit normally instead of aborting");
                 check(process.exitCode() == 1, "invalid schema startup returns failure status");
                 const auto error = process.readAllStandardError();
                 check(error.contains(overrideName) && error.contains("case-1.0.schema.json"),
                       "startup stderr identifies the invalid override and required schema marker");
             });
    }
    test("schema default constructor uses process locator inputs", [&] {
        const auto source = QString(CHIPCAE_REPOSITORY) + "/schema";
        const auto args = QCoreApplication::arguments();
        const auto env = QProcessEnvironment::systemEnvironment();
        const auto executableDir = QCoreApplication::applicationDirPath();
        MainWindow w;
        check(w.schemaDirectory() ==
                  DesktopConfig::locateSchemaDir(args, env, executableDir, source),
              "default window forwards process args, environment, executable and source fallback");
        check(QFileInfo(w.schemaDirectory() + "/case-1.0.schema.json").isFile(),
              "default window schema directory contains marker file");
        if (DesktopConfig::locateSchemaDir(args, env, executableDir, {}).isEmpty())
            check(w.schemaDirectory() == QFileInfo(source).canonicalFilePath(),
                  "default window uses canonical source directory when implicit candidates miss");
    });
    test("schema constructor preserves QWidget parent compatibility", [&] {
        QWidget parent;
        MainWindow w(&parent);
        check(w.parentWidget() == &parent, "existing parent constructor preserves ownership");
    });
    test("schema CLI override selects temporary directory", [&] {
        QTemporaryDir temp;
        const auto directory = schemaFixture(temp, "cli/schema");
        const QStringList args{"app", "--schema-dir", directory};
        const QProcessEnvironment env;
        MainWindow w(args, env);
        checkSchemaHit(w, directory, args, env);
        check(w.schemaDirectory() !=
                  QFileInfo(QString(CHIPCAE_REPOSITORY) + "/schema").canonicalFilePath(),
              "CLI override differs from source schema directory");
    });
    test("schema environment override selects temporary directory", [&] {
        QTemporaryDir temp;
        const auto directory = schemaFixture(temp, "environment/schema");
        const QStringList args{"app"};
        QProcessEnvironment env;
        env.insert("CHIPCAE_SCHEMA_DIR", directory);
        MainWindow w(args, env);
        checkSchemaHit(w, directory, args, env);
    });
    for (const bool validEnvironment : {true, false}) {
        test(validEnvironment ? "schema CLI takes priority over valid environment"
                              : "schema CLI takes priority over invalid environment",
             [&] {
                 QTemporaryDir temp;
                 const auto cli = schemaFixture(temp, "cli/schema");
                 const auto other = schemaFixture(temp, "other/schema");
                 const QStringList args{"app", "--schema-dir", cli};
                 QProcessEnvironment env;
                 env.insert("CHIPCAE_SCHEMA_DIR",
                            validEnvironment ? other : temp.filePath("missing"));
                 MainWindow w(args, env);
                 checkSchemaHit(w, cli, args, env);
             });
    }
    // Isolated layouts catch a missing executableDir argument or a source-only fallback.
    for (const auto& layout :
         {QStringList{
              "macOS Resources", "App.app/Contents/MacOS", "App.app/Contents/Resources/schema"},
          QStringList{"installed prefix", "prefix/bin", "prefix/share/chipcae/schema"},
          QStringList{"adjacent schema", "bin", "bin/schema"}}) {
        test(("schema implicit layout: " + layout[0]).toUtf8().constData(), [&] {
            QTemporaryDir temp;
            const auto directory = schemaFixture(temp, layout[2]);
            const auto executableDir = temp.filePath(layout[1]);
            check(QDir().mkpath(executableDir), "fixture executable directory");
            const QStringList args{"app"};
            const QProcessEnvironment env;
            MainWindow w(args, env, executableDir);
            checkSchemaHit(w, directory, args, env, executableDir);
        });
    }
    test("schema compile time fallback uses supplied directory", [&] {
        QTemporaryDir temp;
        const auto directory = schemaFixture(temp, "fallback/schema");
        const QStringList args{"app"};
        const QProcessEnvironment env;
        MainWindow w(args, env, {}, directory);
        checkSchemaHit(w, directory, args, env, {}, directory);
    });
    test("schema all empty locator inputs fall back to source directory", [&] {
        const QStringList args;
        const QProcessEnvironment env;
        check(DesktopConfig::locateSchemaDir(args, env, {}, {}).isEmpty(),
              "all empty locator inputs return no candidate");
        MainWindow w(args, env);
        const auto source = QString(CHIPCAE_REPOSITORY) + "/schema";
        check(w.schemaDirectory() == source,
              "empty locator result uses source schema concatenation");
        check(QFileInfo(w.schemaDirectory()).canonicalFilePath() ==
                  QFileInfo(source).canonicalFilePath(),
              "source fallback points at source schema directory");
    });
    for (const auto* invalid :
         {"missing argument", "empty argument", "nonexistent", "no marker", "duplicate"}) {
        test((QString("schema rejects CLI override: ") + invalid).toUtf8().constData(), [&] {
            QTemporaryDir temp;
            const auto fallback = schemaFixture(temp, "valid-fallback/schema");
            const auto unmarked = temp.filePath("unmarked");
            check(QDir().mkpath(unmarked), "unmarked fixture directory");
            QStringList args{"app", "--schema-dir"};
            const QString mode = invalid;
            if (mode == "empty argument")
                args << "";
            else if (mode == "nonexistent")
                args << temp.filePath("missing");
            else if (mode == "no marker")
                args << unmarked;
            else if (mode == "duplicate")
                args << fallback << "--schema-dir" << fallback;
            checkSchemaError(args, QProcessEnvironment{}, fallback, "--schema-dir");
        });
    }
    for (const auto* invalid : {"empty", "nonexistent", "no marker"}) {
        test((QString("schema rejects environment override: ") + invalid).toUtf8().constData(),
             [&] {
                 QTemporaryDir temp;
                 const auto fallback = schemaFixture(temp, "valid-fallback/schema");
                 const auto unmarked = temp.filePath("unmarked");
                 check(QDir().mkpath(unmarked), "unmarked fixture directory");
                 const QString mode = invalid;
                 QProcessEnvironment env;
                 env.insert("CHIPCAE_SCHEMA_DIR",
                            mode == "empty"         ? QString()
                            : mode == "nonexistent" ? temp.filePath("missing")
                                                    : unmarked);
                 checkSchemaError({"app"}, env, fallback, "CHIPCAE_SCHEMA_DIR");
             });
    }
    test("schema directory is stable across construction and cached after marker removal", [&] {
        QTemporaryDir temp;
        const auto directory = schemaFixture(temp, "cached/schema");
        const QStringList args{"app", "--schema-dir", directory};
        const QProcessEnvironment env;
        MainWindow first(args, env), second(args, env);
        checkSchemaHit(first, directory, args, env);
        checkSchemaHit(second, directory, args, env);
        const auto cached = first.schemaDirectory();
        check(first.schemaDirectory() == second.schemaDirectory(),
              "same inputs locate same schema");
        check(QFile::remove(directory + "/case-1.0.schema.json"), "remove cached schema marker");
        check(first.schemaDirectory() == cached && second.schemaDirectory() == cached,
              "accessors retain constructed directory without locating again");
    });
#ifdef Q_OS_LINUX
    test("schema lowercase environment key is ignored on Linux", [&] {
        QTemporaryDir temp;
        const auto fallback = schemaFixture(temp, "fallback/schema");
        const auto other = schemaFixture(temp, "other/schema");
        QProcessEnvironment env;
        env.insert("chipcae_schema_dir", other);
        const QStringList args{"app"};
        MainWindow w(args, env, {}, fallback);
        checkSchemaHit(w, fallback, args, env, {}, fallback);
    });
    test("schema marker filename is case sensitive on Linux", [&] {
        QTemporaryDir temp;
        const auto fallback = schemaFixture(temp, "valid-fallback/schema");
        const auto wrongCase = temp.filePath("wrong-case");
        check(QDir().mkpath(wrongCase), "wrong-case fixture directory");
        write(wrongCase + "/Case-1.0.schema.json", "{}");
        checkSchemaError(
            {"app", "--schema-dir", wrongCase}, QProcessEnvironment{}, fallback, "--schema-dir");
    });
#endif
    test("schema selected directory drives case tree form validation and reload", [&] {
        QTemporaryDir temp;
        const auto directory = schemaFixture(temp, "wired/schema", R"({"type":"string"})");
        MainWindow w({"app", "--schema-dir", directory}, QProcessEnvironment{});
        check(w.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
              "open case for schema wiring");
        auto* tree = w.findChild<CaseTreeView*>();
        auto* form = w.findChild<SchemaForm*>();
        auto* model = w.findChild<CaseTreeModel*>();
        check(tree && form && model, "window owns case tree model, view and schema form");
        // The case has section nodes rather than a single case root; each binds the whole JSON.
        const auto section = model->locate("/loads", "case");
        check(section.isValid(), "case loads node exists");
        tree->setCurrentIndex(section);
        check(form->document() == w.document()->toJson(),
              "case selection binds full case document");
        check(!form->errors().empty(), "temporary string schema rejects object case document");
        write(directory + "/case-1.0.schema.json", "{}");
        tree->cancelEdit();
        check(form->document() == w.document()->toJson(), "cancel reload preserves case document");
        check(form->errors().empty(),
              "cancel reloads updated permissive schema from selected directory");
    });
    test("File menu exposes distinct assembly import", [&] {
        MainWindow w;
        QMenu* file = nullptr;
        for (auto* action : w.menuBar()->actions())
            if (action->text() == "File")
                file = action->menu();
        check(file != nullptr, "File menu exists");
        QAction* assembly = nullptr;
        QAction* ecad = nullptr;
        for (auto* action : file->actions()) {
            if (action->text() == QStringLiteral("Import assembly…"))
                assembly = action;
            if (action->text() == QStringLiteral("Import ECAD…"))
                ecad = action;
        }
        check(assembly && ecad && assembly != ecad, "File has distinct Import assembly… action");
    });
    test("assembly import annotates Bodies and refreshes stack editor", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        importAssembly(w, assemblyInput());
        checkAssembly(w, QString("近似材料"));
    });
    test("assembly reimport clears obsolete approximate material annotation", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        importAssembly(w, assemblyInput());
        checkAssembly(w, QString("近似材料"));
        auto exact = QJsonDocument::fromJson(read(assemblyInput())).object();
        auto parts = exact["parts"].toArray();
        auto lid = parts[0].toObject();
        lid["material_approx"] = false;
        parts[0] = lid;
        exact["parts"] = parts;
        const auto path = f.temp.filePath("exact.assembly.json");
        write(path, QJsonDocument(exact).toJson());
        importAssembly(w, path);
        checkAssembly(w, QString(""));
    });
    test("rejected assembly preserves stack and existing part summary", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        importAssembly(w, assemblyInput());
        checkAssembly(w, QString("近似材料"));
        const auto before = w.document()->stackJson();
        auto duplicate = QJsonDocument::fromJson(read(assemblyInput())).object();
        auto parts = duplicate["parts"].toArray();
        parts.append(parts[0]);
        duplicate["parts"] = parts;
        const auto path = f.temp.filePath("duplicate.assembly.json");
        write(path, QJsonDocument(duplicate).toJson());
        check(isAssemblyPath(path), "duplicate-ID fixture passes assembly schema detection");
        importAssembly(w, path, false);
        check(w.document()->stackJson() == before, "rejected assembly leaves stack unchanged");
        checkAssembly(w, QString("近似材料"));
    });
    for (bool nonArray : {false, true}) {
        test(nonArray ? "assembly report with non-array parts clears annotations"
                      : "assembly report without parts clears annotations",
             [&] {
                 Fixture f(argv[1]);
                 f.configure();
                 MainWindow w;
                 f.openCase(w);
                 importAssembly(w, assemblyInput());
                 checkAssembly(w, QString("近似材料"));
                 importAssembly(w, assemblyInput(), true, assemblyReportAdapter(f, nonArray));
                 checkAssembly(w, QString(""));
             });
    }
    test("assembly to convection to preview to submit to completed results", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        importAssembly(w, assemblyInput());
        checkAssembly(w, QString("近似材料"));
        // Convection is a thermal load; use the same mode switch as the Case menu.
        assemblyTree(w)->setThermalMode(true);
        check(!assemblyTree(w)
                   ->addLoad(Json::parse(
                       R"({"type":"convection","target":"face:zmax","h":2,"T_inf":298.15})"))
                   .isEmpty(),
              "add menu-equivalent convection load");
        const auto document = w.document()->toJson();
        check(document.contains("loads") && document.at("loads").is_array(), "case has loads");
        bool convection = false;
        for (const auto& load : document.at("loads").array_range())
            if (load.at("type").as<std::string>() == "convection" &&
                load.at("target").as<std::string>() == "face:zmax")
                convection = true;
        check(convection, "case contains exact convection type and face:zmax target strings");
        auto stack = w.document()->stackJson();
        stack["mock"] = Json::object();
        stack["mock"]["fixture"] = (f.samples + "/legacy.vtkhdf").toStdString();
        w.document()->applyStackJson(stack);
        chipcae::desktop::PreviewOptions options{};
        QObject receiver;
        bool finished = false, success = false;
        QObject::connect(
            previewController(w), &PreviewController::finished, &receiver, [&](bool ok) {
                finished = true;
                success = ok;
            });
        check(w.previewMesh(PYTHON_EXECUTABLE, {PREVIEW_MOCK}, options),
              "imported assembly preview starts");
        check(waitFor([&] { return finished; }) && success, "assembly preview finished(true)");
        check(w.previewView() && w.previewView()->displayed(), "assembly preview displays mesh");
        // The fixture key is only for the preview CLI; finalize validates the stack schema.
        stack.erase("mock");
        w.document()->applyStackJson(stack);
        check(w.writePreviewToCase(), "write assembly preview into case");
        w.setMeshDigestDialogChoice(Choice::Accept);
        check(w.submitCurrentCase(), "assembly case submits");
        f.release();
        check(waitFor([&] { return persistedJob(f.root()).state() == jobs::JOB_STATE_COMPLETED; }),
              "assembly job persists COMPLETED state");
        check(waitFor([&] { return w.resultView() != nullptr; }),
              "assembly completion automatically opens results");
        check(w.centralWidget() == w.resultView() && w.resultView()->error().isEmpty(),
              "assembly workflow ends with central result view without errors");
        w.resultView()->setWarpageScale(10.0);
        check(w.centralWidget() == w.resultView() && w.resultView()->error().isEmpty(),
              "completed assembly results support warpage scaling");
    });
    test("preview display defaults and no case rejection", [&] {
        MainWindow w;
        auto* central = w.centralWidget();
        check(!w.caseIsOpen() && !w.previewMesh(PYTHON_EXECUTABLE, {PREVIEW_MOCK}),
              "preview without an open case is rejected");
        check(!w.previewView() && !w.previewDisplayStale() && w.centralWidget() == central,
              "fresh window keeps placeholder with no preview and no stale display");
    });
    for (bool handwritten : {false, true}) {
        for (bool specified : {false, true}) {
            auto name = QString("preview options reach CLI: %1 %2")
                            .arg(handwritten ? "handwritten stack" : "schema case",
                                 specified ? "specified" : "defaults");
            test(name.toUtf8().constData(), [&] {
                Fixture f(argv[1]);
                MainWindow w;
                if (handwritten) {
                    check(w.loadHandwrittenStack(), "load direct stack preview input");
                    // The direct-stack branch still needs a mesh options object for the
                    // controller's existing effective-options update; keep schema absent.
                    w.document()->applyJson(Json::parse(R"({"mesh":{}})"));
                } else
                    f.openCase(w);
                check(w.document()->toJson().contains("schema") == !handwritten,
                      "fixture selects the intended schema-bundle or direct-stack branch");
                auto options = specified ? specifiedOptions() : PreviewOptions{};
                if (specified)
                    options.surface = f.temp.filePath("surface mesh with spaces.vtkhdf");
                successfulPreview(w, f, options);
                checkPreviewArguments(f, options);
                checkPreviewDisplay(w);
                if (specified)
                    check(w.previewView()->summaryText().section('\n', 0, 0) ==
                              "nx=8 ny=9 levels=3 max_ratio=12",
                          "specified preview summary has the frozen exact first line");
            });
        }
    }
    test("pending first preview keeps placeholder until successful display", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        auto* central = w.centralWidget();
        auto* controller = previewController(w);
        QObject receiver;
        bool finished = false, success = false;
        QObject::connect(controller, &PreviewController::finished, &receiver, [&](bool ok) {
            finished = true;
            success = ok;
        });
        auto gate = f.temp.filePath("preview-release");
        check(w.previewMesh(PYTHON_EXECUTABLE, {f.previewScript(gate)}), "gated preview starts");
        check(waitFor([&] { return QFile::exists(gate + ".ready"); }), "preview reaches gate");
        check(!finished && controller->running(), "gate holds preview before completion");
        check(!w.previewView() && w.centralWidget() == central && !w.previewDisplayStale(),
              "pending first preview leaves placeholder and stale flag unchanged");
        write(gate, "go");
        check(waitFor([&] { return finished; }) && success, "released preview finished(true)");
        checkPreviewDisplay(w);
    });
    test("repeated preview displays the latest mesh summary", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        successfulPreview(w, f);
        checkPreviewDisplay(w);
        auto first = w.previewView()->summaryText().toUtf8();
        successfulPreview(w, f, specifiedOptions());
        checkPreviewDisplay(w);
        check(w.previewView()->summaryText().toUtf8() != first,
              "second display replaces the first preview summary");
        check(w.previewView()->summaryText().section('\n', 0, 0) ==
                  "nx=8 ny=9 levels=3 max_ratio=12",
              "second display reflects latest explicit options");
    });
    for (bool priorPreview : {false, true}) {
        for (auto mode : {"failure", "cancel", "start failure"}) {
            auto name = QString("unsuccessful preview preserves display: %1 %2")
                            .arg(priorPreview ? "after success" : "first attempt", mode);
            test(name.toUtf8().constData(), [&] {
                Fixture f(argv[1]);
                MainWindow w;
                f.openCase(w);
                std::optional<PreviewSnapshot> previous;
                if (priorPreview) {
                    successfulPreview(w, f);
                    checkPreviewDisplay(w);
                    previous.emplace(w);
                }
                auto* central = w.centralWidget();
                auto preserved = [&] {
                    if (previous)
                        previous->preserved(w);
                    else
                        check(!w.previewView() && w.centralWidget() == central,
                              "unsuccessful first preview keeps the original central widget");
                };
                auto* controller = previewController(w);
                QObject receiver;
                bool finished = false, success = true;
                QObject::connect(controller, &PreviewController::finished, &receiver, [&](bool ok) {
                    finished = true;
                    success = ok;
                });
                const bool missingProgram = QString(mode) == "start failure";
                auto gate = f.temp.filePath("failed-preview-release");
                auto script = f.previewScript(gate, true);
                bool started = w.previewMesh(missingProgram ? f.temp.filePath("missing-program")
                                                            : QString(PYTHON_EXECUTABLE),
                                             {script});
                check(started || missingProgram, "unsuccessful fixture starts or fails to start");
                preserved();
                if (!missingProgram) {
                    check(waitFor([&] { return QFile::exists(gate + ".ready"); }),
                          "unsuccessful preview reaches gate");
                    check(!finished && controller->running(), "gate holds unsuccessful preview");
                    preserved();
                    if (QString(mode) == "cancel")
                        controller->cancel();
                    else
                        write(gate, "go");
                }
                if (started)
                    check(waitFor([&] { return finished; }) && !success,
                          "unsuccessful preview emits finished(false)");
                check(!controller->running(), "unsuccessful preview is no longer running");
                preserved();
                check(!w.previewDisplayStale(),
                      "failure alone does not invalidate displayed stack");
            });
        }
    }
    test("preview results preview releases replaced views and result controls", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        successfulPreview(w, f);
        checkPreviewDisplay(w);
        QPointer<PreviewView> oldPreview = w.previewView();
        w.setMeshDigestDialogChoice(Choice::Accept);
        check(w.openResultsPath(f.result("mock") + "/manifest.json"), "open results after preview");
        check(w.resultView() && w.centralWidget() == w.resultView() && !w.previewView(),
              "results replace central preview and clear preview getter");
        check(waitFor([&] { return oldPreview.isNull(); }), "replaced PreviewView is released");
        QPointer<ResultView> oldResult = w.resultView();
        QPointer<QWidget> controls;
        QPointer<QDockWidget> dock;
        for (auto* widget : w.findChildren<QWidget*>()) {
            if (dynamic_cast<ResultControls*>(widget)) {
                check(!controls, "one result controls widget before replacement");
                controls = widget;
                dock = qobject_cast<QDockWidget*>(widget->parentWidget());
            }
        }
        QPointer<QLabel> watermark;
        for (auto* label : oldResult->findChildren<QLabel*>()) {
            if (label->text() == w.provenanceWatermark())
                watermark = label;
        }
        check(controls && dock && watermark, "result controls dock and watermark are present");
        successfulPreview(w, f, specifiedOptions());
        checkPreviewDisplay(w);
        check(w.resultView() == nullptr, "preview replacement clears resultView getter");
        check(waitFor([&] { return !oldResult && !controls && !dock && !watermark; }),
              "preview replacement releases old result, controls, dock and watermark");
    });
    test("stack invalidation tracks the most recent successful preview display", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        editThickness(w);
        check(!w.previewDisplayStale(), "editing before the first display does not mark it stale");
        successfulPreview(w, f);
        checkPreviewDisplay(w);
        editThickness(w);
        check(waitFor([&] { return w.previewDisplayStale(); }),
              "stack invalidation after a successful display marks the preview stale");
        successfulPreview(w, f);
        checkPreviewDisplay(w);
        check(!w.previewDisplayStale(), "new successful display clears stack staleness");
    });
    test("specified preview to write case preserves effective mesh options", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        successfulPreview(w, f, specifiedOptions());
        checkPreviewArguments(f, specifiedOptions());
        checkPreviewDisplay(w);
        const auto effective = previewController(w)->effectiveOptions();
        const auto cellBudget = w.document()->meshOptions().cellBudget;
        check(w.writePreviewToCase(), "displayed preview options write to case and finalize");
        const auto actual = w.document()->meshOptions();
        check(actual.nx == effective.nx && actual.ny == effective.ny &&
                  actual.levels == effective.levels && actual.maxRatio == effective.maxRatio,
              "written nx, ny, levels and maxRatio equal preview effective options exactly");
        check(actual.cellBudget == cellBudget && w.document()->errors().empty(),
              "write retains cellBudget and finalizes without validation errors");
    });
    test("digest defaults and unavailable write leave document unchanged", [&] {
        MainWindow w;
        check(w.meshDigestDialogChoice() == Choice::Unset, "digest hook initially Unset");
        check(w.lastMeshDigestCompare() == MeshSha256Compare::PreviewMissing &&
                  w.lastMeshDigestDetail().isEmpty() && !w.lastOpenCancelledForMeshDigest(),
              "fresh window has no comparison or cancellation");
        auto before = w.document()->toJson();
        check(!w.writePreviewToCaseEnabled() && !w.writePreviewToCase(),
              "write unavailable before a successful preview");
        check(w.document()->toJson() == before, "unavailable write has no document mutation");
        check(!writeAction(w)->isEnabled(), "unavailable write action disabled");
    });
    test("digest mismatch Cancel preserves existing results repeatedly", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        check(w.openResultsPath(f.result("real") + "/manifest.json"), "open original result");
        auto hash = successfulPreview(w, f);
        ResultSnapshot before(w);
        auto other = differentHash(hash);
        auto path = digestResult(f, other);
        w.setMeshDigestDialogChoice(Choice::Cancel);
        for (int attempt = 0; attempt < 2; ++attempt) {
            check(!w.openResultsPath(path), "mismatched digest Cancel rejects result open");
            before.preserved(w);
            checkComparison(w, path, hash, MeshSha256Compare::Mismatch);
            auto detail = w.lastMeshDigestDetail();
            check(detail.contains(hash) && detail.contains(other),
                  "mismatch detail names both hashes");
            check(w.lastOpenCancelledForMeshDigest(), "digest cancellation is reported");
            check(w.meshDigestDialogChoice() == Choice::Cancel, "Cancel hook persists after use");
            check(w.property("meshDigestWarningText").toString().contains(detail),
                  "hook warning records complete mismatch detail");
        }
    });
    test("digest mismatch Accept after Cancel replaces existing results", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        check(w.openResultsPath(f.result("real") + "/manifest.json"), "open original result");
        auto hash = successfulPreview(w, f);
        auto path = digestResult(f, differentHash(hash));
        ResultSnapshot before(w);
        w.setMeshDigestDialogChoice(Choice::Cancel);
        check(!w.openResultsPath(path), "first mismatch is cancelled");
        before.preserved(w);
        w.setMeshDigestDialogChoice(Choice::Accept);
        check(w.openResultsPath(path), "Accept opens the same mismatched manifest");
        before.replaced(w);
        checkComparison(w, path, hash, MeshSha256Compare::Mismatch);
        check(!w.lastOpenCancelledForMeshDigest(), "accepted open clears cancellation");
        check(w.meshDigestDialogChoice() == Choice::Accept, "Accept hook persists after use");
        check(w.property("meshDigestWarningText").toString().contains(w.lastMeshDigestDetail()),
              "accepted hook warning records complete comparator detail");
    });
    test("matching digest opens twice with Cancel hook", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        auto hash = successfulPreview(w, f);
        auto path = digestResult(f, hash);
        w.setMeshDigestDialogChoice(Choice::Cancel);
        // A matching result must never warn, even if the hook would cancel a warning.
        for (int attempt = 0; attempt < 2; ++attempt) {
            ResultSnapshot before(w);
            check(w.openResultsPath(path), "matching digest opens despite Cancel hook");
            before.replaced(w);
            checkComparison(w, path, hash, MeshSha256Compare::Match);
            check(w.lastMeshDigestDetail().isEmpty() && !w.lastOpenCancelledForMeshDigest(),
                  "matching digest has empty detail and no cancellation");
            check(w.meshDigestDialogChoice() == Choice::Cancel, "matching open keeps hook");
            check(w.property("meshDigestWarningText").toString().isEmpty(),
                  "matching digest does not record a warning");
        }
    });
    for (const auto& manifestHash : {QString(), QString("invalid"), QString(64, 'A')}) {
        for (auto choice : {Choice::Accept, Choice::Cancel}) {
            auto name = QString("manifest digest %1 %2")
                            .arg(manifestHash.isNull() ? "missing" : manifestHash,
                                 choice == Choice::Accept ? "Accept" : "Cancel");
            test(name.toUtf8().constData(), [&] {
                Fixture f(argv[1]);
                MainWindow w;
                f.openCase(w);
                check(w.openResultsPath(f.result("real") + "/manifest.json"),
                      "open original result");
                auto hash = successfulPreview(w, f);
                auto path = digestResult(f, manifestHash);
                ResultSnapshot before(w);
                w.setMeshDigestDialogChoice(choice);
                check(w.openResultsPath(path) == (choice == Choice::Accept),
                      "ManifestMissing obeys configured dialog choice");
                if (choice == Choice::Accept)
                    before.replaced(w);
                else
                    before.preserved(w);
                checkComparison(w, path, hash, MeshSha256Compare::ManifestMissing);
                check(w.lastOpenCancelledForMeshDigest() == (choice == Choice::Cancel),
                      "ManifestMissing cancellation follows choice");
                check(w.meshDigestDialogChoice() == choice, "manifest warning keeps hook");
                check(!w.lastMeshDigestDetail().isEmpty() &&
                          w.property("meshDigestWarningText")
                              .toString()
                              .contains(w.lastMeshDigestDetail()),
                      "manifest warning contains full comparator detail");
            });
        }
    }
    for (auto failure : {"missing manifest", "empty steps", "unreadable first step"}) {
        test((QString("failed result open resets digest cancellation: ") + failure)
                 .toUtf8()
                 .constData(),
             [&] {
                 Fixture f(argv[1]);
                 MainWindow w;
                 f.openCase(w);
                 check(w.openResultsPath(f.result("real") + "/manifest.json"),
                       "open original result");
                 auto hash = successfulPreview(w, f);
                 auto path = digestResult(f, differentHash(hash));
                 w.setMeshDigestDialogChoice(Choice::Cancel);
                 check(!w.openResultsPath(path) && w.lastOpenCancelledForMeshDigest(),
                       "establish prior mesh cancellation");
                 ResultSnapshot before(w);
                 auto comparison = w.lastMeshDigestCompare();
                 auto detail = w.lastMeshDigestDetail();
                 auto warning = w.property("meshDigestWarningText");
                 auto bad = f.result("broken") + "/manifest.json";
                 if (QString(failure) == "missing manifest") {
                     check(QFile::remove(bad), "remove copied manifest");
                 } else {
                     auto manifest = QJsonDocument::fromJson(read(bad)).object();
                     if (QString(failure) == "empty steps") {
                         manifest["steps"] = QJsonArray{};
                     } else {
                         auto steps = manifest["steps"].toArray();
                         auto first = steps.first().toObject();
                         first["file"] = "unreadable.vtkhdf";
                         steps[0] = first;
                         manifest["steps"] = steps;
                         write(QFileInfo(bad).dir().filePath("unreadable.vtkhdf"), "not HDF5");
                     }
                     write(bad, QJsonDocument(manifest).toJson());
                 }
                 check(!w.openResultsPath(bad), "broken result open fails before digest warning");
                 before.preserved(w);
                 check(!w.lastOpenCancelledForMeshDigest(), "ordinary failure resets cancellation");
                 check(w.lastMeshDigestCompare() == comparison &&
                           w.lastMeshDigestDetail() == detail,
                       "early failure preserves last completed comparison");
                 check(w.meshDigestDialogChoice() == Choice::Cancel &&
                           w.property("meshDigestWarningText") == warning,
                       "early failure does not consume or replace digest warning");
             });
    }
    test("write preview restores every field repeatedly and through action", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        successfulPreview(w, f);
        auto cached = w.document()->meshOptions();
        check(cached.nx && cached.ny && cached.levels >= 1,
              "preview writes effective mesh options");
        check(w.writePreviewToCaseEnabled(), "successful preview enables cached write");
        auto* action = writeAction(w);
        check(action->isEnabled(), "successful preview enables write action");
        editMeshOptions(w, cached);
        check(w.writePreviewToCase(), "write restores cached options and finalizes");
        checkMeshOptions(w.document()->meshOptions(), cached);
        check(w.document()->meshOptions().levels >= 1 && w.document()->errors().empty(),
              "restored mesh finalizes without document errors");
        check(w.writePreviewToCase(), "cached write is repeatable");
        checkMeshOptions(w.document()->meshOptions(), cached);
        editMeshOptions(w, cached);
        action->trigger();
        checkMeshOptions(w.document()->meshOptions(), cached);
        check(w.document()->errors().empty(), "write action finalizes the restored mesh");
    });
    test("failed finalize retains restored preview options", [&] {
        Fixture f(argv[1]);
        MainWindow w;
        f.openCase(w);
        successfulPreview(w, f);
        auto cached = w.document()->meshOptions();
        editMeshOptions(w, cached);
        w.document()->setValue("/schema", Json("invalid-case-schema"));
        check(!w.writePreviewToCase(), "unrelated validation failure rejects finalize");
        checkMeshOptions(w.document()->meshOptions(), cached);
        check(!w.document()->errors().empty(), "failed finalize exposes document errors");
        check(w.document()->toJson().at("schema").as<std::string>() == "invalid-case-schema",
              "write does not overwrite unrelated invalid document field");
    });
    for (auto mode : {"failure", "cancel", "start failure"}) {
        test((QString("new preview invalidates cached write: ") + mode).toUtf8().constData(), [&] {
            Fixture f(argv[1]);
            MainWindow w;
            f.openCase(w);
            successfulPreview(w, f);
            check(w.writePreviewToCaseEnabled(), "establish successful preview cache");
            auto* action = writeAction(w);
            check(action->isEnabled(), "establish enabled write action");
            auto* controller = previewController(w);
            QObject receiver;
            bool finished = false, success = true;
            QObject::connect(controller, &PreviewController::finished, &receiver, [&](bool ok) {
                finished = true;
                success = ok;
            });
            auto script = f.temp.filePath("preview-next.py");
            auto ready = f.temp.filePath("preview-ready");
            auto literal = QJsonDocument(QJsonArray{ready}).toJson(QJsonDocument::Compact);
            literal = literal.mid(1, literal.size() - 2);
            write(script,
                  "import pathlib, sys, time\npathlib.Path(" + literal + ").write_text('ready')\n" +
                      (QString(mode) == "cancel" ? "time.sleep(60)\n" : "sys.exit(9)\n"));
            const bool missingProgram = QString(mode) == "start failure";
            bool started = w.previewMesh(missingProgram ? f.temp.filePath("missing-program")
                                                        : QString(PYTHON_EXECUTABLE),
                                         {script});
            check(started || missingProgram, "next preview starts or reports missing executable");
            check(!w.writePreviewToCaseEnabled() && !action->isEnabled(),
                  "new preview disables cached write immediately");
            auto before = w.document()->toJson();
            check(!w.writePreviewToCase() && w.document()->toJson() == before,
                  "pending preview cannot write stale cached options");
            if (QString(mode) == "cancel") {
                check(waitFor([&] { return QFile::exists(ready); }),
                      "cancel fixture process ready");
                controller->cancel();
            }
            if (started)
                check(waitFor([&] { return finished; }) && !success,
                      "failed or cancelled preview emits finished(false)");
            check(!controller->running() && !w.writePreviewToCaseEnabled() && !action->isEnabled(),
                  "unsuccessful preview leaves write disabled");
            before = w.document()->toJson();
            check(!w.writePreviewToCase() && w.document()->toJson() == before,
                  "failed preview cannot restore old options");
        });
    }
    for (auto replacement : {"case", "handwritten stack", "import"}) {
        test(
            (QString("document replacement invalidates preview cache: ") + replacement)
                .toUtf8()
                .constData(),
            [&] {
                Fixture f(argv[1]);
                MainWindow w;
                f.openCase(w);
                auto hash = successfulPreview(w, f);
                auto resultPath = digestResult(f, differentHash(hash));
                w.setMeshDigestDialogChoice(Choice::Cancel);
                auto* action = writeAction(w);
                check(w.writePreviewToCaseEnabled() && action->isEnabled(),
                      "completed preview enables write before replacement");
                check(!w.openCase(f.temp.filePath("missing-case.json")) &&
                          !w.loadHandwrittenStack(f.temp.filePath("missing-stack.json")),
                      "failed document replacements are rejected");
                check(w.writePreviewToCaseEnabled() && action->isEnabled(),
                      "failed replacements preserve completed preview cache");
                check(!w.openResultsPath(resultPath) && w.lastOpenCancelledForMeshDigest(),
                      "failed replacements preserve current document digest warning");
                checkComparison(w, resultPath, hash, MeshSha256Compare::Mismatch);
                if (QString(replacement) == "case") {
                    f.openCase(w);
                } else if (QString(replacement) == "handwritten stack") {
                    check(w.loadHandwrittenStack(), "replace handwritten stack");
                } else {
                    ImportRequest request;
                    request.input = f.temp.filePath("fixture.xml");
                    request.output = f.temp.filePath("import.stack.json");
                    request.materialMap = f.temp.filePath("material-map.json");
                    request.polyset = f.temp.filePath("import.polyset");
                    request.report = f.temp.filePath("defects.json");
                    request.program = PYTHON_EXECUTABLE;
                    request.prefix = {QString(CHIPCAE_REPOSITORY) +
                                      "/desktop/tools/mock_import.py"};
                    write(request.input, "success");
                    write(request.materialMap, "{}");
                    QObject receiver;
                    bool finished = false, replaced = false;
                    QObject::connect(
                        w.importController(), &ImportController::finished, &receiver, [&](bool ok) {
                            finished = true;
                            replaced = ok;
                        });
                    check(w.importController()->start(request), "start replacement import");
                    check(waitFor([&] { return finished; }) && replaced,
                          "import successfully replaces document");
                }
                check(!w.writePreviewToCaseEnabled() && !action->isEnabled(),
                      "successful document replacement disables stale preview write");
                auto before = w.document()->toJson();
                check(!w.writePreviewToCase() && w.document()->toJson() == before,
                      "stale preview options cannot mutate replacement document");
                check(w.openResultsPath(resultPath),
                      "replacement results open despite stale digest and Cancel choice");
                check(w.lastMeshDigestCompare() == MeshSha256Compare::PreviewMissing &&
                          w.lastMeshDigestDetail().isEmpty() && !w.lastOpenCancelledForMeshDigest(),
                      "replacement invalidates preview digest without warning or cancellation");
                hash = successfulPreview(w, f);
                check(!w.openResultsPath(resultPath) && w.lastOpenCancelledForMeshDigest(),
                      "new successful preview restores digest warnings for current document");
                checkComparison(w, resultPath, hash, MeshSha256Compare::Mismatch);
            });
    }
    test("busy rejected preview preserves completed write cache", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        successfulPreview(w, f);
        auto cached = w.document()->meshOptions();
        w.setMeshDigestDialogChoice(Choice::Accept);
        check(w.submitCurrentCase(), "submit establishes busy window after successful preview");
        check(!w.previewMesh(PYTHON_EXECUTABLE, {f.previewScript()}), "busy preview rejected");
        f.release();
        check(w.writePreviewToCaseEnabled() && writeAction(w)->isEnabled(),
              "busy rejection leaves completed preview cache available");
        editMeshOptions(w, cached);
        check(w.writePreviewToCase(), "busy rejection keeps cached write usable");
        checkMeshOptions(w.document()->meshOptions(), cached);
    });
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
    test("failed profile persistence prevents submission", [&] {
        Fixture f(argv[1]);
        f.configure();
        check(QDir().mkpath(f.root() + "/submit-profile.json"),
              "block profile file with directory");
        MainWindow w;
        f.openCase(w);
        check(!w.submitCurrentCase(), "profile write failure rejects submission");
        check(runDirectories(f.root()).isEmpty(), "profile write failure creates no service job");
        check(!w.statusBar()->currentMessage().isEmpty(), "profile write failure is reported");
    });
    for (bool preview : {true, false}) {
        test(preview ? "preview recovers from unknown run" : "submit recovers from unknown run",
             [&] {
                 Fixture f(argv[1]);
                 f.configure();
                 MainWindow w;
                 f.openCase(w);
                 check(w.submitCurrentCase(), "create owned job service");
                 f.release();
                 check(waitFor([&] {
                           return persistedJob(f.root()).state() == jobs::JOB_STATE_COMPLETED;
                       }),
                       "initial job completes before replacing reference");
                 w.*member(RunIdMember{}) = QUuid::createUuid().toString(QUuid::WithoutBraces);
                 w.statusBar()->clearMessage();
                 if (preview) {
                     check(w.previewMesh(PYTHON_EXECUTABLE, {f.previewScript()}),
                           "unknown run does not prevent preview");
                     auto controllers = w.findChildren<PreviewController*>();
                     check(controllers.size() == 1, "window owns preview controller");
                     check(waitFor([&] { return !controllers.first()->running(); }),
                           "recovered preview finishes");
                     check(controllers.first()->mesh() != nullptr, "recovered preview has mesh");
                 } else {
                     check(w.submitCurrentCase(), "unknown run does not prevent submission");
                     check(runDirectories(f.root()).size() == 2, "recovered submit creates job");
                 }
                 check(w.statusBar()->currentMessage().contains("Unknown run_id"),
                       "unknown run lookup is reported");
             });
    }
    test("preview finish and busy guards", [&] {
        Fixture f(argv[1]);
        f.configure();
        MainWindow w;
        f.openCase(w);
        auto previewScript = f.previewScript();
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
        checkPreviewDisplay(w);
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
        checkCanRestart(w, f);
    });
    for (auto source : {"real", "mock", "precomputed", "missing", "REAL"}) {
        test((QString("provenance and screenshot ") + source).toUtf8().constData(), [&] {
            Fixture f(argv[1]);
            MainWindow w;
            auto dir = f.result(source);
            check(w.openResultsPath(dir + "/manifest.json"), "open provenance fixture");
            check(w.lastMeshDigestCompare() == MeshSha256Compare::PreviewMissing &&
                      w.lastMeshDigestDetail().isEmpty() && !w.lastOpenCancelledForMeshDigest(),
                  "result without preview opens with PreviewMissing and no cancellation");
            sourceChecks(w, source);
            auto png = f.temp.filePath("capture.png");
            check(w.captureResults(png), "captureResults wrapper succeeds");
            checkWatermark(png, w.provenanceWatermark().toUtf8());
        });
    }
    for (auto source : {"precomputed",
                        "missing",
                        "missing-time",
                        "invalid-time",
                        "unconfigured",
                        "absent-manifest"}) {
        test((QString("failed job fallback ") + source).toUtf8().constData(), [&] {
            Fixture f(argv[1]);
            QString fallback;
            if (QString(source) != "unconfigured")
                fallback = QString(source) == "absent-manifest" ? f.temp.filePath("absent")
                                                                : f.result(source);
            if (QString(source) == "missing-time" || QString(source) == "invalid-time") {
                QJsonObject provenance{{"schema", "chipcae.provenance/0.1"},
                                       {"source", "precomputed"}};
                if (QString(source) == "invalid-time")
                    provenance["generated_at"] = "invalid timestamp";
                write(fallback + "/provenance.json", QJsonDocument(provenance).toJson());
            }
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
                } else if (QString(source) == "missing") {
                    check(w.sourceBadgeText().contains("来源未知") &&
                              w.provenanceWatermark() == "MOCK",
                          "missing fallback provenance");
                    check(w.sourceBannerText().contains("生成时间未知"),
                          "missing provenance explicitly reports unknown generation time");
                } else {
                    check(w.provenanceWatermark() == "预计算" &&
                              w.sourceBadgeText().contains("预计算"),
                          "invalid timestamp preserves precomputed source");
                    check(w.sourceBannerText().contains("生成时间未知"),
                          "missing or invalid timestamp is explicit in fallback banner");
                }
            }
            if (QString(source) == "precomputed")
                checkCanRestart(w, f);
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
        checkCanRestart(w, f);
    });
    std::cout << "Acceptance failures: " << failures << '\n';
    return failures ? 1 : 0;
}
