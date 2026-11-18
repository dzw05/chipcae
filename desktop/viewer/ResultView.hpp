#pragma once

#include "ResultSession.hpp"

#include <QWidget>
#include <array>
#include <memory>
#include <vtkSmartPointer.h>

class vtkDataSet;
class vtkUnstructuredGrid;
class vtkRenderWindow;
namespace chipcae::desktop {
// Quantitative operations use original SI coordinates, independently of display exaggeration.
struct VolumeResult {
    double volume = 0;
    double integral = 0;
    QString error;
};
enum class ResultColorField { Temperature, DisplacementMagnitude, WarpageUz, LayerId, MaterialId };
struct ColorRange {
    double min = 0;
    double max = 0;
    bool valid = false;
    QString unit;
};
class ResultView : public QWidget {
    Q_OBJECT
public:
    explicit ResultView(QWidget* parent = nullptr);
    ~ResultView() override;
    bool loadStep(ResultSession& session, qint64 step);
    void setColorField(ResultColorField field);
    ResultColorField colorField() const;
    ColorRange colorRange() const;
    QString colorBarTitle() const;
    QString tmaxText() const;
    bool setProvenance(const QJsonObject& provenance);
    void clearProvenance();
    QString provenanceWatermark() const;
    QString provenanceBanner() const;
    void setWarpageScale(double scale);
    void setBlockVisible(const QString& block, bool visible);
    void setLayer(int id); // 0 = all; ResultSession layer ids start at 1.
    void setShowVoid(bool show);
    void setClip(bool enabled,
                 std::array<double, 3> origin = {},
                 std::array<double, 3> normal = {1, 0, 0});
    QString summary(const QString& officialName) const;
    QString pickedId(const QString& block, vtkIdType cell) const;
    QString pick(int x, int y);
    vtkDataSet* displayed(const QString& block) const;
    vtkRenderWindow* renderWindow() const;
    bool capture(const QString& path);
    const QString& error() const;
    static vtkSmartPointer<vtkUnstructuredGrid> tetrahedralize(vtkDataSet* input, QString& error);
    static VolumeResult quantify(vtkDataSet* input,
                                 const QString& scalar = {},
                                 bool clip = false,
                                 std::array<double, 3> origin = {},
                                 std::array<double, 3> normal = {1, 0, 0});

signals:
    void displayChanged();
    void elementPicked(const QString& gid);

private:
    void rebuild();
    struct State;
    std::unique_ptr<State> d;
};
} // namespace chipcae::desktop
