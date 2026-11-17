#include "CaseDocument.hpp"
#include "DefectsDialog.hpp"
#include "ImportController.hpp"
#include "ImportSourceDialog.hpp"
#include "MaterialMapMissingDialog.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
namespace chipcae::desktop {
struct ImportControllerTestAccess {
    static QStringList arguments(const ImportRequest& request) {
        return ImportController::argumentsFor(request);
    }
    static void setCopy(ImportController& controller,
                        std::function<bool(const QString&, const QString&)> copy) {
        controller.copyOutput_ = std::move(copy);
    }
};
} // namespace chipcae::desktop
using namespace chipcae::desktop;
namespace {
static_assert(std::is_same_v<decltype(&MaterialMapMissingDialog::prompt), QString (*)(QWidget*)>);
static_assert(std::is_constructible_v<MaterialMapMissingDialog, QWidget*>);
static_assert(!std::is_constructible_v<MaterialMapMissingDialog, QWidget*, const QString&>);
int failures = 0, checks = 0;
void check(bool ok, const QString& message) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << message.toStdString() << '\n';
    }
}
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    require(f.open(QIODevice::WriteOnly), "open fixture");
    require(f.write(bytes) == bytes.size(), "write fixture");
}
QByteArray read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}
QStringList recorded(const QString& path) {
    QStringList result;
    if (!QFile::exists(path))
        return result;
    const auto parsed = Json::parse(read(path).toStdString());
    for (const auto& item : parsed.array_range())
        result << QString::fromStdString(item.as<std::string>());
    return result;
}
void checkStagedArguments(const ImportRequest& request, const QStringList& actual) {
    const auto outputIndex = actual.indexOf("-o");
    require(outputIndex >= 0 && outputIndex + 1 < actual.size(), "child output argument");
    const auto staging = QFileInfo(actual[outputIndex + 1]).absoluteDir();
    auto staged = request;
    staged.output = staging.filePath("out.stack.json");
    staged.report = staging.filePath("defects.json");
    staged.polyset = request.polyset.isEmpty() ? QString{} : staging.filePath("out.polyset");
    check(staging.dirName().startsWith(".chipcae-import-") &&
              actual == ImportControllerTestAccess::arguments(staged),
          "exact child argv uses staged artifacts, unchanged input/map, and no prefix");
}
void waitFor(ImportController& c, const std::function<void()>& action) {
    QEventLoop loop;
    bool done = false;
    auto connection = QObject::connect(&c, &ImportController::finished, &loop, [&](bool) {
        done = true;
        loop.quit();
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(4000);
    action();
    if (!done && c.busy())
        loop.exec();
    if (c.busy()) {
        c.cancel();
        check(false, "process completion timeout");
    }
    QObject::disconnect(connection);
}
QPushButton* button(QDialog& dialog, const QString& text) {
    for (auto* b : dialog.findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}
// Non-native dialogs are driven through their actual widgets, with an independent timeout.
QString pick(const std::function<QString()>& picker,
             const QString& path,
             bool cancel,
             bool map,
             bool directory = false) {
    bool seen = false;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QFileDialog*>(widget);
            if (!dialog || !dialog->isVisible())
                continue;
            seen = true;
            if (map) {
                for (auto* model : dialog->findChildren<QFileSystemModel*>())
                    for (const auto& name : {"map.json", "map.JSON", "map.JsOn"})
                        check(QDir::match(model->nameFilters(), name),
                              "map filter exposes " + QString(name));
                check(dialog->nameFilters() == QStringList{"JSON (*.json)"}, "exact map filter");
            }
            driver.stop();
            if (cancel) {
                dialog->reject();
            } else if (directory) {
                dialog->setDirectory(path);
                for (auto* b : dialog->findChildren<QPushButton*>())
                    if (b->text().contains("ODB++")) {
                        b->click();
                        return;
                    }
                dialog->reject();
            } else {
                // Allow the filesystem model to populate, then enter the filename
                // and use the same accept path as a user pressing Open.
                QTimer::singleShot(100, dialog, [dialog, path, map] {
                    if (map && QFileInfo(path).isFile() &&
                        QFileInfo(path).suffix().compare("json", Qt::CaseInsensitive) == 0) {
                        for (auto* model : dialog->findChildren<QFileSystemModel*>()) {
                            const auto index = model->index(path);
                            check(index.isValid() && (model->flags(index) & Qt::ItemIsEnabled),
                                  "JSON case variant enabled in actual picker model");
                        }
                    }
                    auto* edit = dialog->findChild<QLineEdit*>("fileNameEdit");
                    require(edit, "file picker filename field");
                    edit->setText(path);
                    if (QFileInfo(path).isFile())
                        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                    else
                        dialog->QDialog::done(QDialog::Accepted);
                });
            }
            return;
        }
    });
    driver.start(30);
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [] {
        for (auto* w : QApplication::topLevelWidgets())
            if (auto* d = qobject_cast<QDialog*>(w); d && d->isVisible())
                d->reject();
    });
    watchdog.start(2000);
    auto result = picker();
    check(seen, "picker opens a file dialog");
    return result;
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setAttribute(Qt::AA_DontUseNativeDialogs);
    const bool widgets = argc > 1 && QByteArray(argv[1]) == "--dialog";
    try {
        QTemporaryDir dir(QDir::tempPath() + "/import contract spaces-XXXXXX");
        require(dir.isValid(), "temporary directory");
        const auto map = dir.filePath("material map.JSON");
        write(map, "not parsed by desktop");
        const auto script = dir.filePath("record contract.py");
        // Recording fixture covers argv and deliberately malformed protocol responses.
        write(script, R"PY(import json,pathlib,runpy,sys,time
mock,record,mode=sys.argv[1:4]
a=sys.argv[4:]
pathlib.Path(record).write_text(json.dumps(a))
def value(flag):
    return a[a.index(flag)+1] if flag in a else None
stack=runpy.run_path(mock)['STACK']
out=value('-o'); report=value('--report'); poly=value('--polyset')
pathlib.Path(out).write_text(json.dumps({} if mode=='invalid-stack' else stack))
if poly: pathlib.Path(poly).write_text('polyset')
if mode=='copy-failure':
    pathlib.Path(poly).unlink()
    pathlib.Path(poly).mkdir()
assert not pathlib.Path(record.removesuffix('.argv')+'.stack').exists()
if mode=='hang':
    while True: time.sleep(1)
code,rc={'confirm':('IMPORT_CONFIRMATION_REQUIRED',6),'reject':('IMPORT_REJECTED',7),
'map-missing':('MATERIAL_MAPPING_MISSING',9),'crash-map':('MATERIAL_MAPPING_MISSING',9),'mismatch':('IMPORT_REJECTED',0),
'missing6':('OK',6),'lower':('ok',0)}.get(mode,('OK',0))
if mode=='confirm' and '--accept-b-tier' in a: code,rc='OK',0
if report and mode not in ('missing6','missing0'):
    payload={'code':code,'message':'fixture message','defects':[] if code=='OK' else [{'code':'MOCK_GAP'}]}
    if mode=='grade-conflict': payload['grade']='C'
    if mode=='confirm': payload['grade']='B'
    if mode.startswith('bad-'):
        payload['code']='MATERIAL_MAPPING_MISSING'; payload['grade']='B'; rc=9
        if mode=='bad-code-type': payload['code']=9
        if mode=='bad-message-missing': del payload['message']
        if mode=='bad-message-type': payload['message']=123
        if mode=='bad-defects-missing': del payload['defects']
        if mode=='bad-defects-type': payload['defects']={}
    pathlib.Path(report).write_text('{' if mode=='corrupt' else json.dumps(payload))
if mode=='crash-map':
    import os,signal
    os.kill(os.getpid(),signal.SIGKILL)
sys.exit(rc)
)PY");
        if (!widgets) {
            int mockSerial = 0;
            // Test the shipped mock directly, including its exact input-body mode convention.
            for (bool acceptBTier : {false, true}) {
                for (const auto& mode : {"success",
                                         "confirm",
                                         "b",
                                         "reject",
                                         "c",
                                         "map-missing",
                                         "code-mismatch",
                                         "missing",
                                         "corrupt",
                                         "invalid",
                                         "hang",
                                         "unknown\nreject",
                                         "confirm\nsuccess"}) {
                    const QString m = mode;
                    const auto tag = m.section('\n', 0, 0);
                    auto input = dir.filePath(QString("mock %1 ").arg(++mockSerial) + tag + ".xml");
                    auto output = input + ".stack", report = input + ".report";
                    write(input, m.toUtf8());
                    QProcess p;
                    QStringList arguments{MOCK_SCRIPT,
                                          input,
                                          "-o",
                                          output,
                                          "--material-map",
                                          map,
                                          "--polyset",
                                          output + ".poly",
                                          "--report",
                                          report};
                    if (acceptBTier)
                        arguments << "--accept-b-tier";
                    p.start(PYTHON_EXECUTABLE, arguments);
                    require(p.waitForStarted(), "launch Python mock");
                    if (m == "hang") {
                        check(!p.waitForFinished(200),
                              "mock hang is cancellable and does not exit");
                        p.kill();
                        p.waitForFinished();
                        continue;
                    }
                    require(p.waitForFinished(3000), "mock completion");
                    const bool confirm = (tag == "confirm" || tag == "b") && !acceptBTier;
                    const bool reject = m == "reject" || m == "c" || m == "invalid";
                    int expected = confirm                              ? 6
                                   : reject                             ? 7
                                   : m == "map-missing"                 ? 9
                                   : (m == "missing" || m == "corrupt") ? 1
                                                                        : 0;
                    check(p.exitCode() == expected,
                          "mock exit " + m + ": " + p.readAllStandardError());
                    if (m == "missing") {
                        check(!QFile::exists(report), "mock missing omits report");
                        continue;
                    }
                    check(QFile::exists(report), "mock writes report " + m);
                    bool parsed = false;
                    try {
                        auto j = Json::parse(read(report).toStdString());
                        parsed = true;
                        const QString code = confirm ? "IMPORT_CONFIRMATION_REQUIRED"
                                             : reject || m == "code-mismatch" ? "IMPORT_REJECTED"
                                             : m == "map-missing" ? "MATERIAL_MAPPING_MISSING"
                                                                  : "OK";
                        check(j.contains("code") &&
                                  j.at("code").as<std::string>() == code.toStdString(),
                              "mock report code " + m);
                        check(j.contains("message") && j.at("message").is_string() &&
                                  j.contains("defects") && j.at("defects").is_array(),
                              "mock report shape " + m);
                        if (code == "OK")
                            check(j.contains("defects") && j.at("defects").empty(),
                                  "mock success empty defects");
                        if (confirm || reject)
                            check(j.contains("defects") && !j.at("defects").empty(),
                                  "mock defects " + m);
                    } catch (const std::exception&) {
                    }
                    check(parsed == (m != "corrupt"), "mock JSON presence " + m);
                    if (m == "invalid" && QFile::exists(output)) {
                        CaseDocument d;
                        require(d.load(FIXTURE_DIR "/fcbga_demo.case.json"), "baseline");
                        check(!d.checkImportedStack(Json::parse(read(output).toStdString())),
                              "mock invalid stack");
                    }
                }
            }
        }
        // Exercise the shipped mock through the controller, including publication on rerun.
        for (const auto& mode : {"confirm", "b"}) {
            CaseDocument document;
            require(document.load(FIXTURE_DIR "/fcbga_demo.case.json"), "mock rerun baseline");
            const auto before = document.stackJson();
            ImportController controller(document);
            ImportRequest request;
            request.input = dir.filePath(QString("shipped ") + mode + ".xml");
            request.output = request.input + ".stack.json";
            request.report = request.input + ".report.json";
            request.materialMap = map;
            request.program = PYTHON_EXECUTABLE;
            request.prefix = {MOCK_SCRIPT};
            write(request.input, mode);
            waitFor(controller,
                    [&] { check(controller.start(request), "start shipped B-tier mock"); });
            check(controller.outcome() == ImportOutcome::NeedsConfirmation &&
                      document.stackJson() == before && !QFile::exists(request.output),
                  "shipped mock requires confirmation without publication");
            waitFor(controller,
                    [&] { check(controller.restartAccepted(), "restart shipped mock"); });
            check(controller.outcome() == ImportOutcome::Accepted &&
                      controller.reportCode() == "OK" && controller.exitCode() == 0 &&
                      document.stackJson() != before && QFile::exists(request.output),
                  "shipped mock accepts and publishes confirmed rerun");
        }
        // A valid previous report must never supply fields for a malformed later one.
        for (const auto& malformed : {"bad-code-type",
                                      "bad-message-missing",
                                      "bad-message-type",
                                      "bad-defects-missing",
                                      "bad-defects-type",
                                      "corrupt"}) {
            CaseDocument document;
            require(document.load(FIXTURE_DIR "/fcbga_demo.case.json"), "report reset baseline");
            const auto before = document.stackJson();
            ImportController controller(document);
            ImportRequest request;
            request.input = dir.filePath(QString("report reset ") + malformed + ".xml");
            write(request.input, "fixture");
            request.output = request.input + ".stack";
            request.materialMap = map;
            request.program = PYTHON_EXECUTABLE;
            const auto record = request.input + ".argv";
            request.prefix = {script, MOCK_SCRIPT, record, "confirm"};
            waitFor(controller, [&] { check(controller.start(request), "start valid report"); });
            require(controller.reportCode() == "IMPORT_CONFIRMATION_REQUIRED" &&
                        controller.grade() == "B",
                    "previous valid report fields");
            request.prefix = {script, MOCK_SCRIPT, record, malformed};
            waitFor(controller,
                    [&] { check(controller.start(request), "start malformed report"); });
            check(controller.outcome() == ImportOutcome::Failed &&
                      controller.reportCode().isEmpty() && controller.grade().isEmpty() &&
                      !controller.needsMaterialMapReselect() && document.stackJson() == before &&
                      !QFile::exists(request.output),
                  "malformed subsequent report clears fields and preserves document");
        }
        // External files arriving after preflight remain untouched when publication rolls back.
        for (int scenario = 0; scenario < 6; ++scenario) {
            const int externalIndex = scenario % 3;
            const bool duringCopy = scenario >= 3;
            CaseDocument document;
            require(document.load(FIXTURE_DIR "/fcbga_demo.case.json"),
                    "late destination baseline");
            const auto before = document.stackJson();
            ImportController controller(document);
            ImportRequest request;
            request.input = dir.filePath(QString("late destination %1.xml").arg(scenario));
            write(request.input, "fixture");
            request.output = request.input + ".stack";
            request.polyset = request.input + ".poly";
            request.report = request.input + ".report";
            request.materialMap = map;
            request.program = PYTHON_EXECUTABLE;
            request.prefix = {script, MOCK_SCRIPT, request.input + ".record.argv", "success"};
            const QStringList destinations{request.output, request.polyset, request.report};
            int copies = 0;
            ImportControllerTestAccess::setCopy(
                controller, [&](const QString& source, const QString& destination) {
                    if (duringCopy && copies == externalIndex)
                        write(destinations[externalIndex], "externally owned data");
                    ++copies;
                    // Deliberately destructive copy: the controller must keep externally
                    // owned paths away from the publication operation altogether.
                    QFile::remove(destination);
                    return QFile::copy(source, destination);
                });
            waitFor(controller, [&] {
                check(controller.start(request), "late destination passes initial preflight");
                if (!duringCopy)
                    write(destinations[externalIndex], "externally owned data");
            });
            check(controller.outcome() == ImportOutcome::Failed &&
                      controller.error() == "Cannot publish import outputs" &&
                      copies == externalIndex + (duringCopy ? 1 : 0) &&
                      document.stackJson() == before && !document.isDirty(),
                  "late collision stops publication and preserves document");
            for (int i = 0; i < destinations.size(); ++i)
                check(i == externalIndex ? read(destinations[i]) == "externally owned data"
                                         : !QFile::exists(destinations[i]),
                      "rollback removes only outputs owned by this run");
        }
        int serial = 0;
        auto run = [&](const QString& mode,
                       ImportOutcome expected,
                       const QString& suffix = ".xml",
                       ImportKind kind = ImportKind::Ecad,
                       bool directory = false,
                       bool accept = false,
                       const QString& materialMap = {},
                       const QString& auxiliaryDir = {},
                       const QString& ignoredMapTarget = {}) {
            CaseDocument document;
            require(document.load(FIXTURE_DIR "/fcbga_demo.case.json"), "load baseline");
            const auto before = document.stackJson();
            ImportController c(document);
            DefectsDialog dialog(c);
            auto input = dir.filePath(QString("input %1").arg(++serial) + suffix);
            if (directory)
                require(QDir().mkdir(input), "create ODB directory");
            else
                write(input, "fixture");
            const auto record = input + ".argv";
            ImportRequest r;
            r.input = input;
            r.output = input + ".stack";
            r.materialMap = materialMap.isEmpty() ? (kind == ImportKind::Assembly ? QString{} : map)
                                                  : materialMap;
            r.acceptBTier = accept;
            r.polyset = input + ".poly";
            r.report = input + ".report";
            if (!auxiliaryDir.isEmpty()) {
                r.polyset = QDir(auxiliaryDir).filePath(QFileInfo(r.polyset).fileName());
                r.report = QDir(auxiliaryDir).filePath(QFileInfo(r.report).fileName());
            }
            if (ignoredMapTarget == "input")
                r.materialMap = r.input;
            else if (ignoredMapTarget == "output")
                r.materialMap = r.output;
            else if (ignoredMapTarget == "polyset")
                r.materialMap = r.polyset;
            else if (ignoredMapTarget == "report")
                r.materialMap = r.report;
            r.program =
                mode == "launch-failure" ? "/nonexistent/chipcae-import" : PYTHON_EXECUTABLE;
            r.prefix = {script, MOCK_SCRIPT, record, mode};
            r.kind = kind;
            int copyCalls = 0;
            const bool partialCopy = mode.startsWith("partial-copy-");
            const int failingCopy = partialCopy ? mode.right(1).toInt() : -1;
            if (partialCopy)
                ImportControllerTestAccess::setCopy(
                    c, [&](const QString& source, const QString& destination) {
                        if (copyCalls++ == failingCopy) {
                            write(destination, "partial import output");
                            check(QFileInfo(destination).isFile(),
                                  "failure injected after creation");
                            return false;
                        }
                        return QFile::copy(source, destination);
                    });
            bool replaced = false;
            int confirmations = 0;
            QObject::connect(&c, &ImportController::finished, [&](bool b) { replaced = b; });
            QObject::connect(&c, &ImportController::userConfirmed, [&] {
                ++confirmations;
                check(!c.busy(), "confirmation signal before rerun");
            });
            waitFor(c, [&] {
                check(c.start(r), "request starts " + mode);
                check(c.outcome() == ImportOutcome::Running, "Running outcome " + mode);
                check(!c.start(r), "busy rejects duplicate");
                check(!c.restartAccepted(), "busy rejects restart");
                if (mode == "hang")
                    QTimer::singleShot(150, &c, [&] {
                        check(!QFile::exists(r.output) && !QFile::exists(r.polyset) &&
                                  !QFile::exists(r.report),
                              "running child has no caller-visible artifacts");
                        c.cancel();
                    });
            });
            check(c.outcome() == expected, "outcome " + mode);
            if (partialCopy)
                check(copyCalls == failingCopy + 1 &&
                          c.error() == "Cannot publish import outputs" && !document.isDirty(),
                      "partial copy stops publication and preserves clean document " + mode);
            const QString code = mode == "confirm" ? "IMPORT_CONFIRMATION_REQUIRED"
                                 : mode == "reject" || mode == "mismatch" ? "IMPORT_REJECTED"
                                 : mode == "map-missing" || mode == "crash-map"
                                     ? "MATERIAL_MAPPING_MISSING"
                                 : mode == "lower" ? "ok"
                                                   : "OK";
            if (mode != "hang" && mode != "launch-failure" && mode != "missing6" &&
                !mode.startsWith("bad-") && mode != "corrupt")
                check(c.reportCode() == code, "raw report code " + mode);
            if (mode == "success" || mode == "grade-conflict")
                check(c.grade() == (mode == "grade-conflict" ? "C" : ""),
                      "optional grade for display only");
            check(c.needsMaterialMapReselect() == (mode == "map-missing"), "reselection " + mode);
            if (mode == "crash-map")
                check(c.exitCode() == 9 && c.exitStatus() == QProcess::CrashExit,
                      "crash fixture distinguishes signal 9 from normal protocol exit 9");
            if (mode == "mismatch" || mode == "missing6")
                check(c.error().contains("exit/report mismatch"),
                      "paired exit/report error " + mode);
            if (mode.startsWith("bad-"))
                check(c.reportCode().isEmpty() && c.grade().isEmpty(),
                      "invalid report exposes no partial fields " + mode);
            if (mode == "corrupt")
                check(c.reportBytes() == "{", "retain malformed raw report");
            if (expected == ImportOutcome::Accepted) {
                check(replaced && document.isDirty(),
                      "accepted replaces document and marks it dirty");
                check(QFile::exists(r.output) && QFile::exists(r.polyset) &&
                          QFile::exists(r.report),
                      "accepted publishes all outputs");
                // Expected stack is independently extracted from the shipped fixture constant.
                QProcess p;
                p.start(PYTHON_EXECUTABLE,
                        {"-c",
                         "import runpy,json,sys; "
                         "print(json.dumps(runpy.run_path(sys.argv[1])['STACK']))",
                         MOCK_SCRIPT});
                require(p.waitForFinished(), "expected stack fixture");
                check(document.stackJson() == Json::parse(p.readAllStandardOutput().toStdString()),
                      "accepted stack exact JSON equality");
            } else {
                check(!replaced && document.stackJson() == before, "unchanged stack " + mode);
                check(!QFile::exists(r.output) && !QFile::exists(r.polyset) &&
                          !QFile::exists(r.report),
                      "no partial publication " + mode);
            }
            if (mode != "launch-failure")
                check(QFile::exists(record), "process actually launched " + mode);
            if (QFile::exists(record)) {
                auto actual = recorded(record);
                for (const auto& flag : {"-o", "--polyset", "--report"}) {
                    const auto index = actual.indexOf(flag);
                    require(index >= 0 && index + 1 < actual.size(), "staged artifact flag");
                    check(
                        QFileInfo(actual[index + 1]).absolutePath().contains(".chipcae-import-") &&
                            !QStringList{r.output, r.polyset, r.report}.contains(actual[index + 1]),
                        "child artifact paths are private staging paths");
                }
                checkStagedArguments(r, actual);
                const auto oi = actual.indexOf("-o");
                require(oi >= 0 && oi + 1 < actual.size(), "recorded output argument");
                QStringList wanted;
                if (kind == ImportKind::Assembly)
                    wanted = {"--assembly",
                              input,
                              "-o",
                              r.output,
                              "--report",
                              r.report,
                              "--polyset",
                              r.polyset};
                else
                    wanted = {input,
                              "-o",
                              r.output,
                              "--material-map",
                              r.materialMap,
                              "--polyset",
                              r.polyset,
                              "--report",
                              r.report};
                if (accept)
                    wanted << "--accept-b-tier";
                check(ImportControllerTestAccess::arguments(r) == wanted,
                      "full QStringList argv order and unchanged input " + suffix);
                check(QDir(dir.path())
                          .entryList({".chipcae-import-*"},
                                     QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                          .isEmpty(),
                      "staging removed " + mode);
            }
            if (mode == "confirm") {
                auto* b = button(dialog, "确认并继续");
                check(b && b->isEnabled(), "confirmation button enabled");
                check(dialog.findChild<QPlainTextEdit*>()->toPlainText().contains("MOCK_GAP"),
                      "dialog displays defects");
                waitFor(c, [&] {
                    if (widgets && b)
                        b->click();
                    else
                        check(c.restartAccepted(), "restartAccepted starts rerun");
                });
                check(confirmations == 1, "confirmation signal exactly once");
                auto args = recorded(record);
                check(args.contains("--material-map") && args.contains(map) &&
                          args.last() == "--accept-b-tier" && !args.contains("--force"),
                      "rerun retains mapping and appends accept flag");
                check(args.contains(input), "rerun same input");
                check(c.outcome() == ImportOutcome::Accepted && replaced,
                      "confirmed rerun accepts");
            } else {
                c.confirmDefects();
                check(confirmations == 0 && !c.restartAccepted(),
                      "no-op confirmation outside NeedsConfirmation");
                if (auto* b = button(dialog, "确认并继续"))
                    check(!b->isEnabled(), "nonconfirmation cannot continue");
            }
            if (expected == ImportOutcome::Accepted || mode == "confirm") {
                check(document.isDirty(), "accepted import leaves document dirty");
                const auto imported = document.stackJson();
                const auto saved = r.output + ".case.json";
                check(document.save(saved), "save imported document");
                CaseDocument reloaded;
                check(reloaded.load(saved) && reloaded.stackJson() == imported,
                      "imported stack survives case round trip");
                const auto outputBytes = read(r.output);
                const auto polysetBytes = read(r.polyset);
                const auto reportBytes = read(r.report);
                check(!c.start(r) && !c.busy(), "reject existing outputs before launching");
                check(c.error() ==
                          "import destination already exists or collides with another path",
                      "existing destination has deterministic error");
                check(document.stackJson() == imported && read(r.output) == outputBytes &&
                          read(r.polyset) == polysetBytes && read(r.report) == reportBytes,
                      "existing output rejection preserves document and published files");
            }
        };
        for (const auto& pair : {std::pair{"success", ImportOutcome::Accepted},
                                 {"confirm", ImportOutcome::NeedsConfirmation},
                                 {"reject", ImportOutcome::Rejected},
                                 {"map-missing", ImportOutcome::Rejected},
                                 {"crash-map", ImportOutcome::Failed},
                                 {"mismatch", ImportOutcome::Failed},
                                 {"missing6", ImportOutcome::Failed},
                                 {"corrupt", ImportOutcome::Failed},
                                 {"bad-message-missing", ImportOutcome::Failed},
                                 {"bad-message-type", ImportOutcome::Failed},
                                 {"bad-defects-missing", ImportOutcome::Failed},
                                 {"bad-defects-type", ImportOutcome::Failed},
                                 {"invalid-stack", ImportOutcome::Failed},
                                 {"copy-failure", ImportOutcome::Failed},
                                 {"partial-copy-0", ImportOutcome::Failed},
                                 {"partial-copy-1", ImportOutcome::Failed},
                                 {"partial-copy-2", ImportOutcome::Failed},
                                 {"hang", ImportOutcome::Failed},
                                 {"launch-failure", ImportOutcome::Failed},
                                 {"lower", ImportOutcome::Failed},
                                 {"grade-conflict", ImportOutcome::Accepted}})
            run(pair.first, pair.second);
#ifdef Q_OS_UNIX
        {
            QTemporaryDir auxiliary("/dev/shm/chipcae-import-XXXXXX");
            struct stat localDevice{}, remoteDevice{};
            if (auxiliary.isValid() && ::stat(QFile::encodeName(dir.path()), &localDevice) == 0 &&
                ::stat(QFile::encodeName(auxiliary.path()), &remoteDevice) == 0 &&
                localDevice.st_dev != remoteDevice.st_dev) {
                run("success",
                    ImportOutcome::Accepted,
                    ".xml",
                    ImportKind::Ecad,
                    false,
                    false,
                    {},
                    auxiliary.path());
                run("map-missing",
                    ImportOutcome::Rejected,
                    ".xml",
                    ImportKind::Ecad,
                    false,
                    false,
                    {},
                    auxiliary.path());
                std::cout << "Cross-device publication exercised on distinct filesystem devices\n";
            } else {
                std::cout << "SKIP: no writable second filesystem for cross-device publication\n";
            }
        }
#endif
        for (const auto& suffix : {".TGZ", ".Zip"})
            run("success", ImportOutcome::Accepted, suffix);
        run("success", ImportOutcome::Accepted, " ODB directory", ImportKind::Ecad, true);
        run("success", ImportOutcome::Accepted, ".arbitrary", ImportKind::Assembly);
        run("success", ImportOutcome::Accepted, ".xml", ImportKind::Ecad, false, true);
        run("success", ImportOutcome::Accepted, ".assembly", ImportKind::Assembly, false, true);
        // Mapping path validity belongs to the import program, including code 9 recovery.
        for (const auto& materialMap : {dir.filePath("absent map.json"), dir.path()})
            run("map-missing",
                ImportOutcome::Rejected,
                ".xml",
                ImportKind::Ecad,
                false,
                false,
                materialMap);
        run("success",
            ImportOutcome::Accepted,
            ".assembly",
            ImportKind::Assembly,
            false,
            false,
            dir.filePath("ignored assembly map.json"));
        for (const auto& target : {"input", "output", "polyset", "report"})
            run("success",
                ImportOutcome::Accepted,
                ".assembly",
                ImportKind::Assembly,
                false,
                false,
                {},
                {},
                target);
        {
            CaseDocument d;
            require(d.load(FIXTURE_DIR "/fcbga_demo.case.json"), "baseline");
            auto before = d.stackJson();
            ImportController c(d);
            ImportRequest r;
            r.input = dir.filePath("local.xml");
            write(r.input, "success");
            r.output = dir.filePath("local.stack");
            r.program = "/nonexistent/should-not-launch";
            check(!c.start(r), "empty material map rejected locally");
            check(c.error() == "material map required" && c.outcome() == ImportOutcome::Failed &&
                      !c.busy() && !c.needsMaterialMapReselect(),
                  "local missing-map exact state");
            if (c.busy())
                waitFor(c, [&] { c.cancel(); });
            check(!c.start(r.input, r.output, {}, {}, "/nonexistent/should-not-launch"),
                  "legacy overload rejects without map");
            check(c.error() == "material map required" && d.stackJson() == before,
                  "legacy exact error and unchanged stack");
            if (c.busy())
                waitFor(c, [&] { c.cancel(); });
            r.materialMap = map;
            for (const auto& destination : {r.input, map, dir.path()}) {
                auto collision = r;
                collision.output = destination;
                check(!c.start(collision) && !c.busy() &&
                          c.error() ==
                              "import destination already exists or collides with another path",
                      "input, map and existing-directory collisions have deterministic errors");
            }
            for (bool duplicateReport : {false, true}) {
                auto collision = r;
                if (duplicateReport)
                    collision.report = collision.output;
                else
                    collision.polyset = collision.output;
                check(!c.start(collision) && !c.busy() &&
                          c.error() ==
                              "import destination already exists or collides with another path",
                      "duplicate destinations have deterministic errors");
            }
            for (auto kind : {ImportKind::Ecad, ImportKind::Assembly}) {
                r.kind = kind;
                r.materialMap = map;
                for (const auto& input : {QString{},
                                          dir.filePath("absent.xml"),
                                          dir.filePath("absent.TGZ"),
                                          dir.filePath("absent.ZIP"),
                                          dir.filePath("absent assembly.json"),
                                          kind == ImportKind::Ecad ? map : dir.path()}) {
                    r.input = input;
                    int finished = 0, defects = 0;
                    const auto finishedConnection =
                        QObject::connect(&c, &ImportController::finished, [&] { ++finished; });
                    const auto defectsConnection =
                        QObject::connect(&c, &ImportController::defectsReady, [&] { ++defects; });
                    check(!c.start(r), "invalid input rejected before launching");
                    check(c.error() == "unsupported import input" &&
                              c.outcome() == ImportOutcome::Failed && !c.busy() &&
                              c.exitCode() == -1 && c.reportCode().isEmpty() &&
                              !c.needsMaterialMapReselect(),
                          "invalid input has synchronous validation failure state");
                    check(finished == 0 && defects == 0,
                          "invalid input does not reach process completion");
                    QObject::disconnect(finishedConnection);
                    QObject::disconnect(defectsConnection);
                    check(!QFileInfo::exists(r.output), "invalid input publishes no output");
                    if (c.busy())
                        waitFor(c, [&] { c.cancel(); });
                    check(d.stackJson() == before, "invalid input preserves document");
                }
            }
        }
        for (const auto kind : {ImportKind::Ecad, ImportKind::Assembly}) {
            for (int options = 0; options < 8; ++options) {
                CaseDocument d;
                require(d.load(FIXTURE_DIR "/fcbga_demo.case.json"), "optional export baseline");
                ImportController c(d);
                ImportRequest r;
                r.kind = kind;
                r.input = dir.filePath(QString("optional %1.xml").arg(++serial));
                write(r.input, "fixture");
                r.output = r.input + ".stack";
                r.materialMap = map;
                if (options & 1)
                    r.report = r.input + ".report";
                if (options & 2)
                    r.polyset = r.input + ".poly";
                r.acceptBTier = options & 4;
                r.program = PYTHON_EXECUTABLE;
                const auto record = r.input + ".argv";
                r.prefix = {script, MOCK_SCRIPT, record, "map-missing"};
                waitFor(c, [&] { check(c.start(r), "start optional recovery"); });
                check(c.outcome() == ImportOutcome::Rejected && c.needsMaterialMapReselect(),
                      "code 9 recovery is independent of report export");
                for (const auto& invalidReport : {"missing0", "corrupt", "mismatch"}) {
                    r.prefix = {script, MOCK_SCRIPT, record, invalidReport};
                    waitFor(c, [&] { check(c.start(r), "start invalid optional report"); });
                    check(c.outcome() == ImportOutcome::Failed &&
                              c.error().contains("exit/report mismatch") && !d.isDirty() &&
                              !QFile::exists(r.output) && !QFile::exists(r.report) &&
                              !QFile::exists(r.polyset),
                          "invalid exit 0 report fails regardless of export selection");
                }
                r.prefix = {script, MOCK_SCRIPT, record, "success"};
                waitFor(c, [&] {
                    check(c.start(r), "start optional exports");
                    check(!c.needsMaterialMapReselect(), "new run clears map missing state");
                });
                check(c.outcome() == ImportOutcome::Accepted && !c.needsMaterialMapReselect(),
                      "optional exports accept and reset recovery state");
                const auto args = recorded(record);
                checkStagedArguments(r, args);
                QStringList wanted;
                if (kind == ImportKind::Assembly)
                    wanted = {"--assembly", r.input, "-o", r.output};
                else
                    wanted = {r.input, "-o", r.output, "--material-map", map};
                if (kind == ImportKind::Assembly && !r.report.isEmpty())
                    wanted << "--report" << r.report;
                if (!r.polyset.isEmpty())
                    wanted << "--polyset" << r.polyset;
                if (kind == ImportKind::Ecad && !r.report.isEmpty())
                    wanted << "--report" << r.report;
                if (r.acceptBTier)
                    wanted << "--accept-b-tier";
                check(ImportControllerTestAccess::arguments(r) == wanted,
                      "exact caller argv for every optional flag combination");
                check(args.contains("--report"), "internal report always requested");
                check(QFile::exists(r.output) && (r.report.isEmpty() || QFile::exists(r.report)) &&
                          (r.polyset.isEmpty() || QFile::exists(r.polyset)),
                      "requested optional exports published");
                check(QDir(dir.path())
                          .entryList({".chipcae-import-*"},
                                     QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                          .isEmpty(),
                      "optional staging removed");
                check(!QFile::exists(dir.filePath("defects.json")) &&
                          !QFile::exists(dir.filePath("out.polyset")),
                      "unrequested exports remain unpublished");
            }
        }
        {
            CaseDocument d;
            require(d.load(FIXTURE_DIR "/fcbga_demo.case.json"), "staging failure baseline");
            const auto before = d.stackJson();
            ImportController c(d);
            ImportRequest r;
            r.input = dir.filePath("staging failure.xml");
            write(r.input, "fixture");
            r.materialMap = map;
            r.output = dir.filePath("absent parent/output.stack");
            r.program = PYTHON_EXECUTABLE;
            const auto record = r.input + ".argv";
            r.prefix = {script, MOCK_SCRIPT, record, "success"};
            check(!c.start(r) && c.outcome() == ImportOutcome::Failed && !c.busy(),
                  "staging failure refuses launch");
            check(c.error() == "import staging directory creation failed",
                  "staging failure has deterministic error");
            check(!QFile::exists(record) && !QFile::exists(r.output) && d.stackJson() == before &&
                      !c.needsMaterialMapReselect(),
                  "staging failure preserves document without launching");
        }
        for (const int failure : {0, 1, 2}) {
            CaseDocument d;
            require(d.load(FIXTURE_DIR "/fcbga_demo.case.json"), "failed restart baseline");
            const auto before = d.stackJson();
            ImportController c(d);
            DefectsDialog dialog(c);
            ImportRequest r;
            r.input = dir.filePath(QString("failed restart %1.xml").arg(++serial));
            write(r.input, "fixture");
            r.materialMap = map;
            const auto outputDir = r.input + ".outputs";
            require(QDir().mkdir(outputDir), "restart output directory");
            r.output = outputDir + "/out.stack";
            r.report = r.input + ".report";
            r.program = PYTHON_EXECUTABLE;
            const auto record = r.input + ".argv";
            r.prefix = {script, MOCK_SCRIPT, record, "confirm"};
            waitFor(c, [&] { check(c.start(r), "start failed-restart fixture"); });
            require(c.outcome() == ImportOutcome::NeedsConfirmation, "B-tier fixture");
            const auto firstArgs = read(record);
            int confirmed = 0, finished = 0, defects = 0;
            QObject::connect(&c, &ImportController::userConfirmed, [&] {
                ++confirmed;
                check(!c.busy() && c.outcome() == ImportOutcome::NeedsConfirmation,
                      "failed rerun confirmation precedes startup attempt");
            });
            QObject::connect(&c, &ImportController::defectsReady, [&] { ++defects; });
            QObject::connect(&c, &ImportController::finished, [&](bool replaced) {
                ++finished;
                check(!replaced, "failed restart completion does not replace");
            });
            if (failure == 1)
                QFile::remove(r.input);
            else if (failure == 2)
                require(QDir().rmdir(outputDir), "remove restart output parent");
            else
                write(r.output, "external destination");
            auto* confirm = button(dialog, "确认并继续");
            require(confirm && confirm->isEnabled(), "B-tier dialog actionable");
            confirm->click();
            const QString expectedError =
                failure == 1   ? "unsupported import input"
                : failure == 2 ? "import staging directory creation failed"
                               : "import destination already exists or collides with another path";
            check(!c.busy() && c.outcome() == ImportOutcome::Failed && c.error() == expectedError,
                  "failed restart exposes local failure");
            check(confirmed == 1 && finished == 1, "failed restart notifies once");
            check(defects == 1, "failed restart refreshes defects once");
            check(!c.restartAccepted(), "Failed outcome rejects another accepted restart");
            c.confirmDefects();
            check(confirmed == 1 && finished == 1 && defects == 1 &&
                      c.outcome() == ImportOutcome::Failed && c.error() == expectedError,
                  "confirmation after failed restart is a silent no-op preserving failure");
            check(dialog.isVisible() &&
                      dialog.findChild<QPlainTextEdit*>()->toPlainText().contains(c.error()),
                  "failed restart dialog displays new error");
            auto* buttons = dialog.findChild<QDialogButtonBox*>();
            check(buttons && buttons->button(QDialogButtonBox::Close)->isEnabled() &&
                      !confirm->isEnabled(),
                  "failed restart leaves actionable close and disables invalid confirmation");
            check(d.stackJson() == before && !d.isDirty() && read(record) == firstArgs,
                  "failed restart does not launch or change document");
            if (failure == 0)
                check(read(r.output) == "external destination",
                      "failed restart preserves external file");
        }
        if (widgets) {
            auto filters = ImportSourceDialog::nameFilters();
            check(filters.contains("IPC-2581 / ODB++ (*.xml *.tgz *.zip)") &&
                      filters.contains("ODB++ (*.tgz *.zip)"),
                  "ECAD and ODB filters");
            check(filters.join(' ').contains("B 档") &&
                      filters.join(' ').contains("(*.step *.stp)"),
                  "disabled STEP formats disclosed in chooser");
            for (const auto& suffix : {"xml",
                                       "XML",
                                       "tgz",
                                       "TGZ",
                                       "zip",
                                       "Zip",
                                       "step",
                                       "STEP",
                                       "stp",
                                       "STP",
                                       "json",
                                       "txt"}) {
                auto path = dir.filePath(QString("picker.") + suffix);
                write(path, "unparsed");
                bool supported =
                    QStringList{"xml", "tgz", "zip"}.contains(QString(suffix).toLower());
                check(ImportSourceDialog::isSupportedImportPath(path) == supported,
                      "source suffix " + QString(suffix));
                check(pick(
                          [&] {
                              return ImportSourceDialog::getImportPath(
                                  nullptr, "contract", dir.path());
                          },
                          path,
                          false,
                          false) == (supported ? path : QString{}),
                      "source picker validates " + QString(suffix));
            }
            check(!ImportSourceDialog::isSupportedImportPath(dir.filePath("absent.XML")),
                  "nonexistent XML unsupported");
            check(ImportSourceDialog::isSupportedImportPath(dir.path()),
                  "existing directory supported");
            check(pick([&] { return ImportSourceDialog::getImportPath(nullptr, {}, dir.path()); },
                       dir.path(),
                       false,
                       false,
                       true) == dir.path(),
                  "ODB directory returned unchanged");
            for (const auto& suffix : {"json",
                                       "jsoN",
                                       "jsOn",
                                       "jsON",
                                       "jSon",
                                       "jSoN",
                                       "jSOn",
                                       "jSON",
                                       "Json",
                                       "JsoN",
                                       "JsOn",
                                       "JsON",
                                       "JSon",
                                       "JSoN",
                                       "JSOn",
                                       "JSON"}) {
                const auto path = dir.filePath("case variant." + QString(suffix));
                write(path, "{}");
                check(
                    pick(
                        [&] { return ImportSourceDialog::getMaterialMapPath(nullptr, dir.path()); },
                        path,
                        false,
                        true) == path,
                    "map picker accepts literal case variant " + QString(suffix));
            }
            for (const auto& path : {map,
                                     dir.filePath("picker.json"),
                                     dir.filePath("picker.txt"),
                                     dir.path(),
                                     dir.filePath("absent.json")})
                check(
                    pick(
                        [&] { return ImportSourceDialog::getMaterialMapPath(nullptr, dir.path()); },
                        path,
                        false,
                        true) ==
                        ((path == map || path == dir.filePath("picker.json")) ? path : QString{}),
                    "map picker existing JSON only");
            check(
                pick(
                    [&] { return ImportSourceDialog::getMaterialMapPath(nullptr); }, {}, true, true)
                    .isEmpty(),
                "map picker cancellation");
            check(pick([&] { return ImportSourceDialog::getImportPath(nullptr, {}, dir.path()); },
                       {},
                       true,
                       false)
                      .isEmpty(),
                  "source picker cancellation");
            MaterialMapMissingDialog missing;
            check(missing.messageText().contains("缺少材料映射，请选择映射文件后重试"),
                  "missing-map message");
            check(button(missing, "重新选择文件"), "missing-map reselect button");
            QTimer click;
            QObject::connect(&click, &QTimer::timeout, [&] {
                for (auto* w : QApplication::topLevelWidgets())
                    if (auto* d = dynamic_cast<MaterialMapMissingDialog*>(w); d && d->isVisible())
                        if (auto* b = button(*d, "重新选择文件")) {
                            click.stop();
                            b->click();
                        }
            });
            click.start(30);
            check(pick([&] { return MaterialMapMissingDialog::prompt(); }, map, false, true) == map,
                  "missing-map prompt returns selected map");
            click.start(30);
            check(
                pick([&] { return MaterialMapMissingDialog::prompt(); }, {}, true, true).isEmpty(),
                "missing-map prompt cancelled picker");
        }
    } catch (const std::exception& e) {
        check(false, QString("fixture exception: ") + e.what());
    }
    std::cerr << checks << " assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
