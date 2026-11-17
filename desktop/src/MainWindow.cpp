#include "MainWindow.hpp"

#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
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
    setWindowTitle("ChipCAE Desktop");
    resize(960, 640);
}
