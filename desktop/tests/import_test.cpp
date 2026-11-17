#include "CaseDocument.hpp"
#include "DefectsDialog.hpp"
#include "ImportController.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace chipcae::desktop;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "open fixture");
    require(file.write(bytes) == bytes.size(), "write fixture");
}
} // namespace
int main(int argc, char** argv) {
    const bool widgets = argc > 1 && QByteArray(argv[1]) == "--dialog";
    std::unique_ptr<QCoreApplication> app;
    if (widgets)
        app = std::make_unique<QApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    try {
        CaseDocument document;
        require(document.load(FIXTURE_DIR "/fcbga_demo.case.json"), "load baseline");
        ImportController controller(document);
        std::unique_ptr<DefectsDialog> dialog;
        if (widgets)
            dialog = std::make_unique<DefectsDialog>(controller);
        QTemporaryDir dir(QDir::tempPath() + "/import paths with spaces-XXXXXX");
        require(dir.isValid(), "temporary directory");
        auto run = [&](const QByteArray& mode, const QString& program = PYTHON_EXECUTABLE) {
            const auto before = document.stackJson();
            auto input = dir.filePath("input file " + QString::fromUtf8(mode) +
                                      (mode == "success" ? ".xml" : ".step"));
            auto output = input + ".stack.json";
            auto polyset = input + ".polyset";
            auto report = input + ".report.json";
            write(input, mode);
            QEventLoop loop;
            bool completed = false, replaced = false, reportShown = false;
            const auto finish = QObject::connect(
                &controller, &ImportController::finished, &loop, [&](bool changed) {
                    completed = true;
                    replaced = changed;
                    loop.quit();
                });
            const auto ready = QObject::connect(
                &controller, &ImportController::defectsReady, &loop, [&] { reportShown = true; });
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(5000);
            require(controller.start(input, output, polyset, report, program, {MOCK_SCRIPT}),
                    "start import");
            require(!controller.start(input, output), "reject overlapping import");
            if (mode == "hang")
                QTimer::singleShot(200, &controller, &ImportController::cancel);
            loop.exec();
            QObject::disconnect(finish);
            QObject::disconnect(ready);
            require(completed && !controller.busy(), "completion before timeout");
            if (mode == "success") {
                require(replaced && document.isDirty() && document.stackJson() != before,
                        "successful replacement");
                require(QFile::exists(output) && QFile::exists(polyset) && QFile::exists(report),
                        "publish all outputs");
                require(controller.exitCode() == 0 && controller.grade() == "A", "success status");
                require(!controller.start(input, output), "never overwrite existing output");
                require(document.save(output + ".case.json"), "save imported document");
                CaseDocument reloaded;
                require(reloaded.load(output + ".case.json") &&
                            reloaded.stackJson() == document.stackJson(),
                        "imported stack survives case round trip");
            } else {
                require(!replaced && document.stackJson() == before, "preserve active stack");
                require(!QFile::exists(output) && !QFile::exists(polyset) && !QFile::exists(report),
                        "no partial published outputs");
            }
            if (mode == "b") {
                require(reportShown && controller.grade() == "B" && controller.exitCode() == 2,
                        "B report awaits confirmation");
                bool confirmed = false;
                auto connection =
                    QObject::connect(&controller, &ImportController::userConfirmed, &loop, [&] {
                        confirmed = true;
                    });
                if (dialog) {
                    require(dialog->isVisible(), "B dialog visible");
                    require(
                        dialog->findChild<QPlainTextEdit*>()->toPlainText().contains("MOCK_GAP"),
                        "dialog displays defect details");
                    for (auto* button : dialog->findChildren<QPushButton*>())
                        if (button->text().contains("Acknowledge"))
                            button->click();
                } else {
                    controller.confirmDefects();
                }
                require(confirmed && document.stackJson() == before,
                        "confirmation notification cannot replace document");
                QObject::disconnect(connection);
            }
            if (mode == "c") {
                require(reportShown && controller.grade() == "C" && controller.exitCode() == 3 &&
                            controller.reportBytes().contains("MULTI_MATERIAL_CUT_UNSUPPORTED"),
                        "C rejection preserved");
                if (dialog)
                    for (auto* button : dialog->findChildren<QPushButton*>())
                        if (button->text().contains("Acknowledge"))
                            require(!button->isEnabled(), "C cannot confirm");
            }
            if (mode == "missing")
                require(controller.reportBytes().isEmpty(), "missing report retained as empty");
            if (mode == "corrupt")
                require(controller.reportBytes() == "{", "corrupt report bytes retained");
            if (mode == "missing" || mode == "corrupt" || mode == "invalid")
                require(!controller.error().isEmpty(), "validation error retained");
            if (mode != "failed-start")
                require(controller.stderrText().contains(mode), "stderr retained");
            for (const auto& line : controller.stderrText().split('\n'))
                if (line.startsWith("staged-output: "))
                    require(!QFileInfo(QString::fromUtf8(line.mid(15))).absoluteDir().exists(),
                            "staging cleaned after completion or cancel");
        };
        for (const auto& mode : {"success", "b", "c", "missing", "corrupt", "invalid", "hang"})
            run(mode);
        run("failed-start", "/nonexistent/chipcae-import");
        require(controller.exitCode() == -1 && !controller.error().isEmpty(), "launch error");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
