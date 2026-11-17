#include "chipcae/geometry/diagnostics.hpp"

int main() {
    chipcae::geometry::StepDefect d;
    d.kind = chipcae::geometry::StepDefectKind::Gap;
    d.tier = chipcae::geometry::Tier::B;
    return d.face_index != 0;
}
