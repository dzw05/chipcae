#include "PreviewController.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vtkCellData.h>
#include <vtkCellType.h>
#include <vtkDataArray.h>
#include <vtkDataObject.h>
#include <vtkDataSet.h>
#include <vtkIntArray.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkNew.h>
#include <vtkPoints.h>
#include <vtkUnstructuredGrid.h>
using namespace chipcae::desktop;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "write fixture");
}
// Keep acceptance failures separate so every existing mock mode and the optional real CLI
// regression still run during this test-first pass. Failed expectations always fail the test.
struct Acceptance {
    int failures = 0;
    int expectations = 0;
    void expect(bool value, const QString& message) {
        ++expectations;
        if (!value) {
            ++failures;
            std::cerr << "FAIL [D10b-1] " << message.toStdString() << '\n';
        }
    }
    void defaults(const PreviewOptions& options, const QString& context) {
        expect(!options.nx.has_value(), context + ": nx is unset");
        expect(!options.ny.has_value(), context + ": ny is unset");
        expect(options.levels == 0, context + ": levels=0");
        expect(options.maxRatio == 10, context + ": maxRatio=10");
    }
    void cells(const PreviewView& view, vtkIdType count, int highlighted, const QString& context) {
        auto data = view.displayed();
        expect(data != nullptr, context + ": displayed dataset exists");
        if (data)
            expect(data->GetNumberOfCells() == count, context + ": visible cell count");
        expect(view.highlightedCount() == highlighted, context + ": highlighted cell count");
    }
    void values(const PreviewView& view,
                const char* array,
                const QList<int>& expected,
                const QString& context) {
        auto data = view.displayed();
        if (!data)
            return; // cells() reports the missing dataset without dereferencing a null stub.
        auto values = data->GetCellData()->GetArray(array);
        expect(values != nullptr, context + ": retained " + array);
        if (!values)
            return;
        expect(values->GetNumberOfTuples() == expected.size(), context + ": array tuple count");
        if (values->GetNumberOfTuples() != expected.size())
            return;
        for (qsizetype i = 0; i < expected.size(); ++i)
            expect(values->GetComponent(i, 0) == expected[i],
                   context + ": cell order at " + QString::number(i));
    }
};
void addCells(vtkDataSet* data, const char* name, const QList<int>& values) {
    vtkNew<vtkIntArray> array;
    array->SetName(name);
    for (int value : values)
        array->InsertNextValue(value);
    data->GetCellData()->AddArray(array);
}
vtkSmartPointer<vtkUnstructuredGrid> grid(const QList<int>& kinds = {0, 1, 2}) {
    auto data = vtkSmartPointer<vtkUnstructuredGrid>::New();
    vtkNew<vtkPoints> points;
    for (int i = 0; i < 3; ++i) {
        const double x = i * 2;
        points->InsertNextPoint(x, 0, 0);
        points->InsertNextPoint(x + 1, 0, 0);
        points->InsertNextPoint(x, 1, 0);
        points->InsertNextPoint(x, 0, 1);
        const vtkIdType ids[] = {4 * i, 4 * i + 1, 4 * i + 2, 4 * i + 3};
        data->InsertNextCell(VTK_TETRA, 4, ids);
    }
    data->SetPoints(points);
    addCells(data, "cell_kind", kinds);
    addCells(data, "layer_id", {1, 2, 1});
    addCells(data, "material_id", {1, 1, 2});
    return data;
}
PreviewBlock visibleBlock() {
    PreviewBlock block;
    block.visibleCells = {0, 1};
    block.highlightedCells = {1};
    return block;
}
PreviewOptions explicitOptions() {
    PreviewOptions options;
    options.nx = 8;
    options.ny = 9;
    options.levels = 3;
    options.maxRatio = 12;
    return options;
}
void viewAcceptance(Acceptance& a) {
    const QString firstLine = "nx=8 ny=9 levels=3 max_ratio=12";
    const QStringList layers{"die", "solder"}, materials{"cu", "uf"};
    const auto options = explicitOptions();
    const auto block = visibleBlock();
    auto data = grid();
    {
        PreviewController controller;
        PreviewView view;
        a.defaults(controller.effectiveOptions(), "unstarted controller");
        a.expect(view.colorMode() == PreviewColorMode::Layer, "initial color mode is Layer");
        a.expect(view.scalarArrayName() == "layer_id", "initial scalar array is layer_id");
        a.expect(!view.show(controller), "unstarted controller has no successful result");
        a.expect(view.displayed() == nullptr, "unstarted view has no displayed data");
        a.expect(view.summaryText().isEmpty(), "unstarted view has no summary");
        a.expect(view.highlightedCount() == 0, "unstarted view has no highlights");
        a.expect(!view.show(nullptr, {}, {}, {}, {}), "null input is rejected");
        a.expect(view.displayed() == nullptr && view.summaryText().isEmpty() &&
                     view.highlightedCount() == 0,
                 "null initial input preserves empty state");
    }
    {
        PreviewView view;
        a.expect(view.show(data, layers, materials, {block}, options), "synthetic mesh accepted");
        a.cells(view, 2, 1, "synthetic mesh");
        a.values(view, "cell_kind", {0, 1}, "cut remains visible and hidden cell is removed");
        a.values(view, "layer_id", {1, 2}, "synthetic layer ids");
        a.values(view, "material_id", {1, 1}, "synthetic material ids");
        a.expect(view.colorMode() == PreviewColorMode::Layer &&
                     view.scalarArrayName() == "layer_id",
                 "synthetic Layer mode uses lowercase layer_id");
        a.expect(view.colorBarTitle().contains("die") || view.colorBarTitle().contains("solder"),
                 "Layer title contains the 1-based layer name");
        a.expect(view.summaryText() == firstLine, "non-downgraded summary is exactly one line");
        view.setColorMode(PreviewColorMode::Material);
        a.expect(view.colorMode() == PreviewColorMode::Material &&
                     view.scalarArrayName() == "material_id",
                 "Material mode and lowercase material_id switch together");
        a.expect(view.colorBarTitle().contains("cu") || view.colorBarTitle().contains("uf"),
                 "Material title contains the 1-based material name");
        a.cells(view, 2, 1, "color switch preserves cells and highlights");
        PreviewBlock replacement;
        replacement.visibleCells = {0};
        replacement.hexCutDowngrade = true;
        a.expect(view.show(data, layers, materials, {replacement}, options),
                 "second show replaces the previous result");
        a.cells(view, 1, 0, "replacement uses the supplied visible/highlighted cell lists");
        a.values(view, "cell_kind", {0}, "replacement contains only its selected cell");
        a.expect(view.colorMode() == PreviewColorMode::Material &&
                     view.scalarArrayName() == "material_id",
                 "second show preserves Material mode");
        a.expect(view.summaryText() == firstLine + "\nhex cut downgrade",
                 "replacement summary is exactly the specified two lines");
        const auto displayed = view.displayed();
        const auto summary = view.summaryText();
        const auto highlighted = view.highlightedCount();
        const auto title = view.colorBarTitle();
        a.expect(!view.show(nullptr, {}, {}, {}, {}), "null replacement is rejected");
        a.expect(view.displayed() == displayed && view.summaryText() == summary &&
                     view.highlightedCount() == highlighted && view.colorBarTitle() == title &&
                     view.colorMode() == PreviewColorMode::Material,
                 "null replacement preserves data, summary, color mode, title and highlights");
        PreviewController empty;
        a.expect(!view.show(empty), "unstarted controller cannot replace an existing result");
        a.expect(view.displayed() == displayed && view.summaryText() == summary &&
                     view.highlightedCount() == highlighted &&
                     view.colorMode() == PreviewColorMode::Material,
                 "unstarted controller preserves the existing result");
        view.setColorMode(PreviewColorMode::Layer);
        a.expect(view.colorMode() == PreviewColorMode::Layer &&
                     view.scalarArrayName() == "layer_id",
                 "switching back to Layer restores lowercase layer_id");
    }
    {
        // Distinct markers detect swapped blocks, lost cells and reordered local indices.
        auto first = grid({0, 2, 1});
        auto second = grid({2, 1, 0});
        addCells(first, "marker", {10, 11, 12});
        addCells(second, "marker", {20, 21, 22});
        vtkNew<vtkMultiBlockDataSet> nested;
        nested->SetBlock(0, second);
        vtkNew<vtkMultiBlockDataSet> composite;
        composite->SetBlock(0, first);
        composite->SetBlock(1, nested);
        PreviewBlock b1, b2;
        b1.visibleCells = {0, 2};
        b1.highlightedCells = {2};
        b2.visibleCells = {1, 2};
        b2.highlightedCells = {1};
        b1.hexCutDowngrade = b2.hexCutDowngrade = true;
        PreviewView view;
        a.expect(view.show(composite, layers, materials, {b1, b2}, options),
                 "nested composite mesh accepted");
        a.cells(view, 4, 2, "two composite blocks");
        a.values(view, "marker", {10, 12, 21, 22}, "block order and local cell order");
        a.values(view, "cell_kind", {0, 1, 1, 0}, "composite filtering");
        a.expect(view.summaryText() == firstLine + "\nhex cut downgrade",
                 "multiple downgraded blocks append the marker only once");
    }
    {
        // Appending a block without ids must not discard another block's classification.
        auto first = grid();
        auto second = grid();
        second->GetCellData()->RemoveArray("layer_id");
        first->GetCellData()->RemoveArray("material_id");
        vtkNew<vtkMultiBlockDataSet> composite;
        composite->SetBlock(0, first);
        composite->SetBlock(1, second);
        PreviewView view;
        a.expect(view.show(composite, layers, materials, {block, block}, options),
                 "partially missing classification arrays accepted");
        a.cells(view, 4, 2, "partially missing classifications");
        auto shown = view.displayed();
        auto layer = shown ? shown->GetCellData()->GetArray("layer_id") : nullptr;
        auto material = shown ? shown->GetCellData()->GetArray("material_id") : nullptr;
        a.expect(layer && layer->GetNumberOfTuples() == 4 && layer->GetComponent(0, 0) == 1 &&
                     layer->GetComponent(1, 0) == 2 && std::isnan(layer->GetComponent(2, 0)),
                 "known layer ids survive append and missing ids remain unknown");
        a.expect(material && material->GetNumberOfTuples() == 4 &&
                     std::isnan(material->GetComponent(0, 0)) &&
                     material->GetComponent(2, 0) == 1 && material->GetComponent(3, 0) == 1,
                 "known material ids survive append and missing ids remain unknown");
        a.expect(view.colorBarTitle().contains("die"), "partial layer names remain available");
        view.setColorMode(PreviewColorMode::Material);
        a.expect(view.colorBarTitle().contains("cu"), "partial material names remain available");
        // Both the display and its arrays must own their data independently of the input.
        addCells(second, "material_id", {9, 9, 9});
        a.expect(material && material->GetComponent(2, 0) == 1,
                 "displayed classification is independent of later input changes");
        const auto summary = view.summaryText();
        PreviewBlock invalid = block;
        invalid.visibleCells = {3};
        a.expect(!view.show(first, layers, materials, {invalid}, {}),
                 "out-of-range cells rejected");
        invalid = block;
        invalid.highlightedCells = {2};
        a.expect(!view.show(first, layers, materials, {invalid}, {}),
                 "highlights outside visible cells rejected");
        a.expect(!view.show(composite, layers, materials, {block}, {}),
                 "mismatched block count rejected");
        a.expect(view.displayed() == shown && view.summaryText() == summary &&
                     view.highlightedCount() == 2 && view.colorMode() == PreviewColorMode::Material,
                 "invalid replacements preserve the last valid result");
    }
    {
        PreviewView view;
        a.expect(view.show(data, {}, {}, {}, {}), "zero blocks is a valid empty result");
        a.cells(view, 0, 0, "zero blocks");
        a.expect(view.summaryText() == "nx=auto ny=auto levels=0 max_ratio=10",
                 "zero blocks still has the exact default summary");
        auto hidden = grid({2, 2, 2});
        a.expect(view.show(hidden, {}, {}, {PreviewBlock{}}, options),
                 "all hidden cells is a valid empty result");
        a.cells(view, 0, 0, "all hidden cells");
        a.expect(view.summaryText() == firstLine,
                 "all hidden cells still has an effective summary");
        PreviewOptions fractional;
        fractional.nx = 6;
        fractional.maxRatio = 12.345678901234567;
        a.expect(view.show(data, {}, {}, {block}, fractional), "fractional ratio input accepted");
        a.expect(view.summaryText() == "nx=6 ny=auto levels=0 max_ratio=" +
                                           QString::number(fractional.maxRatio, 'g', 15),
                 "summary uses independent auto dimensions and 15 significant digits");
        fractional.nx.reset();
        fractional.ny = 7;
        a.expect(view.show(data, {}, {}, {block}, fractional), "automatic nx input accepted");
        a.expect(view.summaryText() == "nx=auto ny=7 levels=0 max_ratio=" +
                                           QString::number(fractional.maxRatio, 'g', 15),
                 "automatic nx and explicit ny summary");
    }
    {
        auto missing = grid();
        missing->GetCellData()->RemoveArray("layer_id");
        missing->GetCellData()->RemoveArray("material_id");
        PreviewView view;
        a.expect(view.show(missing, layers, materials, {block}, options),
                 "missing classification arrays do not reject geometry");
        view.setColorMode(PreviewColorMode::Layer);
        a.expect(view.scalarArrayName() == "layer_id", "missing layer array keeps lowercase name");
        a.expect(view.colorBarTitle().isEmpty() || view.colorBarTitle() == "layer_id",
                 "missing layer array has empty or array-name title");
        a.cells(view, 2, 1, "missing classification arrays");
        view.setColorMode(PreviewColorMode::Material);
        a.expect(view.scalarArrayName() == "material_id",
                 "missing material array keeps lowercase name");
        addCells(missing, "Layer_Id", {1, 2, 1});
        a.expect(view.show(missing, layers, materials, {block}, options),
                 "mixed-case array does not prevent displaying geometry");
        view.setColorMode(PreviewColorMode::Layer);
        a.expect(view.scalarArrayName() == "layer_id",
                 "Layer_Id never changes selected array name");
        a.expect(view.colorBarTitle().isEmpty() || view.colorBarTitle() == "layer_id",
                 "Layer_Id must not supply a layer name to the title");
        a.cells(view, 2, 1, "only Layer_Id");
    }
    {
        auto unknown = grid();
        addCells(unknown, "layer_id", {7, 7, 7});
        addCells(unknown, "material_id", {9, 9, 9});
        for (bool emptyNames : {true, false}) {
            PreviewView view;
            const QString context = emptyNames ? "empty name tables" : "out-of-range ids";
            a.expect(view.show(unknown,
                               emptyNames ? QStringList{} : QStringList{"only layer"},
                               emptyNames ? QStringList{} : QStringList{"only material"},
                               {block},
                               options),
                     context + ": accepted");
            a.expect(view.colorBarTitle().contains("layer 7"),
                     context + ": literal layer fallback");
            view.setColorMode(PreviewColorMode::Material);
            a.expect(view.colorBarTitle().contains("material 9"),
                     context + ": literal material fallback");
        }
        PreviewView view;
        const QStringList utfLayers{QString::fromUtf8("芯片"), QString::fromUtf8("焊料")};
        const QStringList utfMaterials{QString::fromUtf8("铜"), QString::fromUtf8("底填胶")};
        a.expect(view.show(data, utfLayers, utfMaterials, {block}, options),
                 "UTF-8 names accepted");
        a.expect(view.colorBarTitle().contains(utfLayers[0]) ||
                     view.colorBarTitle().contains(utfLayers[1]),
                 "Layer title preserves an exact UTF-8 name");
        view.setColorMode(PreviewColorMode::Material);
        a.expect(view.colorBarTitle().contains(utfMaterials[0]) ||
                     view.colorBarTitle().contains(utfMaterials[1]),
                 "Material title preserves an exact UTF-8 name");
    }
}
void controllerViewAcceptance(Acceptance& a, const PreviewController& controller) {
    vtkIdType visible = 0;
    int highlighted = 0;
    bool downgraded = false;
    for (const auto& block : controller.blocks()) {
        visible += block.visibleCells.size();
        highlighted += static_cast<int>(block.highlightedCells.size());
        downgraded = downgraded || block.hexCutDowngrade;
    }
    PreviewView view;
    a.expect(view.show(controller), "successful controller supplies the preview view");
    a.expect(view.colorMode() == PreviewColorMode::Layer && view.scalarArrayName() == "layer_id",
             "controller view defaults to Layer and lowercase layer_id");
    a.cells(view, visible, highlighted, "controller inspection counts");
    a.expect(view.summaryText().section('\n', 0, 0) == "nx=8 ny=9 levels=3 max_ratio=12",
             "controller summary first line is exact");
    a.expect(view.summaryText().contains("hex cut downgrade") == downgraded,
             "controller downgrade summary agrees with inspected blocks");
    // PDC names live in ResultSession, not necessarily in partition FieldData.
    auto data = view.displayed();
    for (auto mode : {PreviewColorMode::Layer, PreviewColorMode::Material}) {
        view.setColorMode(mode);
        const bool layer = mode == PreviewColorMode::Layer;
        const char* arrayName = layer ? "layer_id" : "material_id";
        a.expect(view.scalarArrayName() == arrayName,
                 "controller classification array is lowercase");
        auto ids = data ? data->GetCellData()->GetArray(arrayName) : nullptr;
        if (!ids || ids->GetNumberOfTuples() == 0)
            continue;
        bool named = false;
        for (vtkIdType i = 0; i < ids->GetNumberOfTuples(); ++i) {
            const int id = static_cast<int>(ids->GetComponent(i, 0));
            const auto name =
                layer ? controller.session().layerName(id) : controller.session().materialName(id);
            const auto fallback = QString(layer ? "layer %1" : "material %1").arg(id);
            named = named || view.colorBarTitle().contains(name.isEmpty() ? fallback : name);
        }
        a.expect(named, "controller view title uses ResultSession names");
    }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Acceptance acceptance;
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
                                 if (stage == "success") {
                                     ++successes;
                                     acceptance.expect(controller.running(),
                                                       "success event precedes completion");
                                     acceptance.defaults(controller.effectiveOptions(),
                                                         QString(mode) + " pending success");
                                     acceptance.expect(document.toJson() == before,
                                                       "pending success must not change case");
                                     PreviewView pending;
                                     acceptance.expect(!pending.show(controller),
                                                       "pending success cannot populate a view");
                                     acceptance.expect(pending.displayed() == nullptr &&
                                                           pending.summaryText().isEmpty() &&
                                                           pending.highlightedCount() == 0,
                                                       "pending success view remains empty");
                                 }
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
                acceptance.defaults(controller.effectiveOptions(), "hash failure");
                check(before == document.toJson(), "hash failure must not change case");
                continue;
            }
            check(started, "start preview");
            acceptance.defaults(controller.effectiveOptions(), QString(mode) + " running");
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
                const auto effective = controller.effectiveOptions();
                acceptance.expect(effective.nx == (automatic ? 4 : 8),
                                  QString(mode) + ": effective nx");
                acceptance.expect(effective.ny == (automatic ? 5 : 9),
                                  QString(mode) + ": effective ny");
                acceptance.expect(effective.levels == (automatic ? 1 : 3),
                                  QString(mode) + ": effective levels (L+1)");
                acceptance.expect(effective.maxRatio == 12, QString(mode) + ": effective maxRatio");
                acceptance.expect(effective.nx == mesh.nx && effective.ny == mesh.ny &&
                                      effective.levels == mesh.levels &&
                                      effective.maxRatio == mesh.maxRatio,
                                  QString(mode) + ": effective options equal case write-back");
                if (QString(mode) == "success" || QString(mode) == "pdc")
                    controllerViewAcceptance(acceptance, controller);
                if (QString(mode) == "success") {
                    // A new accepted start must invalidate the previous successful snapshot.
                    config["mode"] = "empty";
                    input["mock"] = Json::parse(QJsonDocument(config).toJson().toStdString());
                    write(stack, QByteArray::fromStdString(input.to_string()));
                    const auto afterSuccess = document.toJson();
                    done = result = false;
                    check(controller.start(&document,
                                           stack,
                                           dir.filePath("mesh out.vtkhdf"),
                                           options,
                                           PREVIEW_PYTHON,
                                           {PREVIEW_MOCK}),
                          "restart previously successful preview");
                    acceptance.defaults(controller.effectiveOptions(), "restart clears snapshot");
                    timer.start(5000);
                    loop.exec();
                    check(done && !result, "restarted empty preview fails");
                    check(document.toJson() == afterSuccess,
                          "failed restart preserves the last written case");
                    acceptance.defaults(controller.effectiveOptions(), "failed restart snapshot");
                }
            } else {
                check(!result && !controller.error().isEmpty(), "invalid run rejected");
                check(before == document.toJson(), "failed run must not change case");
                acceptance.defaults(controller.effectiveOptions(),
                                    QString(mode) + " failed/cancelled");
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
        std::cout << "D10 existing preview regressions passed\n";
        viewAcceptance(acceptance);
        std::cout << "D10b-1 acceptance: " << acceptance.expectations << " expectations, "
                  << acceptance.failures << " failures\n";
        return acceptance.failures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
