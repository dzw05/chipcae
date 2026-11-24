#include "AssemblyImport.hpp"
#include "CaseDocument.hpp"
#include "ImportController.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace chipcae::desktop;
static void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
static QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture/output");
    return file.readAll();
}
static void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "write temporary file");
    require(file.write(bytes) == bytes.size(), "write complete");
}
static QString reportFixture(const QString& sample) {
    auto root = QString(CASE_FIXTURE_DIR) + "/../../tests/contract/fixtures/import-report-1.0";
    if (!QDir(root).exists())
        root = qEnvironmentVariable("CHIPCAE_IMPORT_REPORT_FIXTURE_DIR");
    require(!root.isEmpty() && QDir(root).exists(),
            "set CHIPCAE_IMPORT_REPORT_FIXTURE_DIR to frozen fixtures");
    return QDir(root).filePath(sample);
}
static std::set<std::string> keys(const Json& value) {
    std::set<std::string> result;
    if (value.is_object())
        for (const auto& member : value.object_range())
            result.emplace(member.key());
    return result;
}
static const std::set<std::string> defectKeys{"defect_id",
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
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto test = [&](const char* name, const std::function<void()>& body) {
        try {
            body();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    const QString fixtures = ASSEMBLY_FIXTURE_DIR;
    test("schema detection", [&] {
        require(isAssemblyPath(fixtures + "/example.assembly.json"),
                "assembly schema must be accepted");
    });
    test("reject other roots", [&] {
        QTemporaryDir dir;
        int n = 0;
        for (const auto& bytes : {QByteArray("{}"),
                                  QByteArray(""),
                                  QByteArray("not json"),
                                  QByteArray("{\"schema\":\"chipcae.assembly/0.2\"}"),
                                  QByteArray("[]"),
                                  QByteArray("{\"schema\":42}")}) {
            auto path = dir.filePath(QString::number(n++) + ".json");
            write(path, bytes);
            require(!isAssemblyPath(path), "reject malformed or wrong schema");
        }
        require(!isAssemblyPath(dir.filePath("missing.json")), "reject unreadable file");
        require(!isAssemblyPath(QString(CASE_FIXTURE_DIR) + "/fcbga_demo.case.json"),
                "reject case");
        auto path = dir.filePath("schema only.json");
        write(path, "{\"schema\":\"chipcae.assembly/0.1\"}");
        require(isAssemblyPath(path), "detection must not perform full validation");
    });
    test("argv", [&] {
        const QString path = "input with spaces/assembly.json";
        require(assemblyArguments(path, "out stack.json", "report.json", "poly.json", true) ==
                    QStringList{"--assembly",
                                path,
                                "-o",
                                "out stack.json",
                                "--report",
                                "report.json",
                                "--polyset",
                                "poly.json",
                                "--accept-b-tier"},
                "exact named argv");
        require(assemblyArguments(path, "out") == QStringList{"--assembly", path, "-o", "out"},
                "omit optional argv");
        require(assemblyArguments(path, "out", {}, "poly") ==
                    QStringList{"--assembly", path, "-o", "out", "--polyset", "poly"},
                "independent polyset");
    });
    test("filter", [&] {
        require(assemblyNameFilters() == QStringList{"Assembly (chipcae.assembly) (*.json)"},
                "exact assembly filter");
    });
    auto run = [&](const QStringList& args) {
        QProcess process;
        process.start(PYTHON_EXECUTABLE, QStringList{MOCK_ASSEMBLY_SCRIPT} + args);
        require(process.waitForStarted(), "start python mock");
        require(process.waitForFinished(10000), "finish mock");
        require(process.exitStatus() == QProcess::NormalExit, "normal mock exit");
        return process.exitCode();
    };
    test("mock success and document replacement", [&] {
        QTemporaryDir dir;
        const auto output = dir.filePath("out.json"), report = dir.filePath("report.json");
        require(run({"--assembly",
                     fixtures + "/example.assembly.json",
                     "-o",
                     output,
                     "--report",
                     report}) == 0,
                "mock must import valid assembly (expected exit 0)");
        const auto object = QJsonDocument::fromJson(read(report)).object();
        require(object["code"] == "OK", "top-level OK summary");
        CaseDocument doc;
        require(doc.load(QString(CASE_FIXTURE_DIR) + "/fcbga_demo.case.json"), "load document");
        const auto before = doc.stackJson();
        auto stack = Json::parse(read(output).toStdString());
        require(doc.checkImportedStack(stack) && doc.replaceImportedStack(stack),
                "valid stack import and replacement");
        require(doc.stackJson() == stack && doc.stackJson() != before, "document updated");
        QStringList bodies;
        for (const auto& layer : stack.at("layers").array_range())
            if (layer.contains("regions"))
                for (const auto& region : layer.at("regions").array_range())
                    if (region.contains("body"))
                        bodies << QString::fromStdString(region.at("body").as<std::string>());
        require(bodies.contains("lid") && bodies.contains("cap") && bodies.contains("lid.bond"),
                "part and bond bodies");
        require(object["schema_version"] == "import-report/1.0", "assembly summary schema version");
        require(!object.contains("parts"), "assembly parts belong to sidecar, never summary array");
        const auto sidecar =
            QJsonDocument::fromJson(read(dir.filePath("import_report.json"))).object();
        require(sidecar["parts"].isObject(), "sidecar assembly parts object");
        const auto summary = sidecar["parts"].toObject();
        require(summary["split_layers"].isObject() && summary["rewrite_targets"].isArray() &&
                    summary["material_approx"].isArray(),
                "three production assembly summary fields");
        const auto parts = summary["material_approx"].toArray();
        bool lidApprox = false, capApprox = false;
        QStringList ids;
        for (const auto& entry : parts) {
            const auto part = entry.toObject();
            ids << part["id"].toString();
            if (part["id"] == "lid") {
                lidApprox = part["material_approx"].toBool();
                require(part["label"] == "近似材料", "production approximation label");
            }
            if (part["id"] == "cap")
                capApprox = part["material_approx"].toBool();
        }
        require(lidApprox && !capApprox, "explicit lid approximation and default exact cap");
        require(ids == QStringList{"lid"} || ids == QStringList{"lid", "cap"},
                "approximation entries preserve assembly order; exact cap may be omitted");
        const auto parsed = Json::parse(read(dir.filePath("import_report.json")).toStdString());
        require(parsed.contains("welded_interfaces") &&
                    (parsed.at("welded_interfaces").is_int64() ||
                     parsed.at("welded_interfaces").is_uint64()) &&
                    parsed.at("welded_interfaces").as<int>() == 0,
                "integer welded_interfaces zero");
    });
    for (const auto* fixture : {"negative-abs-path.assembly.json", "negative-dup-id.assembly.json"})
        test(fixture, [&] {
            QTemporaryDir dir;
            CaseDocument doc;
            require(doc.load(QString(CASE_FIXTURE_DIR) + "/fcbga_demo.case.json"), "load document");
            const auto before = doc.stackJson(), caseBefore = doc.toJson();
            auto output = dir.filePath("out.json"), report = dir.filePath("report.json");
            const QByteArray sentinel = QByteArray::fromStdString(before.to_string()) + "\n ";
            write(output, sentinel);
            const int code =
                run({"--assembly", fixtures + "/" + fixture, "-o", output, "--report", report});
            if (code == 0)
                doc.replaceImportedStack(Json::parse(read(output).toStdString()));
            require(read(output) == sentinel && doc.stackJson() == before &&
                        doc.toJson() == caseBefore,
                    "rejection preserves bytes and document");
            require(code == 7, "rejection must exit 7");
            const auto summary = Json::parse(read(report).toStdString());
            require(summary.at("code") == "IMPORT_REJECTED", "rejection report");
            require(summary.contains("schema_version") &&
                        summary.at("schema_version") == "import-report/1.0",
                    "rejection versioned summary");
        });
    test("mock path components", [&] {
        for (const auto* key : {"path", "material_map", "materials", "step"}) {
            for (const auto& path : {QString("parts/lid..v2.step"),
                                     QString("parts/../lid.step"),
                                     QString(R"(parts\..\lid.step)"),
                                     QString(R"(parts/..\lid.step)"),
                                     QString(R"(parts\../lid.step)")}) {
                QTemporaryDir dir;
                auto assembly =
                    QJsonDocument::fromJson(read(fixtures + "/example.assembly.json")).object();
                if (QString(key) == "materials")
                    assembly[key] = path;
                else if (QString(key) == "step") {
                    auto parts = assembly["parts"].toArray();
                    auto part = parts[0].toObject();
                    part[key] = path;
                    parts[0] = part;
                    assembly["parts"] = parts;
                } else {
                    auto board = assembly["board"].toObject();
                    board[key] = path;
                    assembly["board"] = board;
                }
                const auto input = dir.filePath("assembly.json");
                const auto output = dir.filePath("stack.json");
                write(input, QJsonDocument(assembly).toJson());
                write(output, "sentinel");
                const auto code = run({"--assembly", input, "-o", output});
                if (path.contains("lid..v2"))
                    require(code == 0 && read(output) != "sentinel",
                            "ordinary filename with dots imports");
                else
                    require(code == 7 && read(output) == "sentinel",
                            "either slash style traversal rejected without changing output");
            }
        }
    });
    test("mock required arguments and unreadable input", [&] {
        QTemporaryDir dir;
        for (const auto& args :
             {QStringList{},
              QStringList{"--assembly", fixtures + "/example.assembly.json"},
              QStringList{"-o", dir.filePath("out")},
              QStringList{"--assembly", dir.filePath("missing"), "-o", dir.filePath("out")}})
            require(run(args) == 1, "missing arguments/input exit 1");
        require(!QFile::exists(dir.filePath("out")), "no output for unreadable input");
    });
    test("C3 frozen assembly sidecar survives controller staging cleanup", [&] {
        QTemporaryDir dir;
        const auto source = reportFixture("assembly_parts_synthetic");
        for (const auto* name : {"defects.json", "import_report.json", "exit_code"})
            require(QFile::copy(source + '/' + name, dir.filePath(name)),
                    "temporary copy frozen fixture");
        const auto expectedBytes = read(dir.filePath("import_report.json"));
        const auto expected = Json::parse(expectedBytes.toStdString());
        const auto summaryBytes = read(dir.filePath("defects.json"));
        // Only process I/O is adapted: use the actual synthetic stack generator.
        write(dir.filePath("replay.py"), R"PY(import pathlib,runpy,shutil,sys
root,mock=sys.argv[1:3]; a=sys.argv[3:]; sys.argv=[mock]+a
rc=runpy.run_path(mock)['main']()
root=pathlib.Path(root)
shutil.copyfile(root/'defects.json',a[a.index('--report')+1])
sidecar=pathlib.Path(a[a.index('-o')+1]).parent/'import_report.json'
shutil.copyfile(root/'import_report.json',sidecar)
(root/'staged-path').write_text(str(sidecar))
sys.exit(int((root/'exit_code').read_text()))
)PY");
        CaseDocument doc;
        require(doc.load(QString(CASE_FIXTURE_DIR) + "/fcbga_demo.case.json"),
                "C3 document baseline");
        ImportController controller(doc);
        ImportRequest request;
        request.kind = ImportKind::Assembly;
        request.input = fixtures + "/example.assembly.json";
        request.output = dir.filePath("assembly.stack.json");
        request.report = dir.filePath("summary.json");
        request.program = PYTHON_EXECUTABLE;
        request.prefix = {dir.filePath("replay.py"), dir.path(), MOCK_ASSEMBLY_SCRIPT};
        QEventLoop loop;
        QObject::connect(&controller, &ImportController::finished, &loop, &QEventLoop::quit);
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(5000);
        require(controller.start(request), "C3 replay starts");
        if (controller.busy())
            loop.exec();
        require(!controller.busy() && controller.exitCode() == 0 &&
                    controller.outcome() == ImportOutcome::Accepted,
                "C3 completed Accepted");
        require(controller.reportBytes() == summaryBytes, "C3 summary raw bytes unchanged");
        require(!QFile::exists(QString::fromUtf8(read(dir.filePath("staged-path")))),
                "C3 staging released");
        require(controller.importReportBytes() == expectedBytes,
                "C3 raw sidecar retained byte-for-byte");
        const auto actual = controller.importReport();
        require(actual.contains("parts") && actual.at("parts").is_object(),
                "C3 sidecar parts object");
        for (const auto* key : {"split_layers", "rewrite_targets", "material_approx"})
            require(actual.at("parts").contains(key) &&
                        actual.at("parts").at(key) == expected.at("parts").at(key),
                    "C3 fixture parts deep equality including array order");
        require(actual.at("welded_interfaces").is_int64() ||
                    actual.at("welded_interfaces").is_uint64(),
                "C3 welded_interfaces integer");
        require(actual.at("welded_interfaces").as<int>() == 0, "C3 welded_interfaces zero");
        require(actual == expected, "C3 entire parsed frozen sidecar");
    });
    for (const auto* input : {"example.assembly.json", "negative-dup-id.assembly.json"})
        test(input, [&] {
            QTemporaryDir dir;
            const bool reject = QString(input).startsWith("negative");
            const auto report = dir.filePath("summary.json");
            require(run({"--assembly",
                         fixtures + '/' + input,
                         "-o",
                         dir.filePath("out.json"),
                         "--report",
                         report}) == (reject ? 7 : 0),
                    "mock assembly success/rejection exit");
            const auto summary = Json::parse(read(report).toStdString());
            require(QFile::exists(dir.filePath("import_report.json")),
                    "assembly mock must emit import_report.json sidecar");
            const auto sidecar =
                Json::parse(read(dir.filePath("import_report.json")).toStdString());
            const auto expected =
                Json::parse(read(reportFixture("assembly_parts_synthetic") + "/import_report.json")
                                .toStdString());
            require(keys(summary) ==
                        std::set<std::string>{"schema_version", "code", "message", "defects"},
                    "assembly mock summary exact fixture keys");
            require(keys(sidecar) == keys(expected), "assembly mock sidecar exact fixture keys");
            require(summary.at("schema_version") == "import-report/1.0" &&
                        sidecar.at("schema_version") == "import-report/1.0",
                    "both mock versions");
            require(keys(sidecar.at("parts")) == keys(expected.at("parts")),
                    "assembly parts exact subkeys");
            require(sidecar.at("parts").at("split_layers").is_object() &&
                        sidecar.at("parts").at("rewrite_targets").is_array() &&
                        sidecar.at("parts").at("material_approx").is_array(),
                    "assembly parts types");
            for (const auto& entry : sidecar.at("parts").at("material_approx").array_range()) {
                require(entry.at("id").is_string() && entry.at("material_approx").is_bool(),
                        "assembly approximation id/boolean types");
                require(keys(entry) == std::set<std::string>{"id", "material_approx", "label"} ||
                            keys(entry) == std::set<std::string>{"id", "material_approx"},
                        "assembly approximation production keys");
                if (entry.at("material_approx").as<bool>())
                    require(entry.contains("label") && entry.at("label") == "近似材料",
                            "approximation production label");
            }
            require(sidecar.at("welded_interfaces").is_int64() ||
                        sidecar.at("welded_interfaces").is_uint64(),
                    "mock welded_interfaces integer");
            require(sidecar.at("welded_interfaces").as<int>() == 0, "mock no welded interfaces");
            for (const auto* key : {"L", "t_min", "eps_weld", "tau_A", "tau_B", "delta_f"})
                require(sidecar.at(key).is_number(), "assembly numeric threshold");
            require(keys(sidecar.at("tier_counts")) == std::set<std::string>{"Weld", "A", "B", "C"},
                    "assembly tier count keys");
            for (const auto* tier : {"Weld", "A", "B", "C"}) {
                const auto& count = sidecar.at("tier_counts").at(tier);
                require((count.is_int64() || count.is_uint64()) && count.as<int>() >= 0,
                        "assembly integer tier count");
            }
            require(sidecar.at("b_tier_accepted").is_null(), "assembly unaccepted state null");
            const auto& defects = sidecar.at("defects");
            require(defects.is_array() && (reject ? !defects.empty() : defects.empty()),
                    "assembly scenario defects");
            for (const auto& d : defects.array_range())
                require(keys(d) == defectKeys, "assembly defect eleven keys");
            require(summary.at("defects").is_array(), "assembly summary defects array");
            if (!summary.at("defects").empty())
                require(summary.at("defects") == defects,
                        "assembly summary uses same production defects");
        });
    return failures ? 1 : 0;
}
