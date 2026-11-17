#include "MainWindow.hpp"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkActor.h>
#include <vtkConeSource.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
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
