#pragma once
#include <QStringList>
#include <QWidget>
#include <optional>

class QLineEdit;

namespace chipcae::desktop {
// UI sidecar: nx/ny never modify stack polygons. levels=0 selects automatic refinement.
struct PreviewOptions {
    std::optional<int> nx, ny;
    int levels = 0;
    double maxRatio = 10;
    QString surface;
    QString validate() const;
    QStringList
    arguments(const QString& stack, const QString& output, const QString& polyset = {}) const;
    bool save(const QString& path) const;
    static PreviewOptions load(const QString& path);
};

class PreviewOptionsPanel : public QWidget {
    Q_OBJECT
public:
    explicit PreviewOptionsPanel(QWidget* parent = nullptr);
    void setOptions(const PreviewOptions& options);
    PreviewOptions options() const;
    QString validationMessage() const;
signals:
    void optionsEdited();

private:
    QLineEdit* nx_;
    QLineEdit* ny_;
    QLineEdit* levels_;
    QLineEdit* maxRatio_;
    QLineEdit* surface_;
};
} // namespace chipcae::desktop
