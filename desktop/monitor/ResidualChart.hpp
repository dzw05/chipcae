#pragma once
#include "RunMonitor.hpp"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>
class vtkChartXY;
class vtkContextView;
class vtkTable;
namespace chipcae::desktop {
class ResidualChart : public QVTKOpenGLNativeWidget {
public:
    explicit ResidualChart(QWidget* parent = nullptr);
    ~ResidualChart() override;
    void setSamples(const std::vector<ResidualSample>& samples);
    vtkChartXY* chart() const;
    const std::vector<ResidualSample>& samples() const { return samples_; }

private:
    vtkSmartPointer<vtkContextView> view_;
    vtkSmartPointer<vtkChartXY> chart_;
    std::vector<vtkSmartPointer<vtkTable>> tables_;
    std::vector<ResidualSample> samples_;
};
} // namespace chipcae::desktop
