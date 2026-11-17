#include "MainWindow.hpp"

#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>
int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    MainWindow window;
    // Lifecycle only: offscreen Qt does not support QOpenGLWidget presentation.
    if (!window.centralWidget() || window.size() != QSize(960, 640)) {
        return 1;
    }
    if (app.arguments().contains("--present")) {
        window.show();
    }
    QTimer::singleShot(app.arguments().contains("--present") ? 1000 : 0, &app, &QApplication::quit);
    return app.exec();
}
