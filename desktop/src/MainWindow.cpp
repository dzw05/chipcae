#include "MainWindow.hpp"

#include "CaseTreeView.hpp"
#include "DefectsDialog.hpp"
#include "ImportController.hpp"
#include "ImportSourceDialog.hpp"
#include "ResidualChart.hpp"
#include "ResultControls.hpp"
#include "ResultView.hpp"
#include "StackEditor.hpp"

#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkConeSource.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    auto* menu = menuBar()->addMenu(tr("File"));
    auto report = [this](bool ok) {
        if (!ok && !document_.errors().empty())
            QMessageBox::warning(this, tr("Case document"), document_.errors().front().message);
    };
    menu->addAction(tr("Open"), this, [this, report] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Open case"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            report(openCase(path));
    });
    menu->addAction(tr("Save"), this, [this, report] {
        const auto path = QFileDialog::getSaveFileName(this, tr("Save case"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            report(saveCase(path));
    });
    menu->addAction(tr("Monitor run (explicit legacy CAELOG1)"), this, [this] {
        auto path = QFileDialog::getOpenFileName(this, tr("Select legacy CAELOG1 event log"));
        if (!path.isEmpty())
            monitorRun(QFileInfo(path).absolutePath(),
                       chipcae::desktop::EventFormat::LegacyCaelog1,
                       nullptr,
                       {},
                       QFileInfo(path).fileName());
    });
    menu->addAction(tr("Import ECAD…"), this, [this] {
        const auto path = chipcae::desktop::ImportSourceDialog::getImportPath(this);
        if (!path.isEmpty() && !importEcadPath(path))
            statusBar()->showMessage(tr("Cannot start import"));
    });
    menu->addAction(tr("Open Results…"), this, [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, tr("Open Results"), {}, "Manifest (*.json)");
        if (!path.isEmpty() && !openResultsPath(path))
            QMessageBox::warning(
                this, tr("Open Results"), tr("Cannot open result manifest or first step"));
    });
    menu->addAction(tr("Load handwritten stack template"), this, [this] {
        if (!loadHandwrittenStack())
            QMessageBox::warning(this, tr("Stack"), tr("Cannot load handwritten stack template"));
    });
    importer_ = new chipcae::desktop::ImportController(document_, this);
    auto* defects = new chipcae::desktop::DefectsDialog(*importer_, this);
    connect(importer_, &chipcae::desktop::ImportController::defectsReady, defects, &QDialog::open);
    auto* importDock = new QDockWidget(tr("Import log"), this);
    auto* importLog = new QPlainTextEdit(importDock);
    importLog->setReadOnly(true);
    importDock->setWidget(importLog);
    addDockWidget(Qt::BottomDockWidgetArea, importDock);
    importDock->hide();
    auto* cancel = new QPushButton(tr("Cancel import"), this);
    statusBar()->addPermanentWidget(cancel);
    cancel->hide();
    connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelImport);
    auto* logTimer = new QTimer(this);
    auto logged = std::make_shared<qsizetype>(0);
    auto appendLog = [this, importLog, logged] {
        const auto bytes = importer_->stderrText();
        if (bytes.size() > *logged) {
            importLog->moveCursor(QTextCursor::End);
            importLog->insertPlainText(QString::fromUtf8(bytes.mid(*logged)));
            *logged = bytes.size();
        }
    };
    connect(logTimer, &QTimer::timeout, this, appendLog);
    connect(importer_,
            &chipcae::desktop::ImportController::finished,
            this,
            [this, cancel, logTimer, appendLog](bool replaced) {
                appendLog();
                logTimer->stop();
                cancel->hide();
                statusBar()->showMessage(replaced ? tr("Import complete") : tr("Import ended"));
            });
    connect(importer_, &chipcae::desktop::ImportController::userConfirmed, this, [this] {
        statusBar()->showMessage(tr("Defects acknowledged"));
    });
    logTimer->setInterval(100);
    beginImportUi_ = [this, cancel, logTimer, importDock, importLog, logged] {
        *logged = 0;
        importLog->clear();
        importDock->show();
        cancel->show();
        statusBar()->showMessage(tr("导入中"));
        logTimer->start();
    };
    auto* view = new QVTKOpenGLNativeWidget(this);
    vtkNew<vtkGenericOpenGLRenderWindow> window;
    view->setRenderWindow(window);
    vtkNew<vtkRenderer> renderer;
    window->AddRenderer(renderer);
    vtkNew<vtkConeSource> cone;
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(cone->GetOutputPort());
    vtkNew<vtkActor> actor;
    actor->SetMapper(mapper);
    renderer->AddActor(actor);
    renderer->SetBackground(0.08, 0.12, 0.18);
    renderer->ResetCamera();
    setCentralWidget(view);
    auto* dock = new QDockWidget("Case (schema example)", this);
    auto* split = new QSplitter(dock);
    auto* form = new chipcae::desktop::SchemaForm(split);
    auto* model = new chipcae::desktop::CaseTreeModel(&document_, split);
    const QString repository = CHIPCAE_REPOSITORY;
    auto* outline = new chipcae::desktop::CaseTreeView(model, form, repository + "/schema", split);
    split->insertWidget(0, outline);
    dock->setWindowTitle("Case");
    dock->setWidget(split);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    auto* stackDock = new QDockWidget(tr("Stack"), this);
    stackDock->setWidget(new chipcae::desktop::StackEditor(document(), stackDock));
    addDockWidget(Qt::RightDockWidgetArea, stackDock);
    auto* edits = menuBar()->addMenu(tr("Case"));
    edits->addAction(tr("Apply properties"), outline, [outline] { outline->commitEdit(); });
    edits->addAction(tr("Cancel properties"), outline, [outline] { outline->cancelEdit(); });
    edits->addAction(tr("Uniform temperature"), model, [model] { model->setThermalMode(false); });
    edits->addAction(tr("Thermal"), model, [model] { model->setThermalMode(true); });
    edits->addAction(tr("Add convection load (zmax)"), model, [model] {
        model->addLoad(chipcae::desktop::Json::parse(
            R"({"type":"convection","target":"face:zmax","h":2,"T_inf":298.15})"));
    });
    edits->addAction(
        tr("Add fixed face (zmin)"), model, [model] { model->addFixedFace("face:zmin"); });
    edits->addAction(tr("Remove selected"), outline, [outline, model] {
        model->remove(
            outline->currentIndex().data(chipcae::desktop::CaseTreeModel::IdentityRole).toString());
    });
    connect(&document_, &chipcae::desktop::CaseDocument::dirtyChanged, this, [this](bool dirty) {
        setWindowModified(dirty);
    });
    setWindowTitle("ChipCAE Desktop[*]");

    resize(960, 640);
}

void MainWindow::monitorRun(const QString& directory,
                            chipcae::desktop::EventFormat format,
                            chipcae::desktop::LocalJobService* broker,
                            const QString& runId,
                            const QString& eventFile) {
    const QPointer<chipcae::desktop::LocalJobService> guardedBroker(broker);
    auto model = std::make_shared<chipcae::desktop::RunMonitor>(directory, format, eventFile);
    auto* dock = new QDockWidget("Run monitor: " + directory, this);
    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    auto* status = new QLabel(panel);
    auto* chart = new chipcae::desktop::ResidualChart(panel);
    auto* logs = new QPlainTextEdit(panel);
    logs->setReadOnly(true);
    auto* stop = new QPushButton("STOP", panel);
    stop->setEnabled(broker && !runId.isEmpty());
    layout->addWidget(status);
    layout->addWidget(chart);
    layout->addWidget(logs);
    layout->addWidget(stop);
    auto elapsed = std::make_shared<QElapsedTimer>();
    connect(stop, &QPushButton::clicked, panel, [model, guardedBroker, runId, elapsed] {
        if (!guardedBroker)
            return;
        model->stop(*guardedBroker, runId);
        elapsed->start();
    });
    auto* timer = new QTimer(panel);
    connect(timer,
            &QTimer::timeout,
            panel,
            [model, status, chart, logs, elapsed, guardedBroker, runId] {
                model->poll();
                if (guardedBroker) {
                    chipcae::desktop::jobs::JobRef ref;
                    ref.set_run_id(runId.toStdString());
                    try {
                        auto job = guardedBroker->Get(ref);
                        if (job.state() == chipcae::desktop::jobs::JOB_STATE_COMPLETED ||
                            job.state() == chipcae::desktop::jobs::JOB_STATE_FAILED)
                            model->processExited(job.native().exit_status_raw());
                    } catch (const std::exception& e) {
                        const auto diagnostic =
                            QString("cannot get job status: %1").arg(QString::fromUtf8(e.what()));
                        if (!model->diagnostics.contains(diagnostic))
                            model->diagnostics.append(diagnostic);
                    }
                }
                if (elapsed->isValid())
                    model->checkTimeout(elapsed->elapsed());
                auto text = QString("%4 | %1 | %2% | STOP %3")
                                .arg(model->state)
                                .arg(model->progress.fraction * 100)
                                .arg(model->stopStatus)
                                .arg(model->format == chipcae::desktop::EventFormat::LegacyCaelog1
                                         ? "Legacy CAELOG1"
                                         : "CCEVLOG (unsupported)");
                if (model->ack)
                    text += QString(" step=%1 time=%2")
                                .arg(model->ack->effective_step)
                                .arg(model->ack->effective_time);
                status->setText(text);
                logs->setPlainText((model->logs + model->diagnostics).join('\n'));
                if (chart->samples().size() != model->residuals.size())
                    chart->setSamples(model->residuals);
            });
    timer->start(250);
    dock->setWidget(panel);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
}

MainWindow::~MainWindow() {
    // Controls capture their session, and the importer references the document.
    delete resultDock_;
    delete takeCentralWidget();
    delete importer_;
}

bool MainWindow::importEcadPath(const QString& input,
                                const QString& program,
                                const QStringList& prefix) {
    if (importer_->busy() || !chipcae::desktop::ImportSourceDialog::isSupportedImportPath(input))
        return false;
    auto output = std::make_unique<QTemporaryDir>();
    if (!output->isValid())
        return false;
    const auto path = output->filePath("import.stack.json");
    importOutput_ = std::move(output);
    beginImportUi_();
    return importer_->start(input,
                            path,
                            importOutput_->filePath("import.polyset"),
                            importOutput_->filePath("defects.json"),
                            program,
                            prefix);
}
void MainWindow::cancelImport() {
    importer_->cancel();
}

bool MainWindow::loadHandwrittenStack(const QString& path) {
    QFile file(path.isEmpty()
                   ? QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.stack.json"
                   : path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    try {
        return document_.setStackJson(chipcae::desktop::Json::parse(file.readAll().toStdString()));
    } catch (const std::exception&) {
        return false;
    }
}

bool MainWindow::openResultsPath(const QString& manifestPath) {
    auto session = std::make_unique<chipcae::desktop::ResultSession>();
    if (!session->open(manifestPath) || session->steps().isEmpty())
        return false;
    auto view = std::make_unique<chipcae::desktop::ResultView>();
    if (!view->loadStep(*session, session->steps().first().toObject()["n"].toInteger()))
        return false;
    delete resultDock_;
    delete takeCentralWidget();
    session_ = std::move(session);
    resultView_ = view.release();
    setCentralWidget(resultView_);
    resultDock_ = new QDockWidget(tr("Result controls"), this);
    resultDock_->setWidget(
        new chipcae::desktop::ResultControls(*resultView_, *session_, resultDock_));
    addDockWidget(Qt::RightDockWidgetArea, resultDock_);
    return true;
}
