#pragma once
#include <QProcess>
#include <QTimer>
// One running child per worker. All methods and signals belong to the owner Qt thread.
// Lines exclude LF/CRLF; the final unterminated fragment is emitted before finished.
class PreviewWorker : public QObject {
    Q_OBJECT
public:
    explicit PreviewWorker(QObject* parent = nullptr);
    ~PreviewWorker() override;
    bool start(const QString& program, const QStringList& arguments);
    bool start(const QString& program,
               const QStringList& arguments,
               const QProcessEnvironment& environment);
    void cancel(int graceMilliseconds = 1000);
signals:
    void started(qint64 processId);
    void stdoutLine(const QByteArray& line);
    void stderrLine(const QByteArray& line);
    void finished(int exitCode, QProcess::ExitStatus status);
    void errorOccurred(QProcess::ProcessError error, const QString& message);

private:
    void drain(bool standardError, bool flush = false);
    QProcess process_;
    QTimer killTimer_;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
};
