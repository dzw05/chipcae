#include "ResidualChart.hpp"

#include <QApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <stdexcept>
#include <vtkChartXY.h>
#include <vtkContextScene.h>
#include <vtkContextView.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkNew.h>
#include <vtkOutputWindow.h>
#include <vtkPNGWriter.h>
#include <vtkPlot.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>
#include <vtkTable.h>
#include <vtkWindowToImageFilter.h>
using namespace chipcae::desktop;
class RenderErrors : public vtkOutputWindow {
public:
    static RenderErrors* New() { return new RenderErrors; }
    int errors = 0;
    void DisplayErrorText(const char* text) override {
        ++errors;
        vtkOutputWindow::DisplayErrorText(text);
    }
};
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes, bool append = false) {
    QFile f(path);
    check(f.open(QIODevice::WriteOnly | (append ? QIODevice::Append : QIODevice::Truncate)),
          "open");
    check(f.write(bytes) == bytes.size(), "write");
}
int main(int argc, char** argv) {
    const bool present = qEnvironmentVariableIsSet("CHIPCAE_MONITOR_PRESENT");
    if (present)
        qputenv("QT_QPA_PLATFORM", "xcb");
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    vtkNew<RenderErrors> renderErrors;
    vtkOutputWindow::SetInstance(renderErrors);
    QTemporaryDir dir;
    auto path = dir.filePath("events.caelog");
    chipcae::io::KernelHello hello;
    hello.run_id = "run";
    chipcae::io::Residual single{"s", "f", 2, 3, 0.5, 32, "", 0};
    chipcae::io::ResidualBatch batch{2, 1.5, {{"s", "f", 4, 0.2, 16, 0, 0}}, 0};
    std::vector<chipcae::io::EventPayload> payloads = {
        hello,
        chipcae::io::State{chipcae::io::State::Value::running, 2, 1.5},
        chipcae::io::Progress{2, 1.5, 0.1, 0.3, "step"},
        single,
        batch,
        chipcae::io::LogLine{-7, 1, "log", "", 0},
        chipcae::io::ControlAck{1, "id", 1, 2, "ok", 1.5},
        chipcae::io::KernelExit{0, 2, "end_reached"},
        chipcae::io::OutputAvailable{2, 1.5, "untrusted", ""}};
    {
        chipcae::io::EventLogWriter w(path.toStdString());
        quint64 seq = 0;
        for (auto& p : payloads)
            w.append({++seq, 123, p});
    }
    auto codec = makeEventCodec(EventFormat::LegacyCaelog1);
    auto decoded = codec->read(path, 0);
    check(decoded.events.size() == 9 && decoded.diagnostic.isEmpty(), "all payloads");
    check(std::get<chipcae::io::KernelHello>(decoded.events[0].payload).event_log_format == 1,
          "hello");
    const auto& statePayload = std::get<chipcae::io::State>(decoded.events[1].payload);
    check(statePayload.value == chipcae::io::State::Value::running && statePayload.window == 2 &&
              statePayload.time == 1.5,
          "state payload");
    check(std::get<chipcae::io::KernelHello>(decoded.events[0].payload).run_id == "run",
          "hello run ID");
    check(std::get<chipcae::io::Progress>(decoded.events[2].payload).fraction == 0.3, "progress");
    check(std::get<chipcae::io::Residual>(decoded.events[3].payload).flags == 32, "residual");
    check(std::get<chipcae::io::ResidualBatch>(decoded.events[4].payload) == batch, "batch");
    check(std::get<chipcae::io::LogLine>(decoded.events[5].payload).level == -7, "raw log level");
    check(std::get<chipcae::io::ControlAck>(decoded.events[6].payload).effective_time == 1.5,
          "ack");
    check(std::get<chipcae::io::KernelExit>(decoded.events[7].payload).reason == "end_reached",
          "exit");
    check(std::get<chipcae::io::OutputAvailable>(decoded.events[8].payload).path == "untrusted",
          "output");
    for (size_t i = 0; i < decoded.events.size(); ++i)
        check(decoded.events[i].seq == i + 1 && decoded.events[i].wall_time_unix_ns == 123,
              "sequence");
    check(makeEventCodec(EventFormat::Ccevlog)->read(path, 0).diagnostic.contains("unsupported"),
          "wire stub");
    auto badHello = dir.filePath("hello");
    hello.event_log_format = 2;
    {
        chipcae::io::EventLogWriter w(badHello.toStdString());
        w.append({1, 0, hello});
    }
    auto unsupported = codec->read(badHello, 0);
    check(unsupported.offset == 0 && unsupported.events.empty() &&
              unsupported.diagnostic.contains("unsupported"),
          "unsupported hello");
    QFile source(path);
    check(source.open(QIODevice::ReadOnly), "read source");
    auto bytes = source.readAll();
    auto tail = dir.filePath("tail");
    write(tail, bytes.first(12));
    EventTailer tailer(tail, EventFormat::LegacyCaelog1);
    check(tailer.poll().events.empty() && tailer.offset() == 0, "partial header");
    write(tail, bytes.mid(12, 8), true);
    check(tailer.poll().events.empty() && tailer.offset() == 0, "partial body");
    write(tail, bytes.mid(20), true);
    check(tailer.poll().events.size() == 9, "complete");
    auto end = tailer.offset();
    check(tailer.poll().events.empty(), "once");
    auto corrupt = bytes;
    corrupt[20] ^= 1;
    write(tail, corrupt, true);
    check(tailer.poll().diagnostic.contains("CRC") && tailer.offset() == end, "crc cursor");
    auto prefixBad = dir.filePath("prefix-bad");
    write(prefixBad, bytes + corrupt);
    EventTailer prefixTail(prefixBad, EventFormat::LegacyCaelog1);
    auto prefixResult = prefixTail.poll();
    check(prefixResult.events.size() == 9 && prefixResult.offset == quint64(bytes.size()) &&
              prefixResult.diagnostic.contains("CRC"),
          "valid prefix before bad record");
    auto malformedMagic = bytes;
    malformedMagic[0] = 'X';
    write(dir.filePath("magic"), malformedMagic);
    check(codec->read(dir.filePath("magic"), 0).diagnostic.contains("magic") &&
              codec->read(dir.filePath("magic"), 0).offset == 0,
          "bad magic");
    auto invalidPayload = bytes;
    auto firstLength = qFromLittleEndian<quint32>(invalidPayload.constData() + 8);
    invalidPayload[32] = char(99);
    quint32 crc = 0xffffffffu;
    for (quint32 i = 0; i < firstLength; ++i) {
        crc ^= quint8(invalidPayload[16 + i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    qToLittleEndian<quint32>(crc ^ 0xffffffffu, invalidPayload.data() + 12);
    write(dir.filePath("payload"), bytes + invalidPayload);
    auto badPayload = codec->read(dir.filePath("payload"), 0);
    check(badPayload.events.size() == 9 && badPayload.offset == quint64(bytes.size()) &&
              badPayload.diagnostic.contains("payload"),
          "bad payload retains prefix");
    auto oversized = bytes.first(16);
    oversized[8] = 1;
    oversized[9] = 0;
    oversized[10] = 0;
    oversized[11] = 4;
    write(dir.filePath("oversized"), oversized);
    check(codec->read(dir.filePath("oversized"), 0).diagnostic.contains("64 MiB"), "record limit");
    QFile::remove(tail);
    write(tail, bytes);
    check(tailer.poll().events.empty(), "replacement no replay");
    write(tail, bytes.first(20), true);
    check(tailer.poll().events.empty() && tailer.offset() == end, "truncated tail");
    RunMonitor stateMonitor(dir.path(), EventFormat::LegacyCaelog1);
    stateMonitor.consume({1, 0, chipcae::io::State{chipcae::io::State::Value::completed, 2, 1.5}});
    check(stateMonitor.state == "Completed", "completed state label");
    stateMonitor.consume({2, 0, chipcae::io::State{chipcae::io::State::Value::failed, 2, 1.5}});
    check(stateMonitor.state == "Failed", "failed state label");
    stateMonitor.consume(
        {3, 0, chipcae::io::State{static_cast<chipcae::io::State::Value>(6), 2, 1.5}});
    check(stateMonitor.state == "Failed" &&
              stateMonitor.diagnostics.contains("invalid state value"),
          "invalid state preserves label and reports diagnostic");
    RunMonitor monitor(dir.path(), EventFormat::LegacyCaelog1);
    monitor.trackStop(1, "id");
    monitor.checkTimeout(30000);
    check(monitor.stopStatus == "Pending" && !monitor.ack, "timeout pending");
    monitor.consume({10, 0, chipcae::io::ControlAck{1, "wrong", 1, 2, "", 1.5}});
    monitor.consume({11, 0, chipcae::io::ControlAck{2, "id", 1, 2, "", 1.5}});
    check(!monitor.ack, "both identifiers");
    monitor.poll();
    check(monitor.stopStatus == "APPLIED" && monitor.ack->effective_step == 2, "matching ack");
    check(monitor.state != "Completed", "kernel alone");
    monitor.processExited(0);
    check(monitor.state == "Completed", "joint completion");
    check(monitor.manifestLookups == 1 && monitor.manifest.empty(), "manifest authority");
    write(dir.filePath("manifest.json"), R"({"generation":7,"steps":[]})");
    monitor.consume({15, 0, chipcae::io::OutputAvailable{99, 99, "not-a-manifest", ""}});
    check(monitor.manifestLookups == 2 && monitor.manifest["generation"] == 7,
          "output reads manifest");
    check(monitor.residuals.size() == 2 && monitor.residuals[1].flags == 16, "batch retained");
    RunMonitor failed(dir.path(), EventFormat::LegacyCaelog1);
    failed.processExited(1);
    check(failed.state != "Failed", "process alone");
    failed.consume({1, 0, chipcae::io::KernelExit{0, 0, "end_reached"}});
    check(failed.state == "Failed", "joint failure");
    auto warnings = monitor.diagnostics.size();
    monitor.poll();
    check(monitor.diagnostics.size() == warnings, "absent control");
    auto control = dir.filePath("control.jsonl");
    write(control,
          "bad\n{\"cmd_seq\":\"1\",\"client_request_id\":\"id\",\"action\":\"STOP\"}\n{\"cmd_seq\":"
          "\"1\",\"client_request_id\":\"id\",\"action\":\"STOP\"}\n{");
    monitor.poll();
    check(monitor.commands.size() == 1 && monitor.diagnostics.size() == warnings + 1,
          "malformed duplicate partial");
    write(control, "\"cmd_seq\":\"2\",\"client_request_id\":\"two\",\"action\":\"STOP\"}\n", true);
    monitor.poll();
    check(monitor.commands.size() == 2 && monitor.diagnostics.size() == warnings + 1,
          "continued line");
    auto adapter = dir.filePath("solver");
    write(adapter,
          QByteArray("#!/bin/sh\nexec python3 '") + CHIPCAE_REPOSITORY +
              "/desktop/tools/mock_solver.py' \"$@\"\n");
    QFile::setPermissions(adapter, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    auto profile = dir.filePath("profile.json");
    write(
        profile,
        R"({"schema":"chipcae.run_profile/1.0","ranks":1,"backend":"cpu","gpus":0,"threads_per_rank":1,"launcher":"direct","x-mock-delay":1,"x-mock-windows":10})");
    LocalJobService broker({dir.filePath("runs"), adapter, "mpirun", false, true});
    jobs::SubmitRequest submit;
    submit.set_client_request_id(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    auto job = broker.Submit(
        submit, QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json", profile);
    check(job.state() == jobs::JOB_STATE_STARTING, "broker launch");
    RunMonitor controlled(QString::fromStdString(job.result_dir()), EventFormat::LegacyCaelog1);
    controlled.stop(broker, QString::fromStdString(job.run_id()));
    check(controlled.stopStatus == "Pending" && !controlled.ack, "append is pending");
    auto commandBytes = readJobFile(QString::fromStdString(job.result_dir()) + "/control.jsonl");
    auto command = QJsonDocument::fromJson(commandBytes).object();
    check(commandBytes.endsWith('\n') && command["cmd_seq"] == "1" && command["action"] == "STOP" &&
              !command["client_request_id"].toString().isEmpty(),
          "STOP JSON");
    check(QDateTime::fromString(command["issuedAt"].toString(), Qt::ISODateWithMs).isValid() &&
              command["issuedAt"].toString().endsWith('Z'),
          "RFC3339 timestamp");
    controlled.checkTimeout(30000);
    jobs::JobRef ref;
    ref.set_run_id(job.run_id());
    check(!terminal(broker.Get(ref).state()) && controlled.stopStatus == "Pending",
          "timeout does not kill");
    for (unsigned status = 1; status <= 3; ++status) {
        controlled.trackStop(1, "id");
        controlled.consume(
            {20 + status, 0, chipcae::io::ControlAck{1, "id", status, 5, "result", 2.5}});
        check(controlled.stopStatus == QStringList{"", "APPLIED", "NOOP", "REJECTED"}[status] &&
                  controlled.ack->effective_time == 2.5,
              "ACK status");
    }
    ResidualChart chart;
    chart.setSamples(monitor.residuals);
    check(chart.samples().size() == 2 && chart.samples()[0].iteration == 3 &&
              chart.samples()[0].norm == 0.5 && chart.samples()[0].flags == 32,
          "chart data");
    check(chart.chart()->GetNumberOfPlots() == 2, "series");
    if (present || app.arguments().contains("--present")) {
        chart.resize(600, 400);
        chart.show();
        QEventLoop loop;
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();
        check(chart.isValid(), "Qt chart OpenGL context");
        auto image = chart.grabFramebuffer();
        check(!image.isNull() && image.width() > 0, "embedded chart capture");
        bool varied = false;
        for (int y = 0; y < image.height() && !varied; ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixel(x, y) != image.pixel(0, 0)) {
                    varied = true;
                    break;
                }
        check(varied, "embedded chart has nonconstant pixels");
        check(image.save("desktop-monitor-qt.png"), "embedded capture artifact");
        chart.hide();
    }
    // Exercise the same chart with a standalone EGL context; Qt offscreen cannot host GL.
    vtkNew<vtkContextView> render;
    render->GetRenderWindow()->SetOffScreenRendering(1);
    render->GetRenderWindow()->SetSize(400, 300);
    render->GetScene()->AddItem(chart.chart());
    check(render->GetRenderWindow()->SupportsOpenGL(), "chart OpenGL");
    render->GetRenderWindow()->Render();
    vtkNew<vtkWindowToImageFilter> capture;
    capture->SetInput(render->GetRenderWindow());
    capture->Update();
    check(capture->GetOutput()->GetNumberOfPoints() == 120000, "nonempty chart capture");
    auto* pixels = capture->GetOutput()->GetPointData()->GetScalars();
    double range[2];
    pixels->GetRange(range);
    check(range[1] > range[0], "rendered chart has nonconstant pixels");
    for (vtkIdType i = 0; i < chart.chart()->GetNumberOfPlots(); ++i) {
        auto* table = chart.chart()->GetPlot(i)->GetInput();
        check(table && table->GetNumberOfRows() == 1 && table->GetNumberOfColumns() == 3,
              "chart point and flags columns");
    }
    const auto firstTable = chart.chart()->GetPlot(0)->GetInput();
    const auto secondTable = chart.chart()->GetPlot(1)->GetInput();
    check(firstTable->GetValue(0, 0).ToDouble() == 3 &&
              firstTable->GetValue(0, 1).ToDouble() == 0.5 &&
              firstTable->GetValue(0, 2).ToUnsignedInt() == 32,
          "single chart table");
    check(secondTable->GetValue(0, 0).ToDouble() == 4 &&
              secondTable->GetValue(0, 1).ToDouble() == 0.2 &&
              secondTable->GetValue(0, 2).ToUnsignedInt() == 16,
          "batch chart table");
    vtkNew<vtkPNGWriter> png;
    png->SetFileName("desktop-monitor.png");
    png->SetInputConnection(capture->GetOutputPort());
    png->Write();
    check(QFileInfo("desktop-monitor.png").size() > 0, "capture artifact");
    check(renderErrors->errors == 0, "no VTK rendering errors");
    return 0;
}
