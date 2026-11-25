#include "CaseTreeView.hpp"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMenu>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
using namespace chipcae::desktop;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
static QModelIndex
childNamed(const CaseTreeModel& tree, const QModelIndex& parent, const QString& name) {
    for (int i = 0; i < tree.rowCount(parent); ++i) {
        auto child = tree.index(i, 0, parent);
        if (child.data().toString() == name)
            return child;
    }
    return {};
}
static void assemblyTreeAcceptance() {
    CaseDocument doc;
    require(doc.load(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
            "assembly document");
    QFile file(QString(ASSEMBLY_FIXTURE_DIR) + "/stack-with-bodies.json");
    require(file.open(QIODevice::ReadOnly), "assembly stack fixture");
    const auto stack = Json::parse(file.readAll().toStdString());
    require(doc.checkImportedStack(stack) && doc.replaceImportedStack(stack),
            "assembly fixture is a valid imported stack");
    CaseTreeModel tree(&doc);
    const auto before = doc.toJson(), stackBefore = doc.stackJson();
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            ++failures;
            std::cerr << "FAIL assembly tree: " << message << '\n';
        }
    };
    tree.refresh();
    check(tree.index(0, 0).data() == "Stack layers" && tree.index(1, 0).data() == "Bodies" &&
              tree.index(2, 0).data() == "Materials",
          "root order Stack layers, Bodies, Materials");
    const auto layers = tree.index(0, 0);
    check(tree.rowCount(layers) == 2, "Stack layers contains only layers, no stack metadata");
    for (int i = 0; i < tree.rowCount(layers); ++i)
        check(tree.index(i, 0, layers).data(CaseTreeModel::PointerRole) ==
                  QString("/layers/%1").arg(i),
              "layer order excludes metadata");
    const auto bodies = childNamed(tree, {}, "Bodies");
    check(bodies.isValid() && tree.locate("", "bodies-index") == bodies, "Bodies root and locate");
    check(tree.rowCount(bodies) == 4, "four case-sensitive body groups, no anonymous index group");
    for (int i = 0; i < 2; ++i) {
        const auto layer = tree.locate(QString("/layers/%1").arg(i), "stack");
        check(layer.isValid() &&
                  layer.data(CaseTreeModel::PointerRole) == QString("/layers/%1").arg(i),
              "layer pointer");
        const auto regions = childNamed(tree, layer, "Regions");
        check(regions.isValid(), "layer Regions container");
        check(tree.rowCount(regions) == (i == 0 ? 5 : 2), "same body merges within layer");
        const QStringList names = i == 0 ? QStringList{"lid", "lid", "lid.bond", "cap", "Lid", ""}
                                         : QStringList{"lid", "cap"};
        for (int j = 0; j < names.size(); ++j) {
            auto group = childNamed(tree,
                                    regions,
                                    names[j].isEmpty() ? QString::fromUtf8("（无 body）")
                                                       : "body:" + names[j]);
            check(group.isValid() && group.data(CaseTreeModel::PointerRole).toString().isEmpty() &&
                      group.data(CaseTreeModel::SourceRole) == "stack",
                  "stack display group metadata");
            const auto pointer = QString("/layers/%1/regions/%2").arg(i).arg(j);
            auto region = tree.locate(pointer, "stack");
            check(region.isValid() && region.parent() == group &&
                      region.data(CaseTreeModel::PointerRole) == pointer,
                  "exact region pointer under body group");
            if (!names[j].isEmpty()) {
                auto indexGroup = childNamed(tree, bodies, names[j]);
                auto indexed = tree.locate(pointer, "bodies-index");
                check(indexGroup.isValid() && indexed.isValid() && indexed.parent() == indexGroup &&
                          indexed != region && indexed.data(CaseTreeModel::PointerRole) == pointer,
                      "index pointer agrees across sources");
            }
        }
    }
    const auto lid = childNamed(tree, bodies, "lid");
    check(lid.isValid() && tree.rowCount(lid) == 3, "cross-layer lid merges into one group");
    tree.applyPartSummary(QJsonArray{QJsonObject{{"id", "cap"}, {"material_approx", true}}});
    tree.applyPartSummary(QJsonArray{QJsonObject{{"id", "lid"}, {"material_approx", true}},
                                     QJsonObject{{"id", "unknown"}, {"material_approx", true}}});
    auto annotatedBodies = tree.locate("", "bodies-index");
    for (int i = 0; i < tree.rowCount(annotatedBodies); ++i) {
        const auto group = tree.index(i, 0, annotatedBodies);
        const bool approx = group.data().toString().startsWith("lid") &&
                            !group.data().toString().startsWith("lid.bond");
        check(group.data().toString().contains("近似材料") == approx &&
                  group.data(CaseTreeModel::BodyAnnotationRole).toString() ==
                      (approx ? QString("近似材料") : QString()),
              "annotation visible and dedicated role");
        check(group.data(CaseTreeModel::PointerRole).toString().isEmpty() &&
                  group.data(CaseTreeModel::SourceRole) == "bodies-index",
              "index group metadata");
    }
    check(lid.isValid() && tree.locate("/layers/0/regions/0", "bodies-index")
                               .parent()
                               .data()
                               .toString()
                               .contains("近似材料"),
          "lid annotated by summary");
    auto stackGroup = tree.locate("/layers/0/regions/0", "stack").parent();
    check(!stackGroup.data().toString().contains("近似材料") &&
              stackGroup.data(CaseTreeModel::BodyAnnotationRole).toString().isEmpty(),
          "stack groups have no annotation");
    for (const auto& duplicate :
         {QJsonObject{{"id", "lid"}, {"material_approx", false}}, QJsonObject{{"id", "lid"}}}) {
        tree.applyPartSummary(
            QJsonArray{QJsonObject{{"id", "lid"}, {"material_approx", true}}, duplicate});
        check(lid.data().toString() == "lid （近似材料）" &&
                  lid.data(CaseTreeModel::BodyAnnotationRole) == "近似材料",
              "duplicate false or missing approximation retains earlier true");
    }
    tree.applyPartSummary(QJsonArray{QJsonObject{{"id", "lid"}, {"material_approx", true}}});
    tree.applyPartSummary(QJsonArray{});
    check(lid.data().toString() == "lid" &&
              lid.data(CaseTreeModel::BodyAnnotationRole).toString().isEmpty(),
          "empty summary clears label and annotation");
    tree.applyPartSummary(QJsonArray{QJsonObject{{"id", "lid"}, {"material_approx", true}}});
    tree.applyPartSummary(QJsonArray{QJsonObject{{"id", "lid"}, {"material_approx", false}}});
    auto cleanLid = childNamed(tree, tree.locate("", "bodies-index"), "lid");
    check(cleanLid.isValid() &&
              cleanLid.data(CaseTreeModel::BodyAnnotationRole).toString().isEmpty(),
          "nonempty false summary clears annotation");
    for (const auto& node : {tree.locate("", "bodies-index"),
                             cleanLid,
                             tree.locate("/layers/0/regions/0", "bodies-index")}) {
        check(node.isValid(), "readonly index node exists");
        auto identity = node.data(CaseTreeModel::IdentityRole).toString();
        check(!tree.edit(identity, Json::object()) && !tree.remove(identity),
              "index rejects edit and removal");
    }
    check(doc.toJson() == before && doc.stackJson() == stackBefore,
          "summaries and index edits never mutate document");
    require(failures == 0, "assembly tree acceptance failures");
}

static void caseSchemaPathAcceptance() {
    const auto defaultPath = QStringLiteral(CHIPCAE_REPOSITORY "/schema/case-1.0.schema.json");
    CaseDocument doc;
    require(doc.load(QStringLiteral(CHIPCAE_REPOSITORY "/schema/examples/fcbga_demo.case.json")),
            "schema path fixture loads");
    CaseTreeModel tree(&doc);
    require(tree.caseSchemaPath() == defaultPath, "default case schema path matches exactly");
    tree.setThermalMode(true);
    require(doc.validate().empty(), "schema path thermal fixture is valid");
    const auto convection =
        Json::parse(R"({"type":"convection","target":"face:zmax","h":2,"T_inf":300})");
    auto requireLocated =
        [](const CaseTreeModel& model, const QString& pointer, const QString& identity) {
            require(!identity.isEmpty(), "schema path addition returns an identity");
            const auto node = model.locate(pointer);
            require(node.isValid() && node.data(CaseTreeModel::PointerRole) == pointer &&
                        node.data(CaseTreeModel::SourceRole) == "case" &&
                        node.data(CaseTreeModel::IdentityRole) == identity,
                    "schema path addition locates exact pointer and identity");
        };
    auto addConvection = [&] {
        const auto pointer = QString("/loads/%1").arg(doc.toJson().at("loads").size());
        const auto identity = tree.addLoad(convection);
        requireLocated(tree, pointer, identity);
        return identity;
    };
    auto addFixedFace = [&] {
        const auto constraints = doc.toJson().at("constraints");
        const auto count = constraints.contains("fixed") ? constraints.at("fixed").size() : 0;
        const auto pointer = QString("/constraints/fixed/%1").arg(count);
        const auto identity = tree.addFixedFace("face:zmin");
        requireLocated(tree, pointer, identity);
        return identity;
    };
    const auto loadIdentity = addConvection();
    addFixedFace();

    QTemporaryDir schemas;
    require(schemas.isValid(), "temporary schema directory is valid");
    for (const auto* name : {"case-1.0.schema.json", "parameters.schema.json"})
        require(QFile::copy(QStringLiteral(CHIPCAE_REPOSITORY "/schema/") + name,
                            schemas.filePath(name)),
                "copy official case schema and its parameters dependency");
    const auto copiedPath = QFileInfo(schemas.filePath("case-1.0.schema.json")).absoluteFilePath();
    const auto beforePathChange = doc.toJson();
    for (int i = 0; i < 2; ++i) {
        tree.setCaseSchemaPath(copiedPath);
        require(tree.caseSchemaPath() == copiedPath, "repeated schema path is stored verbatim");
        require(doc.toJson() == beforePathChange, "setting schema path leaves document unchanged");
    }
    addConvection();
    addFixedFace();

    const auto missingPath = QFileInfo(schemas.filePath("missing.schema.json")).absoluteFilePath();
    tree.setCaseSchemaPath(missingPath);
    require(tree.caseSchemaPath() == missingPath, "missing schema path is stored verbatim");
    const auto beforeMissingSchema = doc.toJson();
    require(tree.addLoad(convection).isEmpty(), "missing schema rejects valid convection load");
    require(doc.toJson() == beforeMissingSchema, "missing schema load leaves document unchanged");
    require(tree.addFixedFace("face:zmin").isEmpty(), "missing schema rejects valid fixed face");
    require(doc.toJson() == beforeMissingSchema,
            "missing schema fixed face leaves document unchanged");
    require(
        !tree.edit(loadIdentity,
                   Json::parse(R"({"type":"convection","target":"face:zmax","h":3,"T_inf":300})")),
        "missing schema rejects valid load edit");
    require(doc.toJson() == beforeMissingSchema, "missing schema edit leaves document unchanged");
    require(!tree.remove(loadIdentity), "missing schema rejects valid load removal");
    require(doc.toJson() == beforeMissingSchema,
            "missing schema removal leaves document unchanged");
    tree.setThermalMode(false);
    require(doc.toJson() == beforeMissingSchema,
            "missing schema thermal switch leaves document unchanged");

    tree.setCaseSchemaPath(copiedPath);
    require(tree.caseSchemaPath() == copiedPath, "restore copied schema path");
    addConvection();
    addFixedFace();
    tree.setCaseSchemaPath(QString());
    require(tree.caseSchemaPath() == defaultPath, "empty schema path restores exact default");
    addConvection();

    tree.setCaseSchemaPath(missingPath);
    const auto beforeOtherModel = doc.toJson();
    CaseDocument otherDoc;
    require(
        otherDoc.load(QStringLiteral(CHIPCAE_REPOSITORY "/schema/examples/fcbga_demo.case.json")),
        "independent schema path fixture loads");
    CaseTreeModel other(&otherDoc);
    require(other.caseSchemaPath() == defaultPath, "other model retains default schema path");
    other.setThermalMode(true);
    const auto otherPointer = QString("/loads/%1").arg(otherDoc.toJson().at("loads").size());
    requireLocated(other, otherPointer, other.addLoad(convection));
    require(tree.caseSchemaPath() == missingPath && tree.addLoad(convection).isEmpty(),
            "other model success does not replace missing schema path");
    require(doc.toJson() == beforeOtherModel,
            "independent model leaves rejected document unchanged");
}

static void loadHeatSourceFixture(CaseDocument& doc) {
    require(doc.load(QString(CHIPCAE_REPOSITORY) + "/schema/examples/fcbga_demo.case.json"),
            "heat source fixture loads");
}

static void requireHeatSourceRejected(CaseTreeModel& tree, const QString& layer, double power) {
    const auto before = tree.document()->toJson();
    require(tree.addHeatSource(layer, power).isEmpty(),
            "rejected heat source returns empty identity");
    require(tree.document()->toJson() == before, "rejected heat source leaves document unchanged");
}

static void requireHeatSourceAdded(const CaseDocument& doc,
                                   const QString& identity,
                                   std::size_t previousCount,
                                   const Json& expected) {
    require(!identity.isEmpty(), "heat source addition returns an identity");
    const auto loads = doc.toJson().at("loads");
    require(loads.size() == previousCount + 1, "heat source appends exactly one load");
    auto load = loads.at(previousCount);
    require(load.contains("id") && load.at("id").is_string(), "heat source has string id");
    const auto id = QString::fromStdString(load.at("id").as<std::string>());
    require(QRegularExpression("^[A-Za-z0-9_.-]{1,64}$").match(id).hasMatch(),
            "heat source id matches schema pattern");
    require(identity == "case:" + id, "heat source returns exact case identity");
    load.erase("id");
    require(load == expected, "heat source object equals specified type, target and power");
    require(load.at("power").as<double>() == expected.at("power").as<double>(),
            "heat source power matches exactly");
    require(doc.validate().empty(), "heat source document is valid");
}

static void heatSourceOutsideThermalAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    requireHeatSourceRejected(tree, "die_layer", 1.0);
}

static void heatSourceAddedAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    tree.setThermalMode(true);
    require(doc.validate().empty(), "thermal heat source fixture is valid");
    const auto count = doc.toJson().at("loads").size();
    const auto identity = tree.addHeatSource("die_layer", 1.0);
    requireHeatSourceAdded(
        doc,
        identity,
        count,
        Json::parse(R"({"type":"heat_source","target":"layer:die_layer","power":1.0})"));
    requireHeatSourceRejected(tree, "die_layer", 1.0);
}

static void heatSourceEdgeCasesAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    tree.setThermalMode(true);
    // Seed through the existing API so rejection checks also run before addHeatSource exists.
    require(!tree.addLoad(Json::parse(
                              R"({"type":"heat_source","target":"layer:die_layer","power":1.0})"))
                 .isEmpty(),
            "seed existing heat source");
    require(doc.validate().empty(), "edge case fixture is valid");
    requireHeatSourceRejected(tree, "die_layer", 1.0);
    requireHeatSourceRejected(tree, "unknown_layer", 1.0);
    requireHeatSourceRejected(tree, "Die_layer", 1.0);
    requireHeatSourceRejected(tree, "", 1.0);
    // Use a layer without a source so duplicate detection cannot mask nonfinite power checks.
    requireHeatSourceRejected(tree, "substrate", std::numeric_limits<double>::quiet_NaN());
    requireHeatSourceRejected(tree, "substrate", std::numeric_limits<double>::infinity());
    requireHeatSourceRejected(tree, "substrate", -std::numeric_limits<double>::infinity());
    auto count = doc.toJson().at("loads").size();
    auto identity = tree.addHeatSource("substrate", 1.0);
    requireHeatSourceAdded(
        doc,
        identity,
        count,
        Json::parse(R"({"type":"heat_source","target":"layer:substrate","power":1.0})"));
    int heatSources = 0;
    const auto loads = doc.toJson().at("loads");
    for (const auto& load : loads.array_range())
        if (load.at("type") == "heat_source")
            ++heatSources;
    require(heatSources == 2, "different layers have exactly two heat sources");
    count = doc.toJson().at("loads").size();
    identity = tree.addHeatSource("bump_eq", -2.0);
    requireHeatSourceAdded(
        doc,
        identity,
        count,
        Json::parse(R"({"type":"heat_source","target":"layer:bump_eq","power":-2.0})"));
}

static void heatSourceVolumetricDuplicateAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    tree.setThermalMode(true);
    require(!tree.addLoad(Json::parse(
                              R"({"type":"heat_source","target":"layer:die_layer","q_vol":2.0})"))
                 .isEmpty(),
            "seed volumetric source on layer");
    require(doc.validate().empty(), "volumetric duplicate fixture is valid");
    requireHeatSourceRejected(tree, "die_layer", 1.0);
}

static void heatSourceMissingLayersAcceptance() {
    CaseDocument fixture;
    loadHeatSourceFixture(fixture);
    CaseTreeModel fixtureTree(&fixture);
    fixtureTree.setThermalMode(true);
    auto thermal = fixture.toJson();
    thermal.erase("stack");
    thermal.erase("materials");
    // Keep the default empty stack; do not retain dangling fixture activation_groups.
    CaseDocument doc;
    doc.applyJson(thermal);
    require(doc.toJson() == thermal && !doc.stackJson().contains("layers"),
            "missing layers fixture keeps thermal case and empty stack");
    CaseTreeModel tree(&doc);
    const auto stackBefore = doc.stackJson();
    requireHeatSourceRejected(tree, "die_layer", 1.0);
    require(doc.stackJson() == stackBefore, "missing layers rejection preserves stack");
}

static void heatSourceMissingSchemaAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    tree.setThermalMode(true);
    require(doc.validate().empty(), "missing schema fixture starts valid");
    QTemporaryDir schemas;
    require(schemas.isValid(), "heat source temporary schema directory");
    tree.setCaseSchemaPath(schemas.filePath("missing.schema.json"));
    requireHeatSourceRejected(tree, "die_layer", 1.0);
}

static void layerContextMenuAcceptance(bool thermal) {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    SchemaForm form;
    CaseTreeView view(&tree, &form, QString(CHIPCAE_REPOSITORY) + "/schema");
    if (thermal)
        tree.setThermalMode(true);
    // Resolve after switching mode: the switch rebuilds all indexes.
    const auto index = tree.locate("/layers/2", "stack");
    require(index.isValid() && index.data(CaseTreeModel::PointerRole) == "/layers/2",
            "context menu targets exact die layer node");
    std::unique_ptr<QMenu> menu(view.contextMenuFor(index));
    require(menu != nullptr, "layer context menu exists");
    require(menu->parent() == &view, "layer context menu is owned by view");
    require(menu->actions().size() == 1, "layer context menu has exactly one action");
    const auto* action = menu->actions().front();
    require(action->text() == QStringLiteral("在此层添加热源"), "exact heat source action text");
    require(action->isEnabled() == thermal, "heat source action enabled only in thermal mode");
    // Per the brief revision, Qt uses the action text when no tooltip is specified.
    require(action->toolTip() == (thermal ? QStringLiteral("在此层添加热源")
                                          : QStringLiteral("需先切换到 Thermal 模式")),
            "exact heat source action tooltip");
}

static void layerContextMenuRejectsOtherNodesAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    SchemaForm form;
    CaseTreeView view(&tree, &form, QString(CHIPCAE_REPOSITORY) + "/schema");
    tree.setThermalMode(true);
    const QModelIndex nodes[] = {tree.locate("/loads"),
                                 tree.locate("/loads/0"),
                                 tree.locate("/layers/2/regions", "stack"),
                                 tree.locate("/layers/2/regions/0", "stack"),
                                 tree.locate("/layers/2/name", "stack"),
                                 childNamed(tree, {}, "Stack layers"),
                                 childNamed(tree, {}, "Bodies"),
                                 childNamed(tree, {}, "Materials")};
    for (const auto& index : nodes) {
        require(index.isValid(), "non-layer menu test node exists");
        std::unique_ptr<QMenu> menu(view.contextMenuFor(index));
        require(menu == nullptr, "non-layer node has no heat source context menu");
    }
    std::unique_ptr<QMenu> invalid(view.contextMenuFor(QModelIndex()));
    require(invalid == nullptr, "invalid index has no context menu");
    CaseTreeModel other(&doc);
    std::unique_ptr<QMenu> foreign(view.contextMenuFor(other.locate("/layers/2", "stack")));
    require(foreign == nullptr, "foreign model layer has no context menu");
}

static void layerContextMenuPolicyAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    SchemaForm form;
    CaseTreeView view(&tree, &form, QString(CHIPCAE_REPOSITORY) + "/schema");
    require(view.contextMenuPolicy() == Qt::CustomContextMenu,
            "tree view requests custom context menus");
}

static void layerContextMenuAddsAndSelectsAcceptance() {
    CaseDocument doc;
    loadHeatSourceFixture(doc);
    CaseTreeModel tree(&doc);
    SchemaForm form;
    CaseTreeView view(&tree, &form, QString(CHIPCAE_REPOSITORY) + "/schema");
    tree.setThermalMode(true);
    const auto layer = tree.locate("/layers/2", "stack");
    require(layer.isValid() && layer.data(CaseTreeModel::PointerRole) == "/layers/2",
            "menu addition targets exact die layer");
    tree.itemFromIndex(layer)->setText("display label differs from layer name");
    std::unique_ptr<QMenu> menu(view.contextMenuFor(layer));
    require(menu != nullptr && menu->actions().size() == 1,
            "thermal layer has one heat source action");
    auto* action = menu->actions().front();
    require(action->isEnabled(), "thermal action is enabled before triggering");
    const auto count = doc.toJson().at("loads").size();
    action->trigger();
    const auto loads = doc.toJson().at("loads");
    require(loads.size() == count + 1, "menu action appends one heat source");
    const auto path = QString("/loads/%1").arg(count);
    const auto selected = tree.locate(path);
    require(selected.isValid() && selected.data(CaseTreeModel::PointerRole) == path,
            "new heat source has exact tree path");
    requireHeatSourceAdded(
        doc,
        selected.data(CaseTreeModel::IdentityRole).toString(),
        count,
        Json::parse(R"({"type":"heat_source","target":"layer:die_layer","power":1.0})"));
    require(view.currentIndex() == selected, "menu action selects new heat source");
    require(form.objectPath() == path, "menu selection binds new heat source form");
    require(form.setValue(path + "/power", 10), "stage heat source power edit");
    require(view.commitEdit(), "commit heat source power edit");
    auto edited = doc.toJson().at("loads").at(count);
    require(edited.at("power").as<double>() == 10.0, "edited heat source is exactly 10 W");
    edited.erase("id");
    QFile demo(QString(CHIPCAE_REPOSITORY) + "/examples/demo_gate/case.json");
    require(demo.open(QIODevice::ReadOnly), "DEMO-GATE case loads");
    const auto demoCase = Json::parse(demo.readAll().toStdString());
    require(edited == demoCase.at("loads").at(1), "edited heat source equals DEMO-GATE source");
    require(doc.validate().empty(), "edited heat source document is valid");
    const auto beforeDuplicate = doc.toJson();
    action->trigger();
    require(doc.toJson() == beforeDuplicate, "repeated menu trigger leaves document unchanged");
    // Duplicate prevention belongs to the model; the thermal menu remains enabled.
    std::unique_ptr<QMenu> duplicateMenu(view.contextMenuFor(tree.locate("/layers/2", "stack")));
    require(duplicateMenu != nullptr && duplicateMenu->actions().size() == 1 &&
                duplicateMenu->actions().front()->isEnabled(),
            "thermal menu remains enabled for layer with heat source");
}

static void heatSourceAcceptance() {
    struct Test {
        const char* name;
        void (*run)();
    };
    const Test tests[] = {
        {"heat source rejected outside thermal mode", heatSourceOutsideThermalAcceptance},
        {"heat source added on layer", heatSourceAddedAcceptance},
        {"heat source edge cases", heatSourceEdgeCasesAcceptance},
        {"heat source volumetric duplicate", heatSourceVolumetricDuplicateAcceptance},
        {"heat source missing layers", heatSourceMissingLayersAcceptance},
        {"heat source missing schema", heatSourceMissingSchemaAcceptance},
        {"layer context menu outside thermal mode", [] { layerContextMenuAcceptance(false); }},
        {"layer context menu in thermal mode", [] { layerContextMenuAcceptance(true); }},
        {"layer context menu rejects other nodes", layerContextMenuRejectsOtherNodesAcceptance},
        {"layer context menu policy", layerContextMenuPolicyAcceptance},
        {"layer context menu adds and selects heat source",
         layerContextMenuAddsAndSelectsAcceptance}};
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS D14i-1 " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL D14i-1 " << test.name << ": " << error.what() << '\n';
        }
    }
    require(failures == 0, "D14i-1 heat source acceptance failures");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        const QString root = CHIPCAE_REPOSITORY;
        CaseDocument doc;
        require(doc.load(root + "/schema/examples/fcbga_demo.case.json"), "fixture");
        CaseTreeModel tree(&doc);
        SchemaForm form;
        CaseTreeView view(&tree, &form, root + "/schema");
        require(tree.locate("/layers/0", "stack").isValid(), "stack layers");
        const auto snapshot = doc.submitSnapshot();
        require(!tree.remove("case:/loads/0") && doc.toJson() == snapshot,
                "cannot remove sole uniform load");
        require(tree.addLoad(Json::object()).isEmpty(), "reject malformed load");
        tree.setThermalMode(true);
        require(doc.validate().empty(), "thermal mode valid");
        auto id = tree.addLoad(
            Json::parse(R"({"type":"convection","target":"face:zmax","h":2,"T_inf":300})"));
        require(!id.isEmpty(), "add load");
        auto valid = doc.toJson();
        require(!tree.edit(id, Json::parse(R"({"type":"uniform_temperature"})")) &&
                    doc.toJson() == valid,
                "invalid edit rolls back");
        require(
            tree.edit(
                id, Json::parse(R"({"type":"convection","target":"face:zmax","h":3,"T_inf":300})")),
            "edit load");
        auto fixed = tree.addFixedFace("face:zmin");
        auto second = tree.addFixedFace("face:zmax");
        require(tree.remove(fixed) && tree.edit(second, Json::parse(R"({"target":"face:xmin"})")),
                "stable fixed id after remove");
        require(tree.remove(second) && doc.toJson().at("constraints").at("rigid") == "3-2-1",
                "default rigid");
        view.setCurrentIndex(tree.locate("/loads/1"));
        auto before = doc.toJson();
        require(form.setValue("/loads/1/h", 7), "staged edit");
        view.cancelEdit();
        require(doc.toJson() == before, "cancel unchanged");
        require(form.setValue("/loads/1/h", 8) && view.commitEdit(), "commit edit");
        require(tree.remove(id), "remove stable load");
        tree.setThermalMode(false);
        require(doc.validate().empty() &&
                    snapshot.at("loads").at(0).at("type") == "uniform_temperature",
                "uniform and frozen snapshot");
        auto library = doc.materialLibrary();
        library["materials"].push_back(library.at("materials").at(0));
        require(!doc.setMaterialLibrary(library) &&
                    doc.errors().front().pointer.startsWith("/materials/") &&
                    doc.errors().front().source == "materials",
                "duplicate material pointer");
        auto profile = Json::parse(R"({"ranks":"invalid"})");
        require(!doc.applyRunProfile(profile), "invalid profile");
        bool profilePointer = false;
        for (const auto& error : doc.errors())
            profilePointer |= error.pointer == "/ranks";
        require(profilePointer, "run profile field pointer");
        QTemporaryDir dir;
        require(doc.save(dir.filePath("case.json")), "save");
        CaseDocument reopened;
        require(reopened.load(dir.filePath("case.json")), "reopen");
        CaseTreeModel other(&reopened);
        require(other.rowCount() == tree.rowCount() && other.locate("/loads/0").isValid(),
                "tree roundtrip");
        auto invalid = doc.toJson();
        invalid["mesh"]["levels"] = "bad";
        doc.applyJson(invalid);
        bool found = false;
        for (const auto& error : doc.validate())
            if (error.pointer == "/mesh/levels") {
                found = true;
                require(view.highlight(error), "highlight error");
            }
        require(found, "field pointer");
        for (const auto& ref :
             {QString("urn:chipcae:stack:0.2"),
              QString("urn:chipcae:run-profile:1.0"),
              QString("https://chipcae.local/schema/parameters.schema.json"),
              QUrl::fromLocalFile(dir.filePath("stack-0.2.schema.json")).toString()}) {
            // Keep the probe alongside copies of the official schemas in a temporary directory.
            for (const auto* name : {"stack-0.2", "run-profile-1.0", "parameters"})
                if (!QFile::exists(dir.filePath(QString(name) + ".schema.json")))
                    require(QFile::copy(root + "/schema/" + name + ".schema.json",
                                        dir.filePath(QString(name) + ".schema.json")),
                            "schema copy");
            QFile probe(dir.filePath("probe.schema.json"));
            require(probe.open(QIODevice::WriteOnly), "probe");
            Json wrapper = Json::object();
            wrapper["$ref"] = ref.toStdString();
            probe.write(QByteArray::fromStdString(wrapper.to_string()));
            probe.close();
            // File URI probe uses the original path; URI registry is local to the copied directory.
            SchemaValidator validator(probe.fileName());
            auto resolved = validator.resolved(validator.schema());
            require(QString::fromStdString(resolved.at("$id").as<std::string>()) ==
                        (ref.startsWith("file:") ? QString("urn:chipcae:stack:0.2") : ref),
                    "URI maps to correct schema");
            const bool parameters = ref.contains("parameters");
            require(validator.validate(Json::object()).empty() == parameters,
                    "referenced requirements enforced");
            if (parameters)
                require(!validator.validate(Json::parse(R"({"linear":{"rtol":0}})")).empty(),
                        "parameters reference enforces numeric bounds");
        }
        std::cout << "existing tree tests passed\n";
        assemblyTreeAcceptance();
        std::cout << "existing assembly tree tests passed\n";
        caseSchemaPathAcceptance();
        std::cout << "tree tests passed\n";
        heatSourceAcceptance();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
