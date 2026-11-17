#pragma once
#include "JobTypes.hpp"
namespace chipcae::desktop {
class ProfileMapper {
public:
    static jobs::RunProfile map(const QJsonObject& profile, bool cudaAvailable);
    static LaunchCommand command(const QJsonObject& profile,
                                 const LauncherConfig& config,
                                 const QString& casePath,
                                 const QString& profilePath,
                                 const QString& runDirectory);
};
} // namespace chipcae::desktop
