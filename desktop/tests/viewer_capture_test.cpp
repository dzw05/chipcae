#include "ResultView.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    try {
        ResultSession session;
        ResultView view;
        auto capture = [&](const QString& input, const QString& name) {
            if (!session.open(input) || !view.loadStep(session, 0))
                throw std::runtime_error((session.error() + view.error()).toStdString());
            const auto path = QDir(argv[2]).filePath(name + ".png");
            if (!view.capture(path))
                throw std::runtime_error("PNG export failed");
            QImage image(path);
            if (image.isNull() || image.width() != 800 || image.height() != 600)
                throw std::runtime_error("PNG invalid");
            int changed = 0;
            auto background = image.pixel(0, 0);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixel(x, y) != background)
                        ++changed;
            if (changed < 100)
                throw std::runtime_error("capture blank");
        };
        for (auto name : {"default", "old_names", "hex_fallback", "s1_compat", "viewer_mesh"})
            capture(QDir(argv[1]).filePath(name), QString("capture-") + name);
        for (auto name : {"s1", "s1b"}) {
            const auto path = QString("/workspace/samples/") + name;
            if (QFileInfo::exists(path))
                capture(path, QString("capture-official-") + name);
            else
                std::cout << name << " absent: optional sample skipped\n";
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
