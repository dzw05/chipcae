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
class MonitorLogPanel : public QPlainTextEdit {
public:
    explicit MonitorLogPanel(QWidget* parent = nullptr);
    void setEntries(const std::vector<MonitorLogEntry>& entries);
    void setMinimumLevel(int level);
    void clearLevelFilter();
    std::optional<int> minimumLevel() const { return minimumLevel_; }
    QString displayedText() const { return displayedText_; }

private:
    void rebuildText();
    std::vector<MonitorLogEntry> entries_;
    std::optional<int> minimumLevel_ = std::nullopt;
    QString displayedText_;
};
class ResidualChart : public QVTKOpenGLNativeWidget {
public:
    explicit ResidualChart(QWidget* parent = nullptr);
    ~ResidualChart() override;
    void setSamples(const std::vector<ResidualSample>& samples);
    vtkChartXY* chart() const;
    const std::vector<ResidualSample>& samples() const { return samples_; }
    int seriesCount() const;

private:
    vtkSmartPointer<vtkContextView> view_;
    vtkSmartPointer<vtkChartXY> chart_;
    std::vector<vtkSmartPointer<vtkTable>> tables_;
    std::vector<ResidualSample> samples_;
};
} // namespace chipcae::desktop
