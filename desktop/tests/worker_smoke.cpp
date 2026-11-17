#include "PreviewWorker.hpp"

#include <QCoreApplication>
#include <QTimer>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    PreviewWorker worker;
    QList<QByteArray> out, err;
    int phase = 0;
    QObject::connect(&worker, &PreviewWorker::stdoutLine, &app, [&](const QByteArray& line) {
        out.append(line);
    });
    QObject::connect(&worker, &PreviewWorker::stderrLine, &app, [&](const QByteArray& line) {
        err.append(line);
    });
    QObject::connect(&worker,
                     &PreviewWorker::errorOccurred,
                     &app,
                     [&](QProcess::ProcessError error, const QString&) {
                         if (phase == 2 && error == QProcess::FailedToStart) {
                             app.exit(0);
                         } else if (phase != 1 || error != QProcess::Crashed) {
                             app.exit(3);
                         }
                     });
    QObject::connect(
        &worker, &PreviewWorker::finished, &app, [&](int code, QProcess::ExitStatus status) {
            if (phase == 0) {
                if (code != 0 || status != QProcess::NormalExit ||
                    out != QList<QByteArray>{"first", "partial", "tail"} ||
                    err != QList<QByteArray>{"error", "last"}) {
                    app.exit(1);
                    return;
                }
                phase = 1;
                worker.start("/bin/sh", {"-c", "trap '' TERM; exec sleep 20"});
                QTimer::singleShot(100, &worker, [&] { worker.cancel(50); });
            } else {
                phase = 2;
                worker.start("/nonexistent/chipcae-preview", {});
            }
        });
    worker.start("/bin/sh",
                 {"-c",
                  "printf 'first\\npar'; sleep 0.05; printf 'tial\\ntail'; "
                  "printf 'error\\nlast' >&2"});
    QTimer::singleShot(5000, &app, [&] { app.exit(2); });
    return app.exec();
}
