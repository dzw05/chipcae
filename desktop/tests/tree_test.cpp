#include "CaseTreeView.hpp"

#include <QApplication>
#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>
#include <iostream>
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
    check(lid.data().toString() == "lid （近似材料）" &&
              lid.data(CaseTreeModel::BodyAnnotationRole) == "近似材料",
          "empty summary preserves label and annotation");
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
        std::cout << "tree tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
