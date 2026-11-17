#include "PreviewWorker.hpp"
PreviewWorker::PreviewWorker(QObject* parent) : QObject(parent) {
    killTimer_.setSingleShot(true);
    connect(&process_, &QProcess::started, this, [this] { emit started(process_.processId()); });
    connect(&killTimer_, &QTimer::timeout, &process_, &QProcess::kill);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { drain(false); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { drain(true); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        emit errorOccurred(error, process_.errorString());
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        killTimer_.stop();
        drain(false, true);
        drain(true, true);
        emit finished(code, status);
    });
}
PreviewWorker::~PreviewWorker() {
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}
bool PreviewWorker::start(const QString& program, const QStringList& arguments) {
    return start(program, arguments, QProcessEnvironment::systemEnvironment());
}
bool PreviewWorker::start(const QString& program,
                          const QStringList& arguments,
                          const QProcessEnvironment& environment) {
    if (process_.state() != QProcess::NotRunning) {
        return false;
    }
    killTimer_.stop();
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.setProcessEnvironment(environment);
    process_.start(program, arguments);
    return true;
}
void PreviewWorker::cancel(int graceMilliseconds) {
    if (process_.state() != QProcess::NotRunning) {
        process_.terminate();
        killTimer_.start(qMax(0, graceMilliseconds));
    }
}
void PreviewWorker::drain(bool standardError, bool flush) {
    auto& buffer = standardError ? stderrBuffer_ : stdoutBuffer_;
    buffer += standardError ? process_.readAllStandardError() : process_.readAllStandardOutput();
    auto send = [this, standardError](QByteArray line) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (standardError) {
            emit stderrLine(line);
        } else {
            emit stdoutLine(line);
        }
    };
    qsizetype end;
    while ((end = buffer.indexOf('\n')) >= 0) {
        send(buffer.left(end));
        buffer.remove(0, end + 1);
    }
    if (flush && !buffer.isEmpty()) {
        send(buffer);
        buffer.clear();
    }
}
