#include "CaseTreeView.hpp"

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
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
        std::cout << "tree tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
