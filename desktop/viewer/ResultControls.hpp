#pragma once
#include <QWidget>
namespace chipcae::desktop {
class ResultSession;
class ResultView;
class ResultControls : public QWidget {
public:
    ResultControls(ResultView& view, ResultSession& session, QWidget* parent = nullptr);
};
} // namespace chipcae::desktop
