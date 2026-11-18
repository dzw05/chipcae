#include "StackEditor.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const QString& path, const Json& value) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "open fixture");
    const auto bytes = QByteArray::fromStdString(value.to_string());
    require(file.write(bytes) == bytes.size(), "write fixture");
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        CaseDocument doc;
        require(doc.load(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json"), "demo fallback load");
        QTemporaryDir dir;
        require(doc.save(dir.filePath("base.json")), "stage fixture");
        QFile cu(DOCUMENT_FIXTURE_DIR "/fcbga_cu.stack.json");
        require(cu.open(QIODevice::ReadOnly), "copper fallback read");
        auto stack = Json::parse(cu.readAll().toStdString());
        auto library = doc.materialLibrary();
        // Copper is supplied by the shared library; add only the missing stack fixture IDs.
        for (const char* id : {"substrate_resin", "SAC305_ball_eq"}) {
            auto material = library.at("materials").at(0);
            material["id"] = id;
            library["materials"].push_back(material);
        }
        require(doc.setMaterialLibrary(library), "material library");
        require(doc.setStackJson(stack), "handwritten copper stack");
        StackEditor editor(&doc);
        auto* model = editor.model();
        auto edit = [&](int column, const QVariant& value) {
            return model->setData(model->index(0, column), value);
        };
        require(model->rowCount() == 3, "layer list");
        require(!edit(LayerModel::Thickness, 0) && !edit(LayerModel::Thickness, -1),
                "positive thickness");
        require(edit(LayerModel::Thickness, 0.0002), "metres thickness");
        require(edit(LayerModel::Fraction, 0) && edit(LayerModel::Fraction, 1), "phi endpoints");
        require(!edit(LayerModel::Fraction, -0.1) && !edit(LayerModel::Fraction, 1.1),
                "phi bounds");
        require(!edit(LayerModel::Material, "unknown"), "unknown material rejected");
        require(edit(LayerModel::Material, "underfill"), "library material mapping");
        require(doc.stackJson()["layers"][0]["material"] == "underfill", "material is id string");
        require(editor.previewStale(), "preview invalidated");
        editor.revertEdit();
        require(doc.stackJson() == stack, "revert entire edit group");
        auto caseValue = doc.toJson();
        caseValue["constraints"] =
            Json::parse(R"({"fixed":[{"target":"layer:top_copper","id":"fix"}]})");
        doc.applyJson(caseValue);
        editor.beginEdit();
        require(edit(LayerModel::Name, "renamed"), "rename");
        require(doc.toJson()["constraints"]["fixed"][0]["target"] == "layer:renamed",
                "target follows rename");
        editor.revertEdit();
        require(doc.toJson() == caseValue && doc.stackJson() == stack,
                "rename revert restores targets");
        // Keep polygon copper separate from constant copper, as required by stack0.2 oneOf.
        stack["layers"][1]["copper"] = Json::parse(
            R"({"metal":"copper","polygons":{"file":"shapes/polyset.json","layer":"L2"}})");
        QDir().mkpath(dir.filePath("nested/shapes"));
        QFile payload(dir.filePath("nested/shapes/polyset.json"));
        require(payload.open(QIODevice::WriteOnly), "polyset fixture");
        payload.write("polygon-payload");
        payload.close();
        const auto hash =
            QCryptographicHash::hash("polygon-payload", QCryptographicHash::Sha256).toHex();
        stack["external_refs"] = Json::parse(R"([{"path":"shapes/polyset.json","sha256":""}])");
        stack["external_refs"][0]["sha256"] = hash.toStdString();
        write(dir.filePath("nested/model.json"), stack);
        caseValue = doc.toJson();
        caseValue["stack"]["ref"] = "nested/model.json";
        caseValue["stack"].erase("sha256");
        doc.applyJson(caseValue);
        // Changing references reloads the on-disk material library. Restore the fixture IDs.
        require(doc.setMaterialLibrary(library), "restore copper fixture materials");
        require(doc.validate().empty(), "stack-relative polyset validates");
        editor.beginEdit();
        require(!(model->flags(model->index(1, LayerModel::Fraction)) & Qt::ItemIsEditable),
                "polygon phi read only");
        require(!model->setData(model->index(1, LayerModel::Fraction), 0.5),
                "polygon edit rejected");
        require(model->data(model->index(1, LayerModel::Geometry))
                    .toString()
                    .contains(QString::fromLatin1(hash)),
                "geometry source hash display");
        require(edit(LayerModel::Fraction, 0.5) && edit(LayerModel::Thickness, 0.0003),
                "constant edits with polyset");
        require(doc.stackJson()["layers"][1] == stack["layers"][1] &&
                    doc.stackJson()["external_refs"] == stack["external_refs"],
                "polygon and refs fidelity");
        require(editor.actionsAllowed(), "valid preview eligibility");
        QTemporaryDir destination;
        require(editor.save(destination.filePath("saved.json")), "save as");
        CaseDocument reopened;
        require(reopened.load(destination.filePath("saved.json")), "reopen portable snapshot");
        require(reopened.stackJson() == doc.stackJson(), "layers and polyset survive save");
        require(reopened.save(destination.filePath("snapshot.json")), "second snapshot");
        require(reopened.load(destination.filePath("snapshot.json")), "snapshot reopen");
        auto bad = stack;
        bad["external_refs"][0]["sha256"] = std::string(64, '0');
        require(doc.setStackJson(bad), "stage bad hash");
        require(!editor.actionsAllowed(), "bad hash blocks actions");
        bool mismatch = false;
        for (const auto& error : editor.validationErrors())
            mismatch |= error.code == "INPUT_HASH_MISMATCH";
        require(mismatch && !editor.save(destination.filePath("bad.json")),
                "official hash error blocks save");
        std::cout << "Stack editor tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
