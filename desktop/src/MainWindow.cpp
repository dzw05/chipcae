#include "MainWindow.hpp"

#include "CaseTreeView.hpp"

#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
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
