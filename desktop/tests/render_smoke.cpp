#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vtkActor.h>
#include <vtkConeSource.h>
#include <vtkEGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkPNGReader.h>
#include <vtkPNGWriter.h>
#include <vtkPointData.h>
#include <vtkPolyDataMapper.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkWindowToImageFilter.h>
#include <vtkXOpenGLRenderWindow.h>

namespace {

const char* envOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

bool displayAvailable() {
    return envOrEmpty("DISPLAY")[0] != '\0';
}

// vtkEGLRenderWindow can SIGSEGV on Render() after a failed device init, so
// only construct it when an ICD is likely to exist *and* the caller asked for
// it. GitHub-hosted runners have no /dev/dri; CI uses Xvfb + X instead.
vtkSmartPointer<vtkRenderWindow> makeWindow() {
    const std::string requested = envOrEmpty("VTK_DEFAULT_OPENGL_WINDOW");
    const bool wantX = requested == "vtkXOpenGLRenderWindow" ||
                       (requested.empty() && displayAvailable() &&
                        std::strcmp(envOrEmpty("CHIPCAE_FORCE_EGL"), "1") != 0);
    if (wantX) {
        if (!displayAvailable()) {
            std::cerr << "vtkXOpenGLRenderWindow requested but DISPLAY is unset\n";
            return nullptr;
        }
        vtkNew<vtkXOpenGLRenderWindow> window;
        return window;
    }
    vtkNew<vtkEGLRenderWindow> window;
    return window;
}

} // namespace

int main() {
    auto window = makeWindow();
    if (window == nullptr) {
        return 1;
    }
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
    // Query capability before Render(): a failed EGL context then crashes inside
    // vtkOpenGLRenderWindow::Render on some runner images.
    if (!window->SupportsOpenGL()) {
        std::cerr << window->GetClassName()
                  << " OpenGL unavailable (DISPLAY=" << envOrEmpty("DISPLAY")
                  << " VTK_DEFAULT_OPENGL_WINDOW=" << envOrEmpty("VTK_DEFAULT_OPENGL_WINDOW")
                  << ")\n";
        return 1;
    }
    window->Render();
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
