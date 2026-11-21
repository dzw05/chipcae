#include "CaseDocument.hpp"
#include "DefectsDialog.hpp"
#include "ImportController.hpp"
#include "ImportSourceDialog.hpp"
#include "MaterialMapMissingDialog.hpp"

#include <QAbstractProxyModel>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableView>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
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
// Frozen fixtures are read-only. Only temporary copies are passed to the child process.
QString reportFixture(const QString& sample) {
    auto root = QString(FIXTURE_DIR) + "/../../tests/contract/fixtures/import-report-1.0";
    if (!QDir(root).exists())
        root = qEnvironmentVariable("CHIPCAE_IMPORT_REPORT_FIXTURE_DIR");
    require(!root.isEmpty() && QDir(root).exists(),
            "set CHIPCAE_IMPORT_REPORT_FIXTURE_DIR to the frozen import-report/1.0 fixtures");
    return QDir(root).filePath(sample);
}
std::set<std::string> keys(const Json& value) {
    std::set<std::string> result;
    if (value.is_object())
        for (const auto& member : value.object_range())
            result.emplace(member.key());
    return result;
}
const std::set<std::string> defectKeys{"defect_id",
                                       "tier",
                                       "kind",
                                       "reason",
                                       "dimension",
                                       "size",
                                       "size_unit",
                                       "suggestion",
                                       "action",
                                       "rejection_reason",
                                       "material_ambiguous"};
void checkDefects(const Json& actual, const Json& expected) {
    check(actual.is_array() && actual.size() == expected.size(), "C2 exact defect count");
    if (!actual.is_array() || actual.size() != expected.size())
        return;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& a = actual[i];
        const auto& e = expected[i];
        check(a.is_object(), "C2 defect is object");
        for (const auto& key : defectKeys) {
            const bool present = a.contains(key);
            check(present, "C2 required defect key " + QString::fromStdString(key));
            if (!present)
                continue;
            if (key == "size") {
                check(e.at(key).is_null()
                          ? a.at(key).is_null()
                          : a.at(key).is_number() &&
                                std::abs(a.at(key).as<double>() - e.at(key).as<double>()) <= 1e-12,
                      "C2 size absolute tolerance 1e-12 or matching null");
            } else {
                check(a.at(key) == e.at(key),
                      "C2 exact defect field " + QString::fromStdString(key));
                if (key == "reason")
                    check(a.at(key).is_int64() || a.at(key).is_uint64(), "C2 integer reason");
                else if (key == "material_ambiguous")
                    check(a.at(key).is_bool(), "C2 boolean material_ambiguous");
                else
                    check(a.at(key).is_string(), "C2 string defect field");
            }
        }
    }
}
void checkMockReports(const Json& summary, const Json& sidecar, bool hasDefects, bool accepted) {
    const auto fixture =
        Json::parse(read(reportFixture("board_2layer") + "/import_report.json").toStdString());
    check(keys(summary) == std::set<std::string>{"schema_version", "code", "message", "defects"},
          "ECAD mock summary exact fixture keys");
    check(keys(sidecar) == keys(fixture), "ECAD mock sidecar exact fixture keys");
    check(sidecar.contains("schema_version") && sidecar.at("schema_version") == "import-report/1.0",
          "ECAD mock sidecar version");
    for (const auto* key : {"L", "t_min", "eps_weld", "tau_A", "tau_B", "delta_f"})
        check(sidecar.contains(key) && sidecar.at(key).is_number(), "mock numeric threshold");
    if (sidecar.contains("tier_counts")) {
        const auto& counts = sidecar.at("tier_counts");
        check(keys(counts) == std::set<std::string>{"Weld", "A", "B", "C"}, "mock tier count keys");
        for (const auto* tier : {"Weld", "A", "B", "C"})
            check(counts.contains(tier) &&
                      (counts.at(tier).is_int64() || counts.at(tier).is_uint64()) &&
                      counts.at(tier).as<int>() >= 0,
                  "mock nonnegative integer tier count");
    }
    check(sidecar.contains("b_tier_accepted") &&
              (accepted ? sidecar.at("b_tier_accepted") == "all"
                        : sidecar.at("b_tier_accepted").is_null()),
          "mock accepted B state is all or null");
    check(sidecar.contains("defects") && sidecar.at("defects").is_array(),
          "mock sidecar defects array");
    if (!sidecar.contains("defects") || !sidecar.at("defects").is_array())
        return;
    const auto& defects = sidecar.at("defects");
    check(hasDefects ? !defects.empty() : defects.empty(), "mock sidecar has scenario defects");
    std::set<std::string> ids;
    for (const auto& d : defects.array_range()) {
        check(keys(d) == defectKeys, "mock defect exact eleven fixture keys");
        if (keys(d) != defectKeys)
            continue;
        check(d.at("defect_id").is_string() &&
                  ids.insert(d.at("defect_id").as<std::string>()).second,
              "mock unique defect IDs");
        check(d.at("tier") == "Weld" || d.at("tier") == "A" || d.at("tier") == "B" ||
                  d.at("tier") == "C",
              "mock tier enum");
        check(d.at("size_unit") == "1" || d.at("size_unit") == "m", "mock default SI/count units");
        check(d.at("size").is_number() || d.at("size").is_null(), "mock size number/null");
        check(d.at("material_ambiguous").is_bool() &&
                  (d.at("reason").is_int64() || d.at("reason").is_uint64()),
              "mock defect types");
        for (const auto* key : {"kind", "dimension", "suggestion", "action", "rejection_reason"})
            check(d.at(key).is_string(), "mock defect string type");
    }
}
QStringList tableRows(QDialog& dialog) {
    QStringList rows;
    for (auto* table : dialog.findChildren<QTableView*>()) {
        if (!table->isVisible() || !table->model())
            continue;
        const auto* model = table->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            QStringList cells;
            for (int col = 0; col < model->columnCount(); ++col) {
                const auto index = model->index(row, col);
                cells << model->data(index).toString();
                check(!(model->flags(index) & Qt::ItemIsEditable), "defect table is read-only");
            }
            rows << cells.join(" | ");
        }
    }
    return rows;
}
bool hasUnit(const QString& row, const QString& unit) {
    const QRegularExpression token("(?:^|[\\s|0-9—])" + QRegularExpression::escape(unit) +
                                   "(?=$|[\\s|])");
    return token.match(row).hasMatch();
}
QString dialogText(QDialog& dialog) {
    QStringList texts = tableRows(dialog);
    for (auto* text : dialog.findChildren<QPlainTextEdit*>())
        if (text->isVisible())
            texts << text->toPlainText();
    for (auto* text : dialog.findChildren<QTextEdit*>())
        if (text->isVisible())
            texts << text->toPlainText();
    for (auto* label : dialog.findChildren<QLabel*>())
        if (label->isVisible())
            texts << label->text();
    return texts.join('\n');
}
struct ReportReplay {
    QTemporaryDir temp;
    QByteArray summary, sidecar;
    int exitCode = 0, serial = 0;
    QString sidecarMode = "file";
    ImportKind kind = ImportKind::Ecad;
    QString outputPath, summaryPath, sidecarPath;
    ImportRequest lastRequest;
    explicit ReportReplay(const QString& sample) {
        if (sample == "assembly_parts_synthetic")
            kind = ImportKind::Assembly;
        require(temp.isValid(), "report replay temporary directory");
        const auto source = reportFixture(sample);
        for (const auto* name : {"defects.json", "import_report.json", "exit_code"})
            require(QFile::copy(source + '/' + name, temp.filePath(name)),
                    "copy frozen fixture to temp");
        summary = read(temp.filePath("defects.json"));
        sidecar = read(temp.filePath("import_report.json"));
        exitCode = read(temp.filePath("exit_code")).trimmed().toInt();
        write(temp.filePath("input.xml"), "synthetic replay");
        write(temp.filePath("map.json"), "{}");
        write(temp.filePath("replay.py"), R"PY(import json,pathlib,runpy,shutil,sys
root,mock,rc,mode=sys.argv[1:5]
a=sys.argv[5:]
def value(flag): return pathlib.Path(a[a.index(flag)+1])
root=pathlib.Path(root)
out=value('-o'); summary=value('--report'); sidecar=out.parent/'import_report.json'
out.write_text(json.dumps(runpy.run_path(mock)['STACK']))
shutil.copyfile(root/'defects.json',summary)
if mode=='file': shutil.copyfile(root/'import_report.json',sidecar)
if mode=='directory': sidecar.mkdir()
(root/'paths.json').write_text(json.dumps([str(out),str(summary),str(sidecar)]))
sys.exit(int(rc))
)PY");
    }
    void run(ImportController& controller) {
        write(temp.filePath("defects.json"), summary);
        write(temp.filePath("import_report.json"), sidecar);
        ImportRequest r;
        r.kind = kind;
        r.input = temp.filePath("input.xml");
        r.output = temp.filePath(QString("result-%1.stack.json").arg(++serial));
        r.report = r.output + ".summary.json";
        r.materialMap = temp.filePath("map.json");
        r.program = PYTHON_EXECUTABLE;
        r.prefix = {temp.filePath("replay.py"),
                    temp.path(),
                    MOCK_SCRIPT,
                    QString::number(exitCode),
                    sidecarMode};
        lastRequest = r;
        waitFor(controller, [&] { check(controller.start(r), "start frozen report replay"); });
        const auto paths = recorded(temp.filePath("paths.json"));
        require(paths.size() == 3, "replay records all staged paths");
        outputPath = paths[0];
        summaryPath = paths[1];
        sidecarPath = paths[2];
        check(controller.exitCode() == exitCode, "replay preserves fixture exit code");
        check(controller.reportBytes() == summary, "reportBytes remains byte-exact --report");
    }
};
void reportAcceptance() {
    auto test = [](const QString& name, const std::function<void()>& body) {
        const auto before = failures;
        try {
            body();
        } catch (const std::exception& e) {
            check(false, name + ": " + e.what());
        }
        std::cout << (failures == before ? "PASS: " : "FAIL CASE: ") << name.toStdString() << '\n';
    };
    test("C2 frozen three B defects survive staging cleanup", [&] {
        ReportReplay f("confirm_b_tier_synthetic");
        CaseDocument doc;
        require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "C2 baseline");
        ImportController c(doc);
        DefectsDialog dialog(c);
        f.run(c);
        check(c.exitCode() == 6 && c.outcome() == ImportOutcome::NeedsConfirmation,
              "C2 exit 6 NeedsConfirmation");
        check(Json::parse(c.reportBytes().toStdString()).at("defects").empty(),
              "C2 summary defects intentionally empty");
        check(c.importReportBytes() == f.sidecar, "C2 sidecar byte equality after child exits");
        check(!QFile::exists(f.sidecarPath), "valid sidecar staging has been released");
        const auto actual = c.importReport();
        const auto expected = Json::parse(f.sidecar.toStdString());
        check(actual.contains("schema_version") &&
                  actual.at("schema_version") == "import-report/1.0",
              "C2 exact frozen version");
        check(actual == expected, "C2 parsed report deep equality");
        checkDefects(actual.contains("defects") ? actual.at("defects") : Json::array(),
                     expected.at("defects"));
        dialog.show();
        QApplication::processEvents();
        const auto rows = tableRows(dialog);
        check(rows.size() == 3, "C2 visible table has three defect rows");
        for (int i = 0; i < 3 && i < rows.size(); ++i) {
            const auto id = QString("board/Mystery%1/2").arg(QChar('A' + i));
            check(rows[i].split(" | ").contains(id) && rows[i].split(" | ").contains("B") &&
                      rows[i].contains("1") && rows[i].contains("计数") &&
                      rows[i].contains("IMPORT_UNSUPPORTED_ELEMENT") &&
                      rows[i].contains("confirm omitted metadata, components or text") &&
                      rows[i].contains("tier B defect requires explicit confirmation"),
                  "C2 row displays ID, tier, kind, both descriptions and count unit");
        }
        auto* confirm = button(dialog, "确认并继续");
        check(confirm && confirm->isEnabled(), "C2 confirmation stays enabled");
    });
    test("success fixture preserves both reports and empty defect meaning", [&] {
        ReportReplay f("board_2layer");
        CaseDocument doc;
        require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "success baseline");
        ImportController c(doc);
        DefectsDialog dialog(c);
        f.run(c);
        check(c.outcome() == ImportOutcome::Accepted, "success fixture accepted");
        const auto summary = Json::parse(c.reportBytes().toStdString());
        check(summary.at("schema_version") == "import-report/1.0" && summary.at("code") == "OK",
              "summary exact schema_version and OK");
        const auto expected = Json::parse(f.sidecar.toStdString());
        const auto actual = c.importReport();
        check(keys(actual) == keys(expected),
              "success all common required keys, no fabricated assembly keys");
        check(actual == expected && c.importReportBytes() == f.sidecar,
              "success retained sidecar object and raw bytes");
        dialog.show();
        QApplication::processEvents();
        check(dialogText(dialog).contains("无缺陷") &&
                  !dialogText(dialog).contains("无旁路缺陷明细"),
              "legal empty defects display no defects");
    });
    test("P5 unit/null matrix and stable severity grouping", [&] {
        ReportReplay f("confirm_b_tier_synthetic");
        auto j = QJsonDocument::fromJson(f.sidecar).object();
        auto prototype = j["defects"].toArray()[0].toObject();
        QJsonArray defects;
        const QStringList units{"1", "m", "m2", "m3"};
        const QStringList display{"计数", "m", "m²", "m³"};
        const QStringList tiers{"A", "Weld", "C", "B", "C", "B", "A", "Weld"};
        for (int i = 0; i < 8; ++i) {
            auto d = prototype;
            d["defect_id"] = QString("unit-row-%1").arg(i);
            d["tier"] = tiers[i];
            d["size_unit"] = units[i % 4];
            d["size"] = i < 4 ? QJsonValue(0.125) : QJsonValue(QJsonValue::Null);
            defects.append(d);
        }
        j["defects"] = defects;
        f.sidecar = QJsonDocument(j).toJson();
        CaseDocument doc;
        ImportController c(doc);
        DefectsDialog dialog(c);
        f.run(c);
        dialog.show();
        QApplication::processEvents();
        const auto rows = tableRows(dialog);
        const QList<int> order{2, 4, 3, 5, 0, 6, 1, 7};
        check(rows.size() == 8, "unit matrix has eight visible rows");
        for (int row = 0; row < rows.size() && row < order.size(); ++row) {
            const auto i = order[row];
            check(rows[row].split(" | ").contains(QString("unit-row-%1").arg(i)) &&
                      rows[row].split(" | ").contains(tiers[i]),
                  "stable C B A Weld group order retains same-tier report order");
            check(hasUnit(rows[row], display[i % 4]) && rows[row].contains(i < 4 ? "0.125" : "—"),
                  "size is SI unchanged or em dash with its unit");
        }
        const auto actual = c.importReport();
        checkDefects(actual.contains("defects") ? actual.at("defects") : Json::array(),
                     Json::parse(f.sidecar.toStdString()).at("defects"));
    });
    test("duplicate defect IDs retain both rows with diagnostic", [&] {
        ReportReplay f("confirm_b_tier_synthetic");
        auto j = QJsonDocument::fromJson(f.sidecar).object();
        auto d = j["defects"].toArray()[0];
        j["defects"] = QJsonArray{d, d};
        f.sidecar = QJsonDocument(j).toJson();
        CaseDocument doc;
        ImportController c(doc);
        DefectsDialog dialog(c);
        f.run(c);
        dialog.show();
        QApplication::processEvents();
        const auto rows = tableRows(dialog);
        check(rows.size() == 2 && rows[0].split(" | ").contains("board/MysteryA/2") &&
                  rows[1].split(" | ").contains("board/MysteryA/2"),
              "duplicate IDs are not dropped or rewritten");
        check((c.error() + dialogText(dialog)).contains("报告内 defect_id 重复"),
              "duplicate ID diagnostic");
    });
    for (bool summary : {true, false}) {
        for (const auto& version : {QString("missing"),
                                    QString("import-report/2.0"),
                                    QString("import-report/1.0 "),
                                    QString("number"),
                                    QString("Import-report/1.0"),
                                    QString("import-report/1.-1"),
                                    QString("import-report/1.1x")}) {
            test(QString("P1 %1 version %2").arg(summary ? "summary" : "sidecar", version), [&] {
                ReportReplay f("board_2layer");
                auto j = QJsonDocument::fromJson(summary ? f.summary : f.sidecar).object();
                if (version == "missing")
                    j.remove("schema_version");
                else
                    j["schema_version"] = version == "number" ? QJsonValue(1) : QJsonValue(version);
                (summary ? f.summary : f.sidecar) = QJsonDocument(j).toJson();
                CaseDocument doc;
                require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "version baseline");
                ImportController c(doc);
                DefectsDialog dialog(c);
                f.run(c);
                dialog.show();
                QApplication::processEvents();
                check(c.outcome() == (summary ? ImportOutcome::Failed : ImportOutcome::Accepted),
                      "P1 summary rejects acceptance; sidecar does not change matched outcome");
                check(c.importReportBytes().isEmpty() == !summary,
                      "P1 rejected sidecar bytes empty; independent valid sidecar retained");
                check(c.importReport().empty() == !summary, "P1 rejected sidecar object empty");
                const auto diagnostic = c.error() + dialogText(dialog);
                check(diagnostic.contains("导入报告版本不受支持") &&
                          diagnostic.contains(summary ? f.summaryPath : f.sidecarPath),
                      "P1 diagnostic names unsupported version and source path");
            });
        }
    }
    test("P1 higher minor versions and additive fields remain readable", [&] {
        ReportReplay f("board_2layer");
        for (auto* bytes : {&f.summary, &f.sidecar}) {
            auto j = QJsonDocument::fromJson(*bytes).object();
            j["schema_version"] = "import-report/1.27";
            j["future"] = QJsonObject{{"extra", true}};
            *bytes = QJsonDocument(j).toJson();
        }
        CaseDocument doc;
        require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "higher minor baseline");
        ImportController c(doc);
        f.run(c);
        check(c.outcome() == ImportOutcome::Accepted && c.error().isEmpty(),
              "higher minor accepted");
        check(c.importReportBytes() == f.sidecar &&
                  c.importReport() == Json::parse(f.sidecar.toStdString()),
              "higher minor sidecar retains additive fields");
    });
    for (bool summary : {true, false}) {
        test(summary ? "P4 malformed summary" : "P4 malformed sidecar", [&] {
            ReportReplay f("board_2layer");
            (summary ? f.summary : f.sidecar) = "{";
            CaseDocument doc;
            require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "syntax baseline");
            ImportController c(doc);
            DefectsDialog dialog(c);
            f.run(c);
            dialog.show();
            QApplication::processEvents();
            const auto path = summary ? f.summaryPath : f.sidecarPath;
            const auto diagnostic = c.error() + dialogText(dialog);
            check(diagnostic.contains(path) && diagnostic.contains("JSON", Qt::CaseInsensitive),
                  "P4 JSON syntax diagnostic includes original path");
            check(c.outcome() == (summary ? ImportOutcome::Failed : ImportOutcome::Accepted),
                  "P4 corrupt summary fails; corrupt sidecar preserves legal OK summary");
            check(QFile::exists(path) && read(path) == "{",
                  "P4 original malformed bytes remain readable");
            if (!summary)
                check(c.importReportBytes().isEmpty() && c.importReport().empty(),
                      "P4 invalid sidecar hidden");
        });
    }
    for (const auto& pair : {std::pair{0, "OK"},
                             {6, "IMPORT_CONFIRMATION_REQUIRED"},
                             {7, "IMPORT_REJECTED"},
                             {9, "MATERIAL_MAPPING_MISSING"}}) {
        test(QString("missing sidecar preserves pair %1").arg(pair.first), [&] {
            ReportReplay f("board_2layer");
            auto j = QJsonDocument::fromJson(f.summary).object();
            j["code"] = pair.second;
            f.summary = QJsonDocument(j).toJson();
            f.exitCode = pair.first;
            f.sidecarMode = "missing";
            CaseDocument doc;
            require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "missing sidecar baseline");
            ImportController c(doc);
            DefectsDialog dialog(c);
            f.run(c);
            const auto outcome = pair.first == 0   ? ImportOutcome::Accepted
                                 : pair.first == 6 ? ImportOutcome::NeedsConfirmation
                                                   : ImportOutcome::Rejected;
            check(c.outcome() == outcome, "missing sidecar preserves four-pair result");
            check(c.importReportBytes().isEmpty() && c.importReport().empty(),
                  "missing sidecar empty accessors");
            dialog.show();
            QApplication::processEvents();
            check(dialogText(dialog).contains("无旁路缺陷明细"),
                  "missing sidecar is distinct from empty defects");
        });
    }
    test("sidecar read failure identifies path without changing outcome", [&] {
        ReportReplay f("confirm_b_tier_synthetic");
        f.sidecarMode = "directory";
        CaseDocument doc;
        ImportController c(doc);
        DefectsDialog dialog(c);
        f.run(c);
        dialog.show();
        QApplication::processEvents();
        check(c.outcome() == ImportOutcome::NeedsConfirmation,
              "unreadable sidecar preserves summary pair");
        check(c.importReportBytes().isEmpty() && c.importReport().empty(),
              "read failure empty accessors");
        check((c.error() + dialogText(dialog)).contains(f.sidecarPath),
              "read failure identifies sidecar path");
    });
    test("restart and local failure clear prior defects parts and diagnostics", [&] {
        ReportReplay f("confirm_b_tier_synthetic");
        auto j = QJsonDocument::fromJson(f.sidecar).object();
        auto defects = j["defects"].toArray();
        defects.append(defects[0]);
        j["defects"] = defects;
        const auto assembly =
            QJsonDocument::fromJson(
                read(reportFixture("assembly_parts_synthetic") + "/import_report.json"))
                .object();
        j["parts"] = assembly["parts"];
        j["welded_interfaces"] = 0;
        f.sidecar = QJsonDocument(j).toJson();
        CaseDocument doc;
        ImportController c(doc);
        DefectsDialog dialog(c);
        f.run(c);
        check(!c.importReport().empty() &&
                  (c.error() + dialogText(dialog)).contains("报告内 defect_id 重复"),
              "reset test starts with defects parts and duplicate diagnostic");
        f.sidecarMode = "missing";
        f.run(c);
        dialog.show();
        QApplication::processEvents();
        check(c.importReportBytes().isEmpty() && c.importReport().empty() &&
                  tableRows(dialog).isEmpty() &&
                  !(c.error() + dialogText(dialog)).contains("报告内 defect_id 重复") &&
                  !dialogText(dialog).contains("board/Mystery"),
              "next import has no stale sidecar state");
        f.sidecarMode = "file";
        f.run(c);
        auto invalid = f.lastRequest;
        invalid.materialMap.clear();
        check(!c.start(invalid), "local failure after valid sidecar");
        check(c.importReportBytes().isEmpty() && c.importReport().empty() &&
                  !c.error().contains("报告内 defect_id 重复"),
              "local start failure clears retained state");
    });
    // Each mutation has an otherwise valid, versioned report. A generic parse failure
    // cannot accidentally satisfy these checks by rejecting every sidecar.
    for (const auto& mutation : {"missing-L",
                                 "number",
                                 "counts",
                                 "accepted",
                                 "defects",
                                 "tier",
                                 "unit",
                                 "reason",
                                 "size",
                                 "bool",
                                 "missing-defect-key",
                                 "empty-defect-id"}) {
        test(QString("sidecar rejects field shape %1").arg(mutation), [&] {
            ReportReplay f("confirm_b_tier_synthetic");
            auto j = QJsonDocument::fromJson(f.sidecar).object();
            auto defects = j["defects"].toArray();
            auto d = defects[0].toObject();
            const QString mode = mutation;
            if (mode == "missing-L")
                j.remove("L");
            else if (mode == "number")
                j["L"] = "zero";
            else if (mode == "counts")
                j["tier_counts"] = QJsonObject{{"Weld", 0}, {"A", 0}, {"B", true}, {"C", 0}};
            else if (mode == "accepted")
                j["b_tier_accepted"] = true;
            else if (mode == "defects")
                j["defects"] = QJsonObject{};
            else {
                if (mode == "tier")
                    d["tier"] = "b";
                if (mode == "unit")
                    d["size_unit"] = "M";
                if (mode == "reason")
                    d["reason"] = 0.5;
                if (mode == "size")
                    d["size"] = "large";
                if (mode == "bool")
                    d["material_ambiguous"] = "false";
                if (mode == "missing-defect-key")
                    d.remove("suggestion");
                if (mode == "empty-defect-id")
                    d["defect_id"] = "";
                defects[0] = d;
                j["defects"] = defects;
            }
            f.sidecar = QJsonDocument(j).toJson();
            CaseDocument doc;
            ImportController c(doc);
            DefectsDialog dialog(c);
            f.run(c);
            dialog.show();
            QApplication::processEvents();
            check(c.outcome() == ImportOutcome::NeedsConfirmation,
                  "invalid sidecar does not regrade summary");
            check(c.importReportBytes().isEmpty() && c.importReport().empty(),
                  "invalid fields rejected");
            const auto diagnostic = c.error() + dialogText(dialog);
            check(diagnostic.contains(f.sidecarPath) && !diagnostic.contains("版本不受支持") &&
                      (diagnostic.contains("字段") ||
                       diagnostic.contains("field", Qt::CaseInsensitive) ||
                       diagnostic.contains("structure", Qt::CaseInsensitive)),
                  "field shape diagnostic differs from version/syntax");
        });
    }
    for (const auto* mutation : {"parts-array",
                                 "split-array",
                                 "rewrite-object",
                                 "material-object",
                                 "weld-missing",
                                 "weld-bool",
                                 "weld-fraction",
                                 "weld-negative"}) {
        test(QString("assembly sidecar rejects %1").arg(mutation), [&] {
            ReportReplay f("assembly_parts_synthetic");
            auto j = QJsonDocument::fromJson(f.sidecar).object();
            auto parts = j["parts"].toObject();
            const QString mode = mutation;
            if (mode == "split-array")
                parts["split_layers"] = QJsonArray{};
            if (mode == "rewrite-object")
                parts["rewrite_targets"] = QJsonObject{};
            if (mode == "material-object")
                parts["material_approx"] = QJsonObject{};
            j["parts"] = mode == "parts-array" ? QJsonValue(QJsonArray{}) : QJsonValue(parts);
            if (mode == "weld-missing")
                j.remove("welded_interfaces");
            if (mode == "weld-bool")
                j["welded_interfaces"] = true;
            if (mode == "weld-fraction")
                j["welded_interfaces"] = 0.5;
            if (mode == "weld-negative")
                j["welded_interfaces"] = -1;
            f.sidecar = QJsonDocument(j).toJson();
            CaseDocument doc;
            require(doc.load(FIXTURE_DIR "/fcbga_demo.case.json"), "assembly field baseline");
            ImportController c(doc);
            DefectsDialog dialog(c);
            f.run(c);
            dialog.show();
            QApplication::processEvents();
            check(c.outcome() == ImportOutcome::Accepted,
                  "invalid assembly sidecar preserves OK summary");
            check(c.importReportBytes().isEmpty() && c.importReport().empty(),
                  "invalid assembly fields rejected");
            check((c.error() + dialogText(dialog)).contains(f.sidecarPath),
                  "invalid assembly fields identify path");
        });
    }
    for (const auto& pair : {std::pair{13, "CUT_UNSUPPORTED"}, {14, "INPUT_INVALID"}}) {
        test(QString("protocol failure includes %1").arg(pair.second), [&] {
            ReportReplay f("board_2layer");
            auto j = QJsonDocument::fromJson(f.summary).object();
            j["code"] = pair.second;
            f.summary = QJsonDocument(j).toJson();
            f.exitCode = pair.first;
            CaseDocument doc;
            ImportController c(doc);
            f.run(c);
            check(c.outcome() == ImportOutcome::Failed && c.error().contains(pair.second),
                  "13/14 error contains protocol code");
        });
    }
}

// Non-native dialogs are driven through their actual widgets, with an independent timeout.
QString pick(const std::function<QString()>& picker,
             const QString& path,
             bool cancel,
             bool map,
             bool directory = false,
             const std::function<void()>& beforeCancel = {}) {
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
            if (!map) {
                check(!dialog->nameFilters().join(' ').contains("*.step") &&
                          !dialog->nameFilters().join(' ').contains("*.stp"),
                      "STEP disclosure is absent from selectable filters");
                bool disclosed = false;
                for (auto* label : dialog->findChildren<QLabel*>())
                    disclosed |=
                        label->text().contains("B 档") && label->text().contains("(*.step *.stp)");
                check(disclosed, "disabled STEP disclosure remains visible");
            }
            driver.stop();
            if (cancel) {
                if (beforeCancel)
                    beforeCancel();
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
                    if (!map &&
                        QStringList{"step", "stp"}.contains(QFileInfo(path).suffix().toLower())) {
                        auto* model = dialog->proxyModel();
                        auto* files = qobject_cast<QFileSystemModel*>(model->sourceModel());
                        require(files, "source picker filesystem model");
                        const auto index = model->mapFromSource(files->index(path));
                        check(!index.isValid() || !(model->flags(index) &
                                                    (Qt::ItemIsEnabled | Qt::ItemIsSelectable)),
                              "STEP file disabled under STEP filter");
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
    check(watchdog.isActive(), "picker returns before watchdog closes dialogs");
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
        {
            CaseDocument document;
            require(document.load(FIXTURE_DIR "/fcbga_demo.case.json"),
                    "atomic replacement baseline");
            const auto before = document.stackJson();
            int changes = 0, dirtyChanges = 0;
            QObject::connect(&document, &CaseDocument::changed, [&] { ++changes; });
            QObject::connect(&document, &CaseDocument::dirtyChanged, [&] { ++dirtyChanges; });
            check(!document.replaceImportedStack(Json::object()) &&
                      document.stackJson() == before && !document.isDirty() && changes == 0 &&
                      dirtyChanges == 0,
                  "failed document replacement preserves stack, dirty state and notifications");
        }
        reportAcceptance();
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
out=value('-o'); report=value('--report') or str(pathlib.Path(out).parent/'defects.json'); poly=value('--polyset')
pathlib.Path(out).write_text(json.dumps({} if mode=='invalid-stack' else stack))
if poly: pathlib.Path(poly).write_text('polyset')
# Assembly has no contracted default report path.
if '--assembly' in a and '--report' not in a: sys.exit(0)
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
    payload={'schema_version':'import-report/1.0','code':code,'message':'fixture message','defects':[] if code=='OK' else [{'code':'MOCK_GAP'}]}
    if mode in ('grade-conflict','crash-map','cancel-report'): payload['grade']='C'
    if mode=='confirm': payload['grade']='B'
    if mode.startswith('bad-'):
        payload['code']='MATERIAL_MAPPING_MISSING'; payload['grade']='B'; rc=9
        if mode=='bad-code-type': payload['code']=9
        if mode=='bad-message-missing': del payload['message']
        if mode=='bad-message-type': payload['message']=123
        if mode=='bad-defects-missing': del payload['defects']
        if mode=='bad-defects-type': payload['defects']={}
    pathlib.Path(report).write_text('{' if mode=='corrupt' else json.dumps(payload))
if mode=='cancel-report':
    while True: time.sleep(1)
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
                        // ECAD summary defects may be empty; details belong to the sidecar.
                        check(j.contains("schema_version") &&
                                  j.at("schema_version") == "import-report/1.0",
                              "mock summary schema_version " + m);
                        if (m == "success" || m == "confirm" || m == "reject") {
                            const auto sidecar =
                                QFileInfo(output).dir().filePath("import_report.json");
                            check(QFile::exists(sidecar), "mock writes sidecar " + m);
                            if (QFile::exists(sidecar))
                                checkMockReports(j,
                                                 Json::parse(read(sidecar).toStdString()),
                                                 confirm || reject,
                                                 acceptBTier);
                        }
                    } catch (const std::exception& e) {
                        if (m != "corrupt")
                            check(false, "mock invalid JSON or field shape " + m + ": " + e.what());
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
            DefectsDialog mockDialog(controller);
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
            mockDialog.show();
            QApplication::processEvents();
            const auto rows = tableRows(mockDialog);
            check(!rows.empty() &&
                      (hasUnit(rows.join(" | "), "计数") || hasUnit(rows.join(" | "), "m")),
                  "shipped confirmation mock displays sidecar rows and units");
            waitFor(controller,
                    [&] { check(controller.restartAccepted(), "restart shipped mock"); });
            check(controller.outcome() == ImportOutcome::Accepted &&
                      controller.reportCode() == "OK" && controller.exitCode() == 0 &&
                      document.stackJson() != before && QFile::exists(request.output),
                  "shipped mock accepts and publishes confirmed rerun");
            const auto acceptedReport = controller.importReport();
            const auto successFixture = Json::parse(
                read(reportFixture("accept_b_tier") + "/import_report.json").toStdString());
            check(keys(acceptedReport) == keys(successFixture) &&
                      acceptedReport.contains("b_tier_accepted") &&
                      acceptedReport.at("b_tier_accepted") == "all",
                  "shipped accepted B report contains common keys and all acceptance state");
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
                if (mode == "hang" || mode == "cancel-report")
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
            if (mode != "hang" && mode != "crash-map" && mode != "cancel-report" &&
                mode != "launch-failure" && mode != "missing6" && !mode.startsWith("bad-") &&
                mode != "corrupt")
                check(c.reportCode() == code, "raw report code " + mode);
            if (mode == "success" || mode == "grade-conflict")
                check(c.grade() == (mode == "grade-conflict" ? "C" : ""),
                      "optional grade for display only");
            check(c.needsMaterialMapReselect() == (mode == "map-missing"), "reselection " + mode);
            if (mode == "crash-map" || mode == "cancel-report")
                check(c.reportCode().isEmpty() && c.grade().isEmpty() && !c.reportBytes().isEmpty(),
                      "abnormal run retains diagnostics without result fields");
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
                if (mode == "corrupt") {
                    const auto reportIndex = actual.indexOf("--report");
                    require(reportIndex >= 0 && reportIndex + 1 < actual.size(),
                            "malformed report path recorded");
                    const auto original = actual[reportIndex + 1];
                    check(QFile::exists(original) && read(original) == "{" &&
                              c.error().contains(original),
                          "malformed original remains readable until the next import");
                    // P4 retains syntax failures through inspection; the next start
                    // releases that directory even if local validation rejects the request.
                    auto next = r;
                    next.output.clear();
                    check(!c.start(next), "next local start releases malformed staging");
                    check(!QFile::exists(original), "next import removes malformed original");
                }
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
                                 {"cancel-report", ImportOutcome::Failed},
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
            const auto marker = dir.filePath("legacy-child-started");
            const auto markerScript = dir.filePath("legacy-marker.py");
            write(markerScript, "import pathlib,sys\npathlib.Path(sys.argv[1]).touch()\n");
            int legacyFinished = 0;
            const auto legacyConnection =
                QObject::connect(&c, &ImportController::finished, [&] { ++legacyFinished; });
            check(!c.start(r.input, r.output, {}, {}, PYTHON_EXECUTABLE, {markerScript, marker}),
                  "legacy overload rejects without map");
            QEventLoop settle;
            QTimer::singleShot(100, &settle, &QEventLoop::quit);
            settle.exec();
            check(c.error() == "material map required" && !c.busy() &&
                      c.outcome() == ImportOutcome::Failed && legacyFinished == 0 &&
                      !QFile::exists(marker) && !QFile::exists(r.output) &&
                      d.stackJson() == before && !d.isDirty(),
                  "legacy exact error, idle state, no child or document/output side effects");
            QObject::disconnect(legacyConnection);
            int localFinished = 0;
            auto localConnection =
                QObject::connect(&c, &ImportController::finished, [&] { ++localFinished; });
            for (int emptyPaths = 0; emptyPaths < 16; ++emptyPaths) {
                auto emptyMap = r;
                if (emptyPaths & 1)
                    emptyMap.input.clear();
                if (emptyPaths & 2)
                    emptyMap.output.clear();
                if (!(emptyPaths & 4))
                    emptyMap.polyset = r.output;
                if (!(emptyPaths & 8))
                    emptyMap.report = r.output;
                check(!c.start(emptyMap) && !c.busy() && c.error() == "material map required" &&
                          c.outcome() == ImportOutcome::Failed && localFinished == 0 &&
                          !c.needsMaterialMapReselect() && d.stackJson() == before,
                      "empty map takes precedence over empty or colliding request paths");
            }
            QObject::disconnect(localConnection);
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
                check(!c.start(r.input, r.output) && c.error() == "material map required" &&
                          c.reportCode().isEmpty() && c.exitCode() == -1 &&
                          !c.needsMaterialMapReselect(),
                      "local legacy rejection invalidates prior code 9 result");
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
                check(args.contains("--report"),
                      "every import explicitly requests a private report regardless of export");
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
            const auto confirmationReport = c.reportBytes();
            const auto confirmationCode = c.reportCode();
            const auto confirmationGrade = c.grade();
            int confirmed = 0, finished = 0, defects = 0;
            QObject::connect(&c, &ImportController::userConfirmed, [&] {
                ++confirmed;
                check(!c.busy() && c.outcome() == ImportOutcome::NeedsConfirmation,
                      "failed rerun confirmation precedes startup attempt");
                check(!confirmationReport.isEmpty() && c.reportBytes() == confirmationReport &&
                          c.reportCode() == confirmationCode && c.grade() == confirmationGrade &&
                          c.exitCode() == 6,
                      "confirmation observers retain the complete prior B-tier report");
            });
            const auto checkFailedRestartReport = [&] {
                check(c.outcome() == ImportOutcome::Failed && !c.busy() && !c.error().isEmpty() &&
                          c.reportBytes().isEmpty() && c.reportCode().isEmpty() &&
                          c.grade().isEmpty() && c.exitCode() == -1 &&
                          !c.needsMaterialMapReselect(),
                      "failed restart observers see failure without stale confirmation report");
            };
            QObject::connect(&c, &ImportController::defectsReady, [&] {
                ++defects;
                checkFailedRestartReport();
            });
            QObject::connect(&c, &ImportController::finished, [&](bool replaced) {
                ++finished;
                check(!replaced, "failed restart completion does not replace");
                checkFailedRestartReport();
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
            check(confirmed == 1 && finished == 0,
                  "failed restart confirms intent without a process completion signal");
            check(defects == 1, "failed restart refreshes defects once");
            check(!c.restartAccepted(), "Failed outcome rejects another accepted restart");
            c.confirmDefects();
            check(confirmed == 1 && finished == 0 && defects == 1 &&
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
            bool parentStayedOpen = false;
            bool nestedPickerSeen = false;
            QTimer cancelParent;
            QObject::connect(&cancelParent, &QTimer::timeout, [&] {
                for (auto* w : QApplication::topLevelWidgets())
                    if (auto* d = dynamic_cast<MaterialMapMissingDialog*>(w); d && d->isVisible()) {
                        if (d->findChild<QFileDialog*>())
                            continue;
                        if (!nestedPickerSeen)
                            continue;
                        parentStayedOpen = true;
                        cancelParent.stop();
                        d->findChild<QDialogButtonBox*>()
                            ->button(QDialogButtonBox::Cancel)
                            ->click();
                    }
            });
            cancelParent.start(30);
            click.start(30);
            check(pick([&] { return MaterialMapMissingDialog::prompt(); },
                       {},
                       true,
                       true,
                       false,
                       [&] { nestedPickerSeen = true; })
                      .isEmpty(),
                  "missing-map prompt parent cancellation returns empty");
            check(parentStayedOpen, "cancelled nested picker keeps missing-map prompt open");
        }
    } catch (const std::exception& e) {
        check(false, QString("fixture exception: ") + e.what());
    }
    std::cerr << checks << " assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
