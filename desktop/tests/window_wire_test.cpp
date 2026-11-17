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
        require(w.importEcadPath(input.fileName(), PYTHON_EXECUTABLE, {MOCK_SCRIPT}),
                "start import");
        require(w.importController()->busy(), "controller running");
        if (cancel)
            QTimer::singleShot(100, &loop, [&] { w.cancelImport(); });
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
        require(!text.contains("Submit") && !text.contains("Preview"), "out of scope actions");
        for (auto* child : w.findChildren<QObject*>())
            require(QByteArray(child->metaObject()->className()) !=
                        "chipcae::desktop::LocalJobService",
                    "window owns job service");
        wiring(w, QString::fromLocal8Bit(argv[1]));
        std::cout << "window wiring checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
