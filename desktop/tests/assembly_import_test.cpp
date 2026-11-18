#include "AssemblyImport.hpp"
#include "CaseDocument.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
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
        require(object["code"] == "OK" && object["parts"].isArray(), "top-level OK parts report");
        const auto parts = object["parts"].toArray();
        require(parts.size() == 2 && parts[0].toObject()["id"] == "lid" &&
                    parts[1].toObject()["id"] == "cap",
                "parts order");
        require(parts[0].toObject()["material_approx"] == true &&
                    parts[1].toObject()["material_approx"] == false,
                "explicit and default approximation");
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
            require(QJsonDocument::fromJson(read(report)).object()["code"] == "IMPORT_REJECTED",
                    "rejection report");
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
    return failures ? 1 : 0;
}
