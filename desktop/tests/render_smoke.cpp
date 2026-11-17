#include <iostream>
#include <vtkActor.h>
#include <vtkConeSource.h>
#include <vtkEGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkPNGReader.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderer.h>
#include <vtkWindowToImageFilter.h>
int main() {
    vtkNew<vtkEGLRenderWindow> window;
    window->SetOffScreenRendering(1);
    window->SetSize(320, 240);
    vtkNew<vtkRenderer> renderer;
    window->AddRenderer(renderer);
    vtkNew<vtkConeSource> cone;
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(cone->GetOutputPort());
    vtkNew<vtkActor> actor;
    actor->SetMapper(mapper);
    renderer->AddActor(actor);
    renderer->SetBackground(0.05, 0.1, 0.2);
    renderer->ResetCamera();
    window->Render();
    if (!window->SupportsOpenGL()) {
        std::cerr << "EGL OpenGL unavailable\n";
        return 1;
    }
    vtkNew<vtkWindowToImageFilter> capture;
    capture->SetInput(window);
    capture->ReadFrontBufferOff();
    capture->Update();
    vtkNew<vtkPNGWriter> writer;
    writer->SetFileName("desktop-render.png");
    writer->SetInputConnection(capture->GetOutputPort());
    writer->Write();
    vtkNew<vtkPNGReader> reader;
    reader->SetFileName("desktop-render.png");
    reader->Update();
    auto* image = reader->GetOutput();
    auto* size = image->GetDimensions();
    double range[2];
    image->GetPointData()->GetScalars()->GetRange(range);
    std::cout << window->GetClassName() << " PNG " << size[0] << 'x' << size[1] << " range "
              << range[0] << ':' << range[1] << '\n';
    return size[0] == 320 && size[1] == 240 && range[1] > range[0] ? 0 : 1;
}
