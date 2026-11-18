#include "AssemblyImport.hpp"
#include "ImportController.hpp"
#include "ImportSourceDialog.hpp"
#include "MainWindow.hpp"
#include "ResultControls.hpp"
#include "ResultView.hpp"
#include "StackEditor.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
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
int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
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
        return 1;
    }
}
