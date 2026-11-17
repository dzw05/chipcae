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
                check(dialog->nameFilters() == QStringList{"JSON (*.[jJ][sS][oO][nN])"},
                      "exact map filter");
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
                QTimer::singleShot(100, dialog, [dialog, path] {
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
        // Independent CLI fixture isolates controller assertions from the legacy mock parser.
        write(script, R"PY(import json,pathlib,runpy,sys,time
mock,record,mode=sys.argv[1:4]
a=sys.argv[4:]
pathlib.Path(record).write_text(json.dumps(a))
def value(flag):
    return a[a.index(flag)+1] if flag in a else None
stack=runpy.run_path(mock)['STACK']
out=value('-o'); report=value('--report') or str(pathlib.Path(out).parent/'defects.json'); poly=value('--polyset')
pathlib.Path(out).write_text(json.dumps({} if mode=='invalid-stack' else stack))
if poly: pathlib.Path(poly).write_text('polyset')
if mode=='copy-failure': pathlib.Path(record[:-5]+'.poly').mkdir()
if mode=='hang':
    while True: time.sleep(1)
code,rc={'confirm':('IMPORT_CONFIRMATION_REQUIRED',6),'reject':('IMPORT_REJECTED',7),
'map-missing':('MATERIAL_MAPPING_MISSING',9),'crash-map':('MATERIAL_MAPPING_MISSING',9),'mismatch':('IMPORT_REJECTED',0),
'missing6':('OK',6),'lower':('ok',0)}.get(mode,('OK',0))
if mode=='confirm' and '--accept-b-tier' in a: code,rc='OK',0
if report and mode!='missing6':
    payload={'code':code,'message':'fixture message','defects':[] if code=='OK' else [{'code':'MOCK_GAP'}]}
    if mode=='grade-conflict': payload['grade']='C'
    if mode.startswith('bad-'):
        payload['code']='MATERIAL_MAPPING_MISSING'; payload['grade']='B'; rc=9
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
                p.start(PYTHON_EXECUTABLE,
                        {MOCK_SCRIPT,
                         input,
                         "-o",
                         output,
                         "--material-map",
                         map,
                         "--polyset",
                         output + ".poly",
                         "--report",
                         report,
                         "--accept-b-tier"});
                require(p.waitForStarted(), "launch Python mock");
                if (m == "hang") {
                    check(!p.waitForFinished(200), "mock hang is cancellable and does not exit");
                    p.kill();
                    p.waitForFinished();
                    continue;
                }
                require(p.waitForFinished(3000), "mock completion");
                const bool confirm = tag == "confirm" || tag == "b";
                const bool reject = m == "reject" || m == "c" || m == "invalid";
                int expected = confirm                              ? 6
                               : reject                             ? 7
                               : m == "map-missing"                 ? 9
                               : (m == "missing" || m == "corrupt") ? 1
                                                                    : 0;
                check(p.exitCode() == expected, "mock exit " + m + ": " + p.readAllStandardError());
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
        int serial = 0;
        auto run = [&](const QString& mode,
                       ImportOutcome expected,
                       const QString& suffix = ".xml",
                       ImportKind kind = ImportKind::Ecad,
                       bool directory = false,
                       bool accept = false,
                       const QString& materialMap = {}) {
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
            r.program =
                mode == "launch-failure" ? "/nonexistent/chipcae-import" : PYTHON_EXECUTABLE;
            r.prefix = {script, MOCK_SCRIPT, record, mode};
            r.kind = kind;
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
                    QTimer::singleShot(150, &c, &ImportController::cancel);
            });
            check(c.outcome() == expected, "outcome " + mode);
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
            check(c.needsMaterialMapReselect() == (mode == "map-missing" || mode == "crash-map"),
                  "reselection " + mode);
            if (mode == "crash-map")
                check(c.exitCode() == 9 && c.exitStatus() == QProcess::CrashExit,
                      "crash fixture exercises recovery independent of exit status");
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
                check(!QFile::exists(r.output) &&
                          (mode == "copy-failure" ? QFileInfo(r.polyset).isDir()
                                                  : !QFile::exists(r.polyset)) &&
                          !QFile::exists(r.report),
                      "no partial publication " + mode);
            }
            if (mode != "launch-failure")
                check(QFile::exists(record), "process actually launched " + mode);
            if (QFile::exists(record)) {
                auto actual = recorded(record);
                const auto oi = actual.indexOf("-o");
                require(oi >= 0 && oi + 1 < actual.size(), "recorded output argument");
                const QString stage = QFileInfo(actual[oi + 1]).absolutePath();
                QStringList wanted;
                if (kind == ImportKind::Assembly)
                    wanted = {"--assembly",
                              input,
                              "-o",
                              stage + "/out.stack.json",
                              "--report",
                              stage + "/defects.json",
                              "--polyset",
                              stage + "/out.polyset"};
                else
                    wanted = {input,
                              "-o",
                              stage + "/out.stack.json",
                              "--material-map",
                              r.materialMap,
                              "--polyset",
                              stage + "/out.polyset",
                              "--report",
                              stage + "/defects.json"};
                if (accept)
                    wanted << "--accept-b-tier";
                check(actual == wanted,
                      "full QStringList argv order and unchanged input " + suffix);
                check(!QDir(stage).exists(), "staging removed " + mode);
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
                                 {"hang", ImportOutcome::Failed},
                                 {"launch-failure", ImportOutcome::Failed},
                                 {"lower", ImportOutcome::Failed},
                                 {"grade-conflict", ImportOutcome::Accepted}})
            run(pair.first, pair.second);
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
                                          kind == ImportKind::Ecad ? map : dir.path()}) {
                    r.input = input;
                    check(!c.start(r), "invalid input rejected before launching");
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
                      "optional report preserves material-map recovery");
                r.prefix = {script, MOCK_SCRIPT, record, "success"};
                waitFor(c, [&] {
                    check(c.start(r), "start optional exports");
                    check(!c.needsMaterialMapReselect(), "new run clears map missing state");
                });
                check(c.outcome() == ImportOutcome::Accepted && !c.needsMaterialMapReselect(),
                      "optional exports accept and reset recovery state");
                const auto args = recorded(record);
                const auto stage = QFileInfo(args.at(args.indexOf("-o") + 1)).absolutePath();
                QStringList wanted;
                if (kind == ImportKind::Assembly)
                    wanted = {"--assembly", r.input, "-o", stage + "/out.stack.json"};
                else
                    wanted = {r.input, "-o", stage + "/out.stack.json", "--material-map", map};
                if (kind == ImportKind::Assembly && !r.report.isEmpty())
                    wanted << "--report" << stage + "/defects.json";
                if (!r.polyset.isEmpty())
                    wanted << "--polyset" << stage + "/out.polyset";
                if (kind == ImportKind::Ecad && !r.report.isEmpty())
                    wanted << "--report" << stage + "/defects.json";
                if (r.acceptBTier)
                    wanted << "--accept-b-tier";
                check(args == wanted, "exact argv for every optional flag combination");
                check(QFile::exists(r.output) && (r.report.isEmpty() || QFile::exists(r.report)) &&
                          (r.polyset.isEmpty() || QFile::exists(r.polyset)),
                      "requested optional exports published");
                check(!QDir(stage).exists(), "optional staging removed");
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
        if (widgets) {
            auto filters = ImportSourceDialog::nameFilters();
            check(filters.contains("IPC-2581 / ODB++ (*.xml *.tgz *.zip)") &&
                      filters.contains("ODB++ (*.tgz *.zip)"),
                  "ECAD and ODB filters");
            for (const auto& filter : filters)
                check(!filter.contains("*.step") && !filter.contains("*.stp"),
                      "unsupported STEP formats absent from chooser");
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
