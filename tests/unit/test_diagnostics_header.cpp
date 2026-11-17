#include "chipcae/geometry/diagnostics.hpp"

int main() {
    chipcae::geometry::Defect d;
    d.kind = chipcae::geometry::DefectKind::Gap;
    d.tier = chipcae::geometry::Tier::B;
    return d.face_index != 0;
}
