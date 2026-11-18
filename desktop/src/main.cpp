#include "DesktopConfig.hpp"
#include "MainWindow.hpp"

#include <QApplication>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
    try {
        QStringList args;
        for (int i = 0; i < argc; ++i)
            args.append(QString::fromLocal8Bit(argv[i]));
        chipcae::desktop::DesktopConfig::setCurrent(chipcae::desktop::DesktopConfig::load(args));
        QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
        QApplication app(argc, argv);
        MainWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
