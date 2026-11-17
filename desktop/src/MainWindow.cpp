#include "MainWindow.hpp"

#include "CaseOutline.hpp"

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
    auto* outline = new chipcae::desktop::CaseOutline(form, split);
    split->insertWidget(0, outline);
    dock->setWidget(split);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    const QString repository = CHIPCAE_REPOSITORY;
    try {
        outline->setCaseDocument(chipcae::desktop::SchemaForm::caseSchemaPath(repository),
                                 chipcae::desktop::SchemaValidator::read(
                                     repository + "/schema/examples/fcbga_demo.case.json"));
        outline->setStackDocument(repository + "/schema/stack-0.2.schema.json",
                                  chipcae::desktop::SchemaValidator::read(
                                      repository + "/schema/examples/fcbga_demo.stack.json"));
    } catch (const std::exception& error) {
        dock->setToolTip(QString::fromUtf8(error.what()));
    }
    setWindowTitle("ChipCAE Desktop");
    resize(960, 640);
}
