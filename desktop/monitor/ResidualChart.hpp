#pragma once
#include "RunMonitor.hpp"

#include <QPlainTextEdit>
#include <QVTKOpenGLNativeWidget.h>
#include <optional>
#include <vtkSmartPointer.h>
class vtkChartXY;
class vtkContextView;
class vtkTable;
namespace chipcae::desktop {
// Test-first stub for the public panel interface.
class MonitorLogPanel : public QPlainTextEdit {
public:
    explicit MonitorLogPanel(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}
    void setEntries(const std::vector<MonitorLogEntry>&) {}
    void setMinimumLevel(int) {}
    void clearLevelFilter() {}
    std::optional<int> minimumLevel() const { return std::nullopt; }
    QString displayedText() const { return {}; }
};
class ResidualChart : public QVTKOpenGLNativeWidget {
public:
    explicit ResidualChart(QWidget* parent = nullptr);
    ~ResidualChart() override;
    void setSamples(const std::vector<ResidualSample>& samples);
    vtkChartXY* chart() const;
    const std::vector<ResidualSample>& samples() const { return samples_; }
    int seriesCount() const { return 0; } // Test-first stub.

private:
    vtkSmartPointer<vtkContextView> view_;
    vtkSmartPointer<vtkChartXY> chart_;
    std::vector<vtkSmartPointer<vtkTable>> tables_;
    std::vector<ResidualSample> samples_;
};
} // namespace chipcae::desktop
