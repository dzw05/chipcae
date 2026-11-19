#include "MainWindow.hpp"

#include "AssemblyImport.hpp"
#include "CaseTreeView.hpp"
#include "DefectsDialog.hpp"
#include "DesktopConfig.hpp"
#include "ImportController.hpp"
#include "ImportSourceDialog.hpp"
#include "MaterialMapMissingDialog.hpp"
#include "PreviewController.hpp"
#include "ProfileMapper.hpp"
#include "ResidualChart.hpp"
#include "ResultControls.hpp"
#include "ResultView.hpp"
#include "StackEditor.hpp"
#include "SubmitDialog.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkConeSource.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
MainWindow::MainWindow(QWidget* parent)
    : MainWindow(QCoreApplication::arguments(),
                 QProcessEnvironment::systemEnvironment(),
                 QCoreApplication::applicationDirPath(),
                 QString(CHIPCAE_REPOSITORY) + "/schema",
                 parent) {}
QString MainWindow::schemaDirectory() const {
    return schemaDirectory_;
}
MainWindow::MainWindow(const QStringList& args,
                       const QProcessEnvironment& env,
                       const QString& executableDir,
                       const QString& compileTimeFallback,
                       QWidget* parent)
    : QMainWindow(parent), schemaDirectory_(chipcae::desktop::DesktopConfig::locateSchemaDir(
                               args, env, executableDir, compileTimeFallback)) {
    if (schemaDirectory_.isEmpty())
        schemaDirectory_ = QString(CHIPCAE_REPOSITORY) + "/schema";
    preview_ = new chipcae::desktop::PreviewController(this);
    auto* previewOptionsDock = new QDockWidget(tr("Preview options"), this);
    previewOptionsPanel_ = new chipcae::desktop::PreviewOptionsPanel(previewOptionsDock);
    previewOptionsPanel_->setObjectName("previewOptionsPanel");
    previewOptionsDock->setWidget(previewOptionsPanel_);
    addDockWidget(Qt::RightDockWidgetArea, previewOptionsDock);
    chipcae::desktop::SubmitDialog defaults;
    profile_ = defaults.profile();
    auto* menu = menuBar()->addMenu(tr("File"));
    menu->addAction(tr("Preview"), this, [this] { previewMesh(); });
    writePreviewToCaseAction_ =
        menu->addAction(QStringLiteral("写入 case"), this, &MainWindow::writePreviewToCase);
    writePreviewToCaseAction_->setEnabled(false);
    connect(preview_, &chipcae::desktop::PreviewController::finished, this, [this](bool success) {
        if (success) {
            previewDigestValid_ = true;
            previewMeshOptions_ = document_.meshOptions();
            writePreviewToCaseAction_->setEnabled(true);
            auto view = std::make_unique<chipcae::desktop::PreviewView>();
            if (!view->show(*preview_)) {
                statusBar()->showMessage(tr("Cannot display preview mesh"));
                return;
            }
            // Controls reference the result view and session; destroy them first.
            delete resultDock_;
            resultDock_ = nullptr;
            watermarkLabel_ = nullptr;
            delete takeCentralWidget();
            resultView_ = nullptr;
            setCentralWidget(view.release());
            refreshSourceUi();
            previewDisplayed_ = true;
            previewDisplayStale_ = false;
        } else {
            clearPreviewMeshOptions();
        }
    });
    menu->addAction(tr("Submit"), this, [this] {
        chipcae::desktop::SubmitDialog dialog(this);
        dialog.setProfile(profile_);
        dialog.setMeshNx(meshNx_);
        if (dialog.exec() == QDialog::Accepted) {
            profile_ = dialog.profile();
            meshNx_ = dialog.meshNx();
            submitCurrentCase();
        }
    });
    menu->addAction(tr("Save Screenshot"), this, [this] {
        auto path = QFileDialog::getSaveFileName(this, tr("Save Screenshot"), {}, "PNG (*.png)");
        if (!path.isEmpty())
            captureResults(path);
    });
    auto report = [this](bool ok) {
        if (!ok && !document_.errors().empty())
            QMessageBox::warning(this, tr("Case document"), document_.errors().front().message);
    };
    auto* openCaseAction = menu->addAction(tr("Open"), this, [this, report] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Open case"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            report(openCase(path));
    });
    menu->addAction(tr("Save"), this, [this, report] {
        const auto path = QFileDialog::getSaveFileName(this, tr("Save case"), {}, "JSON (*.json)");
        if (!path.isEmpty())
            report(saveCase(path));
    });
    menu->addAction(tr("Monitor run (CAELOG1)"), this, [this] {
        auto path = QFileDialog::getOpenFileName(this, tr("Select CAELOG1 event log"));
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
    menu->addAction(tr("Import assembly…"), this, [this] {
        QFileDialog dialog(this, tr("Import assembly"));
        dialog.setOption(QFileDialog::DontUseNativeDialog);
        dialog.setFileMode(QFileDialog::ExistingFile);
        dialog.setNameFilters(chipcae::desktop::assemblyNameFilters());
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
            return;
        const auto path = dialog.selectedFiles().first();
        if (chipcae::desktop::isAssemblyPath(path) && !importAssemblyPath(path))
            statusBar()->showMessage(tr("Cannot start import"));
    });
    menu->addAction(tr("Open Results…"), this, [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, tr("Open Results"), {}, "Manifest (*.json)");
        if (!path.isEmpty() && !openResultsPath(path) && !lastOpenCancelledForMeshDigest())
            QMessageBox::warning(
                this, tr("Open Results"), tr("Cannot open result manifest or first step"));
    });
    auto* handwrittenAction = menu->addAction(tr("Load handwritten stack template"), this, [this] {
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
                if (replaced && currentImportKind_ == chipcae::desktop::ImportKind::Assembly) {
                    const auto report = QJsonDocument::fromJson(importer_->reportBytes()).object();
                    if (auto* model = findChild<chipcae::desktop::CaseTreeModel*>())
                        model->applyPartSummary(report.value("parts").toArray());
                }
                if (importer_->outcome() != chipcae::desktop::ImportOutcome::NeedsConfirmation)
                    currentImportKind_.reset();
                if (replaced)
                    clearPreviewMeshOptions();
                appendLog();
                logTimer->stop();
                cancel->hide();
                for (auto* action : importBlockedActions_)
                    action->setEnabled(true);
                if (auto* stack = findChild<chipcae::desktop::StackEditor*>())
                    stack->setEnabled(true);
                statusBar()->showMessage(replaced ? tr("Import complete") : tr("Import ended"));
            });
    connect(importer_, &chipcae::desktop::ImportController::userConfirmed, this, [this] {
        statusBar()->showMessage(tr("Defects acknowledged"));
    });
    logTimer->setInterval(100);
    importBlockedActions_ = {openCaseAction, handwrittenAction};
    beginImportUi_ = [this, cancel, logTimer, importDock, importLog, logged] {
        *logged = 0;
        importLog->clear();
        importDock->show();
        cancel->show();
        statusBar()->showMessage(tr("导入中"));
        for (auto* action : importBlockedActions_)
            action->setEnabled(false);
        if (auto* stack = findChild<chipcae::desktop::StackEditor*>())
            stack->setEnabled(false);
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
    auto* outline = new chipcae::desktop::CaseTreeView(model, form, schemaDirectory_, split);
    split->insertWidget(0, outline);
    dock->setWindowTitle("Case");
    dock->setWidget(split);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    auto* stackDock = new QDockWidget(tr("Stack"), this);
    auto* stack = new chipcae::desktop::StackEditor(document(), stackDock);
    connect(stack, &chipcae::desktop::StackEditor::previewInvalidated, this, [this] {
        if (previewDisplayed_)
            previewDisplayStale_ = true;
    });
    stackDock->setWidget(stack);
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

    auto* sourceDock = new QDockWidget(tr("Result source"), this);
    sourceDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    sourceLabel_ = new QLabel(sourceDock);
    sourceLabel_->setWordWrap(true);
    sourceDock->setWidget(sourceLabel_);
    addDockWidget(Qt::TopDockWidgetArea, sourceDock);
    refreshSourceUi();
    resize(960, 640);
}

chipcae::desktop::MonitorLogPanel* MainWindow::monitorLogPanel() const {
    return monitorLogPanel_.data();
}

void MainWindow::monitorRun(const QString& directory,
                            chipcae::desktop::EventFormat format,
                            chipcae::desktop::LocalJobService* broker,
                            const QString& runId,
                            const QString& eventFile) {
    if (!runId.isEmpty() && monitoredRuns_.contains(runId))
        return;
    if (!runId.isEmpty())
        monitoredRuns_.insert(runId);
    if (!session_) {
        provenance_ = chipcae::desktop::Provenance::read(directory);
        refreshSourceUi();
    }
    const QPointer<chipcae::desktop::LocalJobService> guardedBroker(broker);
    auto model = std::make_shared<chipcae::desktop::RunMonitor>(directory, format, eventFile);
    auto* dock = new QDockWidget("Run monitor: " + directory, this);
    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    auto* status = new QLabel(panel);
    auto* chart = new chipcae::desktop::ResidualChart(panel);
    auto* logs = new chipcae::desktop::MonitorLogPanel(panel);
    auto* filter = new QComboBox(panel);
    filter->setObjectName("monitorLogLevelFilter");
    filter->addItems({tr("全部级别"), tr("level ≥ 0")});
    connect(filter, qOverload<int>(&QComboBox::currentIndexChanged), panel, [logs](int index) {
        if (index == 1)
            logs->setMinimumLevel(0);
        else
            logs->clearLevelFilter();
    });
    auto* stop = new QPushButton("STOP", panel);
    stop->setEnabled(broker && !runId.isEmpty());
    layout->addWidget(status);
    layout->addWidget(chart);
    layout->addWidget(filter);
    layout->addWidget(logs);
    layout->addWidget(stop);
    auto elapsed = std::make_shared<QElapsedTimer>();
    connect(stop, &QPushButton::clicked, panel, [model, guardedBroker, runId, elapsed, stop] {
        if (!guardedBroker)
            return;
        model->stop(*guardedBroker, runId);
        elapsed->start();
        stop->setEnabled(false);
    });
    auto* cancel = new QPushButton("Cancel", panel);
    cancel->hide();
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, panel, [guardedBroker, runId] {
        if (!guardedBroker)
            return;
        chipcae::desktop::jobs::CancelRequest request;
        request.set_run_id(runId.toStdString());
        guardedBroker->Cancel(request);
    });
    auto opened = std::make_shared<bool>(false);
    auto* timer = new QTimer(panel);
    connect(
        timer,
        &QTimer::timeout,
        panel,
        [this,
         model,
         status,
         chart,
         logs,
         elapsed,
         guardedBroker,
         runId,
         eventFile,
         cancel,
         stop,
         opened] {
            model->poll();
            if (guardedBroker) {
                chipcae::desktop::jobs::JobRef ref;
                ref.set_run_id(runId.toStdString());
                try {
                    auto job = guardedBroker->Get(ref);
                    if (chipcae::desktop::terminal(job.state())) {
                        stop->setEnabled(false);
                        cancel->hide();
                        if (!*opened) {
                            *opened = true;
                            if (job.state() == chipcae::desktop::jobs::JOB_STATE_COMPLETED)
                                openResultsPath(QString::fromStdString(job.result_dir()) +
                                                "/manifest.json");
                            else if (job.state() == chipcae::desktop::jobs::JOB_STATE_FAILED) {
                                const auto dir =
                                    chipcae::desktop::DesktopConfig::current().precomputed().runDir;
                                if (!dir.isEmpty() && QFile::exists(dir + "/manifest.json") &&
                                    openResultsPath(dir + "/manifest.json")) {
                                    fallback_ = true;
                                    refreshSourceUi();
                                }
                            }
                        }
                    } else if (elapsed->isValid() && elapsed->elapsed() >= 30000 && !model->ack)
                        cancel->show();
                    if (job.state() == chipcae::desktop::jobs::JOB_STATE_COMPLETED ||
                        job.state() == chipcae::desktop::jobs::JOB_STATE_FAILED)
                        model->processExited(job.native().exit_status_raw());
                } catch (const std::exception& e) {
                    const auto diagnostic =
                        QString("cannot get job status: %1").arg(QString::fromUtf8(e.what()));
                    model->appendDiagnostic(diagnostic);
                }
            }
            if (elapsed->isValid())
                model->checkTimeout(elapsed->elapsed());
            auto text = QString("%4 | %1 | %2% | STOP %3")
                            .arg(model->state)
                            .arg(model->progress.fraction * 100)
                            .arg(model->stopStatus)
                            .arg(model->format == chipcae::desktop::EventFormat::LegacyCaelog1
                                     ? "CAELOG1"
                                     : "CCEVLOG (unsupported)");
            if (model->ack)
                text += QString(" step=%1 time=%2")
                            .arg(model->ack->effective_step)
                            .arg(model->ack->effective_time);
            text += " | " + eventFile + " | " + sourceBadgeText();
            monitorStatus_ = text;
            status->setText(text);
            logs->setEntries(model->logEntries());
            if (chart->samples().size() != model->residuals.size())
                chart->setSamples(model->residuals);
        });
    timer->start(250);
    dock->setWidget(panel);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    monitorLogPanel_ = logs;
}

MainWindow::~MainWindow() {
    // Controls capture their session, and the importer references the document.
    delete resultDock_;
    delete takeCentralWidget();
    delete importer_;
    delete preview_;
    delete service_;
}

bool MainWindow::importBusy() const {
    return importer_ && importer_->busy();
}

bool MainWindow::openCase(const QString& path) {
    if (importBusy())
        return false;
    if (!document_.load(path))
        return false;
    clearPreviewMeshOptions();
    casePath_ = path;
    manualStack_ = false;
    refreshSourceUi();
    return true;
}

bool MainWindow::importEcadPath(const QString& input,
                                const QString& program,
                                const QStringList& prefix) {
    if (importer_->busy() || chipcae::desktop::isAssemblyPath(input) ||
        !chipcae::desktop::ImportSourceDialog::isSupportedImportPath(input))
        return false;
    auto output = std::make_unique<QTemporaryDir>();
    if (!output->isValid())
        return false;
    const auto path = output->filePath("import.stack.json");
    importOutput_ = std::move(output);
    const auto& entry = chipcae::desktop::DesktopConfig::current().importEntry();
    chipcae::desktop::ImportRequest request;
    request.input = input;
    request.output = path;
    request.materialMap = entry.materialMap;
    request.polyset = importOutput_->filePath("import.polyset");
    request.report = importOutput_->filePath("defects.json");
    request.kind = chipcae::desktop::ImportKind::Ecad;
    request.program = program == "chipcae-import" ? entry.program : program;
    request.prefix = prefix.isEmpty() && program == "chipcae-import" ? entry.prefix : prefix;
    const auto connection = connect(
        importer_,
        &chipcae::desktop::ImportController::finished,
        this,
        [this, request](bool) mutable {
            if (!importer_->needsMaterialMapReselect())
                return;
            auto path = chipcae::desktop::MaterialMapMissingDialog::prompt(this);
            if (!path.isEmpty()) {
                request.materialMap = path;
                startImportRequest(request);
            }
        },
        Qt::SingleShotConnection);
    const bool started = startImportRequest(request);
    if (!started)
        disconnect(connection);
    return started;
}
bool MainWindow::importAssemblyPath(const QString& input,
                                    const QString& program,
                                    const QStringList& prefix) {
    if (importer_->busy() || !chipcae::desktop::isAssemblyPath(input))
        return false;
    auto output = std::make_unique<QTemporaryDir>();
    if (!output->isValid())
        return false;
    importOutput_ = std::move(output);
    const auto& entry = chipcae::desktop::DesktopConfig::current().importEntry();
    chipcae::desktop::ImportRequest request;
    request.kind = chipcae::desktop::ImportKind::Assembly;
    request.input = input;
    request.output = importOutput_->filePath("import.stack.json");
    request.report = importOutput_->filePath("defects.json");
    request.program = program == "chipcae-import" ? entry.program : program;
    request.prefix = prefix.isEmpty() && program == "chipcae-import" ? entry.prefix : prefix;
    return startImportRequest(request);
}
bool MainWindow::startImportRequest(const chipcae::desktop::ImportRequest& request) {
    currentImportKind_ = request.kind;
    if (!importer_->start(request)) {
        currentImportKind_.reset();
        importOutput_.reset();
        return false;
    }
    beginImportUi_();
    return true;
}
void MainWindow::cancelImport() {
    importer_->cancel();
}

bool MainWindow::loadHandwrittenStack(const QString& path) {
    if (importBusy())
        return false;
    QFile file(path.isEmpty()
                   ? QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.stack.json"
                   : path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    try {
        if (!document_.setStackJson(chipcae::desktop::Json::parse(file.readAll().toStdString())))
            return false;
        clearPreviewMeshOptions();
        manualStack_ = true;
        refreshSourceUi();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void MainWindow::setMeshDigestDialogChoice(MeshDigestDialogChoice choice) {
    meshDigestDialogChoice_ = choice;
}
MainWindow::MeshDigestDialogChoice MainWindow::meshDigestDialogChoice() const {
    return meshDigestDialogChoice_;
}
chipcae::desktop::MeshSha256Compare MainWindow::lastMeshDigestCompare() const {
    return lastMeshDigestCompare_;
}
QString MainWindow::lastMeshDigestDetail() const {
    return lastMeshDigestDetail_;
}
bool MainWindow::lastOpenCancelledForMeshDigest() const {
    return lastOpenCancelledForMeshDigest_;
}
bool MainWindow::writePreviewToCase() {
    if (!previewMeshOptions_)
        return false;
    const auto options = *previewMeshOptions_;
    document_.setMeshOptions(options);
    const bool finalized = document_.finalizeMeshForSubmit();
    if (!finalized && !document_.errors().empty())
        statusBar()->showMessage(document_.errors().front().message);
    return finalized;
}
bool MainWindow::writePreviewToCaseEnabled() const {
    return previewMeshOptions_.has_value();
}
void MainWindow::clearPreviewMeshOptions() {
    previewDigestValid_ = false;
    previewMeshOptions_.reset();
    writePreviewToCaseAction_->setEnabled(false);
}

bool MainWindow::openResultsPath(const QString& manifestPath) {
    lastOpenCancelledForMeshDigest_ = false;
    auto session = std::make_unique<chipcae::desktop::ResultSession>();
    if (!session->open(manifestPath) || session->steps().isEmpty())
        return false;
    auto view = std::make_unique<chipcae::desktop::ResultView>();
    if (!view->loadStep(*session, session->steps().first().toObject()["n"].toInteger()))
        return false;
    const auto& digest = preview_->digest();
    const QString hash =
        previewDigestValid_ && digest ? QString::fromStdString(digest->mesh_sha256) : QString();
    lastMeshDigestCompare_ = session->comparePreviewMeshSha256(hash, &lastMeshDigestDetail_);
    using chipcae::desktop::MeshSha256Compare;
    if (lastMeshDigestCompare_ == MeshSha256Compare::PreviewMissing)
        lastMeshDigestDetail_.clear();
    if (lastMeshDigestCompare_ == MeshSha256Compare::Mismatch ||
        lastMeshDigestCompare_ == MeshSha256Compare::ManifestMissing) {
        const QString warning = tr("网格摘要不一致：\n%1").arg(lastMeshDigestDetail_);
        setProperty("meshDigestWarningText", warning);
        bool accept = meshDigestDialogChoice_ == MeshDigestDialogChoice::Accept;
        if (meshDigestDialogChoice_ == MeshDigestDialogChoice::Unset)
            accept = QMessageBox::warning(this,
                                          QStringLiteral("网格摘要不一致"),
                                          warning,
                                          QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok;
        if (!accept) {
            lastOpenCancelledForMeshDigest_ = true;
            return false;
        }
    }
    delete resultDock_;
    watermarkLabel_ = nullptr;
    delete takeCentralWidget();
    provenance_ = chipcae::desktop::Provenance::read(QFileInfo(manifestPath).absolutePath());
    fallback_ = false;
    session_ = std::move(session);
    resultView_ = view.release();
    setCentralWidget(resultView_);
    resultDock_ = new QDockWidget(tr("Result controls"), this);
    resultDock_->setWidget(
        new chipcae::desktop::ResultControls(*resultView_, *session_, resultDock_));
    addDockWidget(Qt::RightDockWidgetArea, resultDock_);
    watermarkLabel_ = new QLabel(resultView_);
    watermarkLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    watermarkLabel_->move(20, 20);
    watermarkLabel_->setStyleSheet("color: #cc8800; font-size: 28px; font-weight: bold;");
    refreshSourceUi();
    return true;
}

bool MainWindow::caseIsOpen() const {
    return !casePath_.isEmpty() || !document_.stackJson().empty();
}
bool MainWindow::workBusy() const {
    if (preview_->running())
        return true;
    if (!service_ || runId_.isEmpty())
        return false;
    chipcae::desktop::jobs::JobRef ref;
    ref.set_run_id(runId_.toStdString());
    try {
        return !chipcae::desktop::terminal(service_->Get(ref).state());
    } catch (const std::exception& e) {
        statusBar()->showMessage(tr("Cannot get job status: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
}
chipcae::desktop::PreviewOptionsPanel* MainWindow::previewOptionsPanel() const {
    return previewOptionsPanel_;
}
chipcae::desktop::PreviewView* MainWindow::previewView() const {
    return qobject_cast<chipcae::desktop::PreviewView*>(centralWidget());
}
bool MainWindow::previewDisplayStale() const {
    return previewDisplayStale_;
}
bool MainWindow::previewMesh(const QString& program, const QStringList& prefix) {
    const auto message = previewOptionsPanel_->validationMessage();
    if (!message.isEmpty()) {
        statusBar()->showMessage(message);
        return false;
    }
    return previewMesh(program, prefix, previewOptionsPanel_->options());
}
bool MainWindow::previewMesh(const QString& program,
                             const QStringList& prefix,
                             const chipcae::desktop::PreviewOptions& options) {
    if (workBusy() || !caseIsOpen())
        return false;
    clearPreviewMeshOptions();
    previewOutput_ = std::make_unique<QTemporaryDir>();
    // Saving a temporary bundle preserves external polyset references for preview.
    if (document_.toJson().contains("schema")) {
        const auto snapshot = previewOutput_->filePath("preview.case.json");
        if (document_.save(snapshot)) {
            const auto value =
                QJsonDocument::fromJson(chipcae::desktop::readJobFile(snapshot)).object();
            const auto ref = value["stack"].toObject()["ref"].toString();
            if (!ref.isEmpty()) {
                const auto& entry = chipcae::desktop::DesktopConfig::current().previewEntry();
                return preview_->start(
                    &document_,
                    previewOutput_->filePath(ref),
                    previewOutput_->filePath("mesh.vtkhdf"),
                    options,
                    program == "chipcae-preview" ? entry.program : program,
                    program == "chipcae-preview" && prefix.isEmpty() ? entry.prefix : prefix);
            }
        }
    }
    auto stack = previewOutput_->filePath("stack.json");
    QFile f(stack);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QByteArray::fromStdString(document_.stackJson().to_string()));
    f.close();
    const auto& entry = chipcae::desktop::DesktopConfig::current().previewEntry();
    return preview_->start(&document_,
                           stack,
                           previewOutput_->filePath("mesh.vtkhdf"),
                           options,
                           program == "chipcae-preview" ? entry.program : program,
                           program == "chipcae-preview" && prefix.isEmpty() ? entry.prefix
                                                                            : prefix);
}
bool MainWindow::submitCurrentCase() {
    using namespace chipcae::desktop;
    if (!caseIsOpen() || workBusy())
        return false;
    jobs::RunProfile mapped;
    try {
        mapped = ProfileMapper::map(profile_, false);
    } catch (...) {
        return false;
    }
    const auto& config = DesktopConfig::current();
    try {
        QDir().mkpath(config.runRoot());
        document_.setValue("/mesh/nx", Json(meshNx_));
        const auto caseFile = config.runRoot() + "/submit-case.json";
        if (!document_.save(caseFile))
            return false;
        const auto profileFile = config.runRoot() + "/submit-profile.json";
        writeJobFile(profileFile, QJsonDocument(profile_).toJson());
        if (!service_) {
            auto launcher = config.launcherConfig();
            launcher.eventsFile = config.eventsFile();
            service_ = new LocalJobService(launcher, this);
        }
        jobs::SubmitRequest request;
        request.set_client_request_id(
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
        *request.mutable_profile() = mapped;
        auto job = service_->Submit(request, caseFile, profileFile);
        runId_ = QString::fromStdString(job.run_id());
        runDir_ = QString::fromStdString(job.result_dir());
        monitorRun(runDir_, EventFormat::LegacyCaelog1, service_, runId_, config.eventsFile());
        return true;
    } catch (const std::exception& e) {
        statusBar()->showMessage(QString::fromUtf8(e.what()));
        return false;
    }
}
QString MainWindow::sourceBadgeText() const {
    auto text = provenance_.label();
    if (manualStack_ || provenance_.degraded == "manual_stack")
        text += " | 叠层为手写模板";
    return text;
}
QString MainWindow::sourceBannerText() const {
    QString text;
    if (provenance_.source != "real")
        text = provenance_.label();
    if (manualStack_ || provenance_.degraded == "manual_stack")
        text += " 叠层为手写模板";
    if (fallback_) {
        const auto generatedAt =
            provenance_.generatedAt.isEmpty() ? QString("生成时间未知") : provenance_.generatedAt;
        text.prepend("现场求解失败，显示预计算结果 " + generatedAt + " ");
    }
    return text;
}
QString MainWindow::provenanceWatermark() const {
    return provenance_.watermark();
}
QString MainWindow::summaryWithSource(const QString& name) const {
    if (!resultView_)
        return {};
    return resultView_->summary(name) + (provenance_.source == "real"          ? QString{}
                                         : provenance_.source == "precomputed" ? "（预计算）"
                                                                               : "（MOCK）");
}
bool MainWindow::captureResults(const QString& path) {
    return resultView_ &&
           chipcae::desktop::writeCapturedPng(*resultView_, path, provenanceWatermark());
}

void MainWindow::refreshSourceUi() {
    if (sourceLabel_) {
        QString text = sourceBadgeText() + "\n" + sourceBannerText();
        if (resultView_)
            for (auto name : {"warpage_pv", "U_mag_max", "T_max"})
                text += "\n" + summaryWithSource(name);
        sourceLabel_->setText(text);
    }
    if (watermarkLabel_) {
        watermarkLabel_->setText(provenanceWatermark());
        watermarkLabel_->adjustSize();
        watermarkLabel_->show();
    }
}
