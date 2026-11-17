#include "MainWindow.hpp"

#include "CaseTreeView.hpp"
#include "ResidualChart.hpp"

#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
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
    connect(stop, &QPushButton::clicked, panel, [model, broker, runId, elapsed] {
        model->stop(*broker, runId);
        elapsed->start();
    });
    auto* timer = new QTimer(panel);
    connect(timer, &QTimer::timeout, panel, [model, status, chart, logs, elapsed, broker, runId] {
        model->poll();
        if (broker) {
            chipcae::desktop::jobs::JobRef ref;
            ref.set_run_id(runId.toStdString());
            try {
                auto job = broker->Get(ref);
                if (job.state() == chipcae::desktop::jobs::JOB_STATE_COMPLETED ||
                    job.state() == chipcae::desktop::jobs::JOB_STATE_FAILED)
                    model->processExited(job.native().exit_status_raw());
            } catch (const std::exception&) {
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
