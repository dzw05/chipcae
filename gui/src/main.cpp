#include "chipcae/core/version.hpp"

#include <QApplication>
#include <QLabel>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QLabel label(QStringLiteral("chipcae %1")
                     .arg(QString::fromUtf8(chipcae::version_string().data(),
                                            static_cast<int>(chipcae::version_string().size()))));
    label.setWindowTitle(QStringLiteral("chipcae"));
    label.resize(360, 120);
    label.show();
    return app.exec();
}
