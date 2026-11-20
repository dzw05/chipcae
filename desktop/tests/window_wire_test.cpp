#include "AssemblyImport.hpp"
#include "ImportController.hpp"
#include "ImportSourceDialog.hpp"
#include "MainWindow.hpp"
#include "ResultControls.hpp"
#include "ResultView.hpp"
#include "StackEditor.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class Window>
void wiring(Window& w, const QString& samples) {
    auto filters = ImportSourceDialog::nameFilters().join(' ');
    require(filters.contains("*.tgz") && filters.contains("*.zip") && filters.contains("ODB++") &&
                filters.contains("*.step") && filters.contains("*.stp") &&
                filters.contains("B 档") && filters.contains("disabled"),
            "selector filters");
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory");
    for (auto suffix : {"xml", "tgz", "zip", "step", "stp", "txt"}) {
        QFile source(dir.filePath(QString("input.") + suffix));
        require(source.open(QIODevice::WriteOnly), "create source fixture");
        source.close();
    }
    for (auto suffix : {"xml", "tgz", "zip"})
        require(ImportSourceDialog::isSupportedImportPath(dir.filePath(QString("input.") + suffix)),
                "supported source");
    require(ImportSourceDialog::isSupportedImportPath(dir.path()), "directory source");
    for (auto suffix : {"step", "stp", "txt"})
        require(
            !ImportSourceDialog::isSupportedImportPath(dir.filePath(QString("input.") + suffix)),
            "unsupported source");
    require(w.loadHandwrittenStack(), "load handwritten template");
    QFile f(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.stack.json");
    require(f.open(QIODevice::ReadOnly), "read template");
    require(w.document()->stackJson() == Json::parse(f.readAll().toStdString()),
            "template content");
    require(w.document()->isDirty(), "template dirty");
    auto* editor = w.template findChild<StackEditor*>();
    require(editor && editor->model()->rowCount() ==
                          static_cast<int>(w.document()->stackJson().at("layers").size()),
            "stack refresh");
    require(editor->model()->flags(editor->model()->index(0, 0)) & Qt::ItemIsEditable,
            "stack editable");
    require(editor->model()->setData(editor->model()->index(0, 0), "edited_substrate"),
            "stack editor accepts edits");
    require(w.document()->stackJson().at("layers")[0].at("name").template as<std::string>() ==
                "edited_substrate",
            "stack edit reaches document");
    QFile materialMap(dir.filePath("material map.json"));
    require(materialMap.open(QIODevice::WriteOnly), "create material map fixture");
    require(materialMap.write("{}") == 2, "write material map fixture");
    materialMap.close();
    auto run = [&](const QByteArray& mode, bool cancel) {
        QFile input(dir.filePath("fixture.xml"));
        require(input.open(QIODevice::WriteOnly | QIODevice::Truncate), "write input");
        input.write(mode);
        input.close();
        auto before = w.document()->stackJson();
        QEventLoop loop;
        bool finished = false, replaced = false;
        auto connection = QObject::connect(
            w.importController(), &ImportController::finished, &loop, [&](bool changed) {
                finished = true;
                replaced = changed;
                loop.quit();
            });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        ImportRequest request;
        request.input = input.fileName();
        request.output = dir.filePath("import.stack.json");
        request.materialMap = materialMap.fileName();
        request.polyset = dir.filePath("import.polyset");
        request.report = dir.filePath("defects.json");
        request.kind = ImportKind::Ecad;
        request.program = PYTHON_EXECUTABLE;
        request.prefix = {MOCK_SCRIPT};
        require(w.importController()->start(request), "start ImportRequest");
        require(w.importController()->busy(), "controller running");
        if (cancel) {
            require(!w.loadHandwrittenStack() && w.document()->stackJson() == before,
                    "handwritten template blocked during import");
            require(
                !w.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
                "open case blocked during import");
            QTimer::singleShot(100, &loop, [&] { w.cancelImport(); });
        }
        loop.exec();
        QObject::disconnect(connection);
        require(finished, "import finished");
        if (cancel)
            require(!replaced && w.document()->stackJson() == before, "cancel preserves stack");
        else
            require(replaced && w.document()->stackJson() != before &&
                        editor->model()->rowCount() ==
                            static_cast<int>(w.document()->stackJson().at("layers").size()),
                    "A import refresh");
    };
    require(w.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
            "load case and materials for validated import");
    const auto beforeLegacy = w.document()->stackJson();
    require(!w.importEcadPath(dir.filePath("input.xml"), PYTHON_EXECUTABLE, {MOCK_SCRIPT}),
            "legacy import rejects missing material map");
    require(w.importController()->error() == "material map required" &&
                w.importController()->outcome() == ImportOutcome::Failed &&
                !w.importController()->busy() &&
                !w.importController()->needsMaterialMapReselect() &&
                w.document()->stackJson() == beforeLegacy,
            "legacy rejection preserves document without starting import");
    run("hang", true);
    run("success", false);
    require(w.openResultsPath(samples + "/default/manifest.json"), "open results");
    require(w.centralWidget() == w.resultView() && w.resultView()->error().isEmpty(),
            "central result view");
    require(w.resultView()->loadStep(*w.resultSession(), 1), "load next step");
    require(!w.resultSession()->summary().isEmpty(), "summary available");
    auto summary = w.resultSession()->summary().first().toObject();
    require(!w.resultView()->summary(summary["name"].toString()).contains("unavailable"),
            "summary readable");
    require(w.openResultsPath(samples + "/hex_only/manifest.json"), "open uncut results again");
    require(w.resultView()->error().isEmpty(), "uncut result view");
    require(!w.openResultsPath(samples + "/missing/manifest.json"), "invalid results rejected");
    require(w.centralWidget() == w.resultView(), "preserve results on failure");
}
void selectableEcadFilters(MainWindow& w) {
    bool inspected = false, excludesStep = false, existingFile = false;
    QTimer::singleShot(0, &w, [&] {
        auto* dialog = w.findChild<QFileDialog*>();
        if (!dialog)
            return;
        inspected = true;
        const auto filters = dialog->nameFilters().join(' ');
        excludesStep = !filters.contains("step", Qt::CaseInsensitive) &&
                       !filters.contains("stp", Qt::CaseInsensitive);
        existingFile = dialog->fileMode() == QFileDialog::ExistingFile &&
                       dialog->testOption(QFileDialog::DontUseNativeDialog);
        dialog->reject();
    });
    require(ImportSourceDialog::getImportPath(&w).isEmpty(), "cancel ECAD selector");
    require(inspected && existingFile && excludesStep,
            "selectable ECAD filters exclude STEP/STP and retain non-native ExistingFile mode");
}
void assemblyWiring(MainWindow& w) {
    const auto input =
        QString(CHIPCAE_REPOSITORY) + "/schema/examples/assembly/package_bulk_parts.assembly.json";
    const auto mock = QString(CHIPCAE_REPOSITORY) + "/desktop/tools/mock_assembly_import.py";
    require(isAssemblyPath(input), "I2 is an assembly path");
    QTemporaryDir temp;
    require(temp.isValid(), "assembly temporary directory");
    QFile ordinary(temp.filePath("ordinary.json"));
    require(ordinary.open(QIODevice::WriteOnly) && ordinary.write("{}") == 2,
            "write ordinary JSON");
    ordinary.close();
    const auto before = w.document()->stackJson();
    QObject receiver;
    int completions = 0;
    bool replaced = false;
    QObject::connect(
        w.importController(), &ImportController::finished, &receiver, [&](bool changed) {
            ++completions;
            replaced = changed;
        });
    require(!w.importEcadPath(input, PYTHON_EXECUTABLE, {mock}),
            "ECAD hook rejects assembly input");
    require(!w.importEcadPath(ordinary.fileName(), PYTHON_EXECUTABLE, {mock}) &&
                !w.importAssemblyPath(ordinary.fileName(), PYTHON_EXECUTABLE, {mock}),
            "both hooks reject ordinary JSON");
    require(!w.importAssemblyPath({}, PYTHON_EXECUTABLE, {mock}) &&
                !w.importAssemblyPath(temp.filePath("missing.json"), PYTHON_EXECUTABLE, {mock}),
            "assembly hook rejects empty and missing paths");
    QCoreApplication::processEvents();
    require(completions == 0 && !w.importController()->busy() &&
                w.document()->stackJson() == before,
            "invalid import paths emit no completion and preserve idle document");
    auto import = [&](const QString& path) {
        const int previous = completions;
        QEventLoop loop;
        auto connection = QObject::connect(
            w.importController(), &ImportController::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        require(w.importAssemblyPath(path, PYTHON_EXECUTABLE, {mock}),
                "assembly hook starts valid I2 input");
        require(!w.importAssemblyPath(path, PYTHON_EXECUTABLE, {mock}),
                "busy assembly hook rejects duplicate start");
        if (completions == previous)
            loop.exec();
        QObject::disconnect(connection);
        require(completions == previous + 1 && replaced && !w.importController()->busy() &&
                    w.importController()->outcome() == ImportOutcome::Accepted,
                "assembly completes once with replacement and Accepted outcome");
    };
    import(input);
    const auto upper = temp.filePath("assembly.JSON");
    require(QFile::copy(input, upper) && isAssemblyPath(upper), "uppercase assembly fixture");
    import(upper);
}
namespace {
void writeBytes(const QString& path, const QByteArray& bytes) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture parent");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open fixture output");
    require(file.write(bytes) == bytes.size(), "write complete fixture");
}
void copyResults(const QString& source, const QString& target) {
    require(QDir().mkpath(target), "create result copy directory");
    QDirIterator it(source, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        const auto destination = QDir(target).filePath(QDir(source).relativeFilePath(path));
        require(QDir().mkpath(QFileInfo(destination).absolutePath()), "create result subdirectory");
        require(QFile::copy(path, destination), "copy synthetic result fixture");
    }
}
Json resultStack() {
    return Json::parse(R"({"id":"result_stack","unit":"m","outline":{"box":[0,0,0.02,0.02]},
        "symmetry":"none","package_type":"FCBGA","layers":[
        {"name":" Base ","material":"Organic_EQ","thickness":0.0012345678901234567,
         "kind":"Homogenized","n_elem_min":2,"grading":1},
        {"name":"tOP","material":"Si_mixed","thickness":0.00023456789012345678,
         "kind":"Explicit","n_elem_min":2,"grading":1}]})");
}
void requireRows(LayerModel* model, const Json& stack) {
    const auto& layers = stack.at("layers");
    require(model->rowCount() == static_cast<int>(layers.size()),
            qPrintable(QString("result stack row count: expected %1, got %2")
                           .arg(layers.size())
                           .arg(model->rowCount())));
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto& layer = layers.at(row);
        require(model->index(row, LayerModel::Name).data().toString().toStdString() ==
                    layer.at("name").as<std::string>(),
                "stack name preserves case and whitespace");
        require(model->index(row, LayerModel::Material).data().toString().toStdString() ==
                    layer.get_value_or<std::string>("material", ""),
                "stack material matches JSON");
        // This is a display transport check: the brief requires the identical parsed double.
        require(model->index(row, LayerModel::Thickness).data().toDouble() ==
                    layer.at("thickness").as<double>(),
                "stack thickness preserves the parsed double exactly");
    }
}
StackEditor* stackEditor(MainWindow& window) {
    auto* editor = window.findChild<StackEditor*>();
    require(editor != nullptr, "existing StackEditor is installed");
    return editor;
}
ResultControls* resultControls(MainWindow& window) {
    // ResultControls is a QWidget without Q_OBJECT; use C++ RTTI, not Qt metaobject casts.
    for (auto* widget : window.findChildren<QWidget*>())
        if (auto* controls = dynamic_cast<ResultControls*>(widget))
            return controls;
    return nullptr;
}
int resultsStackAcceptance(const QString& samples) {
    int failures = 0;
    const auto run = [&](const QString& name, auto test) {
        try {
            test();
            std::cout << "PASS results stack: " << name.toStdString() << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL results stack: " << name.toStdString() << ": " << e.what() << '\n';
        }
    };
    QTemporaryDir temp;
    require(temp.isValid(), "results acceptance temporary directory");
    const auto pack = temp.filePath("with-stack");
    copyResults(samples + "/viewer_mixed", pack);
    const auto stack = resultStack();
    writeBytes(pack + "/stackup.json", QByteArray::fromStdString(stack.to_string()));
    const auto manifest = pack + "/manifest.json";
    run("result-only rows and document isolation", [&] {
        MainWindow window;
        auto* editor = stackEditor(window);
        const auto before = window.document()->toJson();
        const auto beforeStack = window.document()->stackJson();
        const auto stale = editor->previewStale();
        const auto displayStale = window.previewDisplayStale();
        int invalidated = 0;
        QObject::connect(editor, &StackEditor::previewInvalidated, &window, [&] { ++invalidated; });
        require(window.lastOpenResultsStackFillMs() == -1, "initial stack fill time is -1");
        require(window.openResultsPath(manifest), "open result-only pack");
        requireRows(editor->model(), stack);
        require(editor->isReadOnly(), "result-only stack is read-only");
        require(!window.caseIsOpen(), "result display is not an open case");
        require(!window.document()->isDirty() && window.document()->toJson() == before &&
                    window.document()->stackJson() == beforeStack,
                "result display does not mutate or dirty CaseDocument");
        require(editor->previewStale() == stale && window.previewDisplayStale() == displayStale &&
                    invalidated == 0,
                "result display leaves preview state and signals untouched");
        require(!window.submitCurrentCase(), "result-only submit remains rejected");
        require(!window.previewMesh("missing-preview-program", {}),
                "result-only preview remains rejected");
        require(window.lastOpenResultsStackFillMs() >= 0, "successful stack fill records time");
    });
    run("timing on success and early failure", [&] {
        MainWindow window;
        require(window.lastOpenResultsStackFillMs() == -1, "timer initially unset");
        require(!window.openResultsPath(temp.filePath("missing/manifest.json")),
                "missing results rejected");
        require(window.lastOpenResultsStackFillMs() >= 0, "early failure records stack fill time");
        require(window.openResultsPath(manifest), "open timed result");
        require(window.lastOpenResultsStackFillMs() >= 0,
                "successful open records stack fill time");
    });
    run("existing case keeps every cell and editability", [&] {
        MainWindow window;
        require(
            window.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
            "open case before result");
        auto* editor = stackEditor(window);
        const auto before = window.document()->toJson();
        const auto beforeStack = window.document()->stackJson();
        const auto dirty = window.document()->isDirty();
        const auto stale = editor->previewStale();
        QList<QVariant> cells;
        for (int row = 0; row < editor->model()->rowCount(); ++row)
            for (int col = 0; col < editor->model()->columnCount(); ++col)
                cells << editor->model()->index(row, col).data();
        int invalidated = 0;
        QObject::connect(editor, &StackEditor::previewInvalidated, &window, [&] { ++invalidated; });
        require(window.openResultsPath(manifest), "open results after case");
        requireRows(editor->model(), beforeStack);
        QList<QVariant> afterCells;
        for (int row = 0; row < editor->model()->rowCount(); ++row)
            for (int col = 0; col < editor->model()->columnCount(); ++col)
                afterCells << editor->model()->index(row, col).data();
        require(afterCells == cells, "all case cells survive result open");
        require(window.caseIsOpen() && !editor->isReadOnly() &&
                    (editor->model()->flags(editor->model()->index(0, 0)) & Qt::ItemIsEditable),
                "case remains open and editable");
        require(window.document()->toJson() == before && window.document()->isDirty() == dirty &&
                    editor->previewStale() == stale && invalidated == 0,
                "case and preview unchanged by results");
    });
    run("document stack without case path takes priority", [&] {
        MainWindow window;
        require(window.loadHandwrittenStack(), "load handwritten case stack");
        const auto before = window.document()->stackJson();
        require(window.openResultsPath(manifest), "open result after handwritten stack");
        requireRows(stackEditor(window)->model(), before);
        require(!stackEditor(window)->isReadOnly() && window.document()->stackJson() == before,
                "handwritten stack remains editable and unchanged");
    });
    run("result to empty result clears old rows", [&] {
        MainWindow window;
        require(window.openResultsPath(manifest), "open populated results");
        requireRows(stackEditor(window)->model(), stack);
        require(window.openResultsPath(samples + "/viewer_mixed/manifest.json"),
                "open result without stackup");
        require(stackEditor(window)->model()->rowCount() == 0, "no stale result rows");
        require(!window.caseIsOpen() && !window.document()->isDirty(),
                "empty result stays result-only");
    });
    run("result to case restores editable document rows", [&] {
        MainWindow window;
        require(window.openResultsPath(manifest), "open results before case");
        require(
            window.openCase(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
            "open case after result");
        auto* editor = stackEditor(window);
        requireRows(editor->model(), window.document()->stackJson());
        require(
            window.caseIsOpen() && !editor->isReadOnly() &&
                editor->model()->setData(editor->model()->index(0, LayerModel::Name), "renamed"),
            "case editing restored after results");
    });
    run("failed result open preserves existing rows", [&] {
        MainWindow window;
        require(window.openResultsPath(manifest), "open results before failed replacement");
        require(!window.openResultsPath(temp.filePath("absent.json")), "replacement fails");
        requireRows(stackEditor(window)->model(), stack);
        require(stackEditor(window)->isReadOnly(),
                "failed replacement keeps read-only result stack");
    });
    run("Chinese form labels", [&] {
        MainWindow window;
        require(window.openResultsPath(manifest), "open result for controls");
        auto* controls = resultControls(window);
        require(controls != nullptr, "result controls installed");
        auto* form = qobject_cast<QFormLayout*>(controls->layout());
        require(form != nullptr, "controls form layout");
        QComboBox *field = nullptr, *layer = nullptr;
        for (auto* combo : controls->findChildren<QComboBox*>()) {
            if (combo->itemText(0) == QStringLiteral("温度"))
                field = combo;
            else if (combo->count() && combo->itemData(0).isValid() &&
                     combo->itemData(0).toInt() == 0 &&
                     (combo->itemText(0) == QStringLiteral("全部层") ||
                      combo->itemText(0) == "All layers"))
                layer = combo;
        }
        require(field && layer, "field and layer controls found by role");
        const auto label = [&](QWidget* widget) {
            auto* text = qobject_cast<QLabel*>(form->labelForField(widget));
            require(text != nullptr, "form field has a label");
            return text->text();
        };
        require(label(field) == QStringLiteral("字段"), "field label is 字段");
        require(label(layer) == QStringLiteral("叠层"), "layer label is 叠层");
        QDoubleSpinBox* scale = nullptr;
        for (auto* spin : controls->findChildren<QDoubleSpinBox*>())
            if (spin->decimals() == 6 && spin->value() == 1)
                scale = spin;
        require(scale && label(scale) == QStringLiteral("翘曲放大系数"), "warpage scale label");
        require(layer->itemText(0) == QStringLiteral("全部层") && layer->itemData(0).toInt() == 0,
                "all-layers item and data");
    });
    run("layer dropdown consumes fallback names in order", [&] {
        MainWindow window;
        require(window.openResultsPath(manifest), "open unnamed pack with stack");
        auto* controls = resultControls(window);
        require(controls != nullptr, "fallback controls present");
        QComboBox* layer = nullptr;
        auto* form = qobject_cast<QFormLayout*>(controls->layout());
        require(form != nullptr, "fallback controls form");
        for (auto* combo : controls->findChildren<QComboBox*>()) {
            auto* label = qobject_cast<QLabel*>(form->labelForField(combo));
            if (label && (label->text() == QStringLiteral("叠层") || label->text() == "Layer"))
                layer = combo;
        }
        require(layer && layer->count() == 3, "layer dropdown includes two fallback names");
        for (int row = 0; row < 2; ++row)
            require(layer->itemText(row + 1).toStdString() ==
                            stack.at("layers").at(row).at("name").as<std::string>() &&
                        layer->itemData(row + 1).toInt() == row + 1,
                    "fallback dropdown name and 1-based data");
    });
    for (const auto& name : {"missing",
                             "empty",
                             "corrupt",
                             "array",
                             "no-layers",
                             "empty-layers",
                             "wrong-layers",
                             "wrong-row",
                             "wrong-name",
                             "wrong-thickness",
                             "wrong-material",
                             "absolute",
                             "parent",
                             "symlink",
                             "specified-missing",
                             "relative"}) {
        run(QString("stack path and JSON: ") + name, [&] {
            const auto target = temp.filePath(name);
            copyResults(samples + "/viewer_mixed", target);
            QFile file(target + "/manifest.json");
            require(file.open(QIODevice::ReadOnly), "read manifest for path fixture");
            auto m = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            const auto valid = QByteArray::fromStdString(stack.to_string());
            const QMap<QString, QByteArray> invalid{
                {"empty", ""},
                {"corrupt", "{"},
                {"array", "[]"},
                {"no-layers", "{}"},
                {"empty-layers", "{\"layers\":[]}"},
                {"wrong-layers", "{\"layers\":3}"},
                {"wrong-row", "{\"layers\":[3]}"},
                {"wrong-name", "{\"layers\":[{\"name\":3,\"thickness\":0.1}]}"},
                {"wrong-thickness", "{\"layers\":[{\"name\":\"x\",\"thickness\":[]}]}"},
                {"wrong-material",
                 "{\"layers\":[{\"name\":\"x\",\"thickness\":0.1,\"material\":{}}]}"}};
            if (invalid.contains(name))
                writeBytes(target + "/stackup.json", invalid.value(name));
            if (QString(name) == "relative") {
                m["stackup"] = "Nested/StackUP.json";
                writeBytes(target + "/Nested/StackUP.json", valid);
                writeBytes(target + "/stackup.json", "{}");
            } else if (QString(name) == "absolute" || QString(name) == "parent" ||
                       QString(name) == "symlink" || QString(name) == "specified-missing") {
                writeBytes(temp.filePath("outside.json"), valid);
                // A valid default must not mask an explicitly invalid manifest path.
                writeBytes(target + "/stackup.json", valid);
                if (QString(name) == "absolute")
                    m["stackup"] = temp.filePath("outside.json");
                else if (QString(name) == "parent")
                    m["stackup"] = "../outside.json";
                else if (QString(name) == "specified-missing")
                    m["stackup"] = "absent.json";
                else {
                    require(QFile::link(temp.filePath("outside.json"), target + "/escape.json"),
                            "create escaping stackup symlink");
                    m["stackup"] = "escape.json";
                }
            }
            writeBytes(target + "/manifest.json", QJsonDocument(m).toJson());
            MainWindow window;
            require(window.openResultsPath(target + "/manifest.json"),
                    "invalid stack does not reject results");
            if (QString(name) == "relative")
                requireRows(stackEditor(window)->model(), stack);
            else
                require(stackEditor(window)->model()->rowCount() == 0,
                        "invalid or absent stack gives zero rows");
            require(!window.caseIsOpen() && !window.document()->isDirty(),
                    "path test preserves document");
        });
    }
    for (const auto& entry :
         {qMakePair("CHIPCAE_S1", QStringList{"substrate", "die_attach", "Si"}),
          qMakePair("CHIPCAE_S1B", QStringList{"substrate", "die_attach", "Si"}),
          qMakePair("CHIPCAE_DEMO_GATE", QStringList{"substrate", "bump_eq", "die_layer"})}) {
        const auto path = qEnvironmentVariable(entry.first);
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            std::cout << "SKIP official results stack: " << entry.first << " is unavailable\n";
            continue;
        }
        run(entry.first, [&] {
            MainWindow window;
            require(window.openResultsPath(
                        QFileInfo(path).isDir() ? QDir(path).filePath("manifest.json") : path),
                    "open optional official results");
            auto* model = stackEditor(window)->model();
            require(model->rowCount() == 3, "official stack has three layers");
            for (int row = 0; row < 3; ++row)
                require(model->index(row, LayerModel::Name).data().toString() == entry.second[row],
                        "official stack layer order");
        });
    }
    return failures;
}
} // namespace

int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    int failures = 0;
    try {
        require(argc > 1, "result samples argument required");
        MainWindow w;
        QStringList actions;
        for (auto* action : w.findChildren<QAction*>())
            actions << action->text();
        auto text = actions.join('\n');
        require(text.contains("Import ECAD"), "missing Import ECAD action");
        require(text.contains("Open Results"), "missing Open Results action");
        require(text.contains("handwritten", Qt::CaseInsensitive), "missing handwritten action");
        wiring(w, QString::fromLocal8Bit(argv[1]));
        selectableEcadFilters(w);
        assemblyWiring(w);
        std::cout << "window wiring checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        ++failures;
    }
    if (argc > 1)
        failures += resultsStackAcceptance(QString::fromLocal8Bit(argv[1]));
    return failures ? 1 : 0;
}
