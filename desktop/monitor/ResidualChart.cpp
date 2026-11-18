#include "ResidualChart.hpp"

#include <QStringList>
#include <cmath>
#include <map>
#include <vtkAxis.h>
#include <vtkChartXY.h>
#include <vtkContextScene.h>
#include <vtkContextView.h>
#include <vtkDoubleArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkPlot.h>
#include <vtkPlotPoints.h>
#include <vtkTable.h>
namespace chipcae::desktop {
MonitorLogPanel::MonitorLogPanel(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
}
void MonitorLogPanel::setEntries(const std::vector<MonitorLogEntry>& entries) {
    entries_ = entries;
    rebuildText();
}
void MonitorLogPanel::setMinimumLevel(int level) {
    minimumLevel_ = level;
    rebuildText();
}
void MonitorLogPanel::clearLevelFilter() {
    minimumLevel_.reset();
    rebuildText();
}
void MonitorLogPanel::rebuildText() {
    QStringList lines;
    for (const auto& entry : entries_) {
        if (entry.diagnostic)
            lines.append("[diag] " + entry.message);
        else if (!minimumLevel_ || entry.level >= *minimumLevel_)
            lines.append(
                QString("level=%1 rank=%2 %3").arg(entry.level).arg(entry.rank).arg(entry.message));
    }
    displayedText_ = lines.join('\n');
    setPlainText(displayedText_);
}

ResidualChart::ResidualChart(QWidget* parent) : QVTKOpenGLNativeWidget(parent) {
    vtkNew<vtkGenericOpenGLRenderWindow> window;
    setRenderWindow(window);
    view_ = vtkSmartPointer<vtkContextView>::New();
    view_->SetRenderWindow(window);
    view_->SetInteractor(interactor());
    chart_ = vtkSmartPointer<vtkChartXY>::New();
    view_->GetScene()->AddItem(chart_);
    chart_->GetAxis(vtkAxis::BOTTOM)->SetTitle("Iteration");
    chart_->GetAxis(vtkAxis::LEFT)->SetTitle("Residual norm");
    chart_->SetShowLegend(true);
}
ResidualChart::~ResidualChart() = default;
vtkChartXY* ResidualChart::chart() const {
    return chart_;
}
int ResidualChart::seriesCount() const {
    return chart_->GetNumberOfPlots();
}
void ResidualChart::setSamples(const std::vector<ResidualSample>& samples) {
    samples_ = samples;
    chart_->ClearPlots();
    tables_.clear();
    std::map<QString, std::vector<ResidualSample>> groups;
    for (const auto& s : samples_) {
        if (!std::isfinite(s.norm))
            continue;
        auto key = QString::fromStdString(s.solver) + "/" + QString::fromStdString(s.field) +
                   " window=" + QString::number(s.window);
        if (s.time)
            key += " time=" + QString::number(*s.time, 'g', 17);
        groups[key].push_back(s);
    }
    for (const auto& [key, points] : groups) {
        auto table = vtkSmartPointer<vtkTable>::New();
        for (auto name : {"iteration", "norm", "flags"}) {
            vtkNew<vtkDoubleArray> column;
            column->SetName(name);
            table->AddColumn(column);
        }
        table->SetNumberOfRows(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            table->SetValue(i, 0, double(points[i].iteration));
            table->SetValue(i, 1, points[i].norm);
            table->SetValue(i, 2, double(points[i].flags));
        }
        auto* plot = chart_->AddPlot(points.size() == 1 ? vtkChart::POINTS : vtkChart::LINE);
        if (auto* markers = vtkPlotPoints::SafeDownCast(plot))
            markers->SetMarkerStyle(vtkPlotPoints::CIRCLE);
        plot->SetInputData(table, 0, 1);
        plot->SetLabel(key.toStdString());
        tables_.push_back(table);
    }
    update();
}
} // namespace chipcae::desktop
