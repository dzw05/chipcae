#include "StackEditor.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
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
int resultsDisplayAcceptance() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "PASS stack result display: " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL stack result display: " << name << ": " << e.what() << '\n';
        }
    };
    const auto result = Json::parse(R"({"layers":[
        {"name":" Duplicate ","thickness":0.0012345678901234567,"material":"Mixed_CASE"},
        {"name":" Duplicate ","thickness":0.00023456789012345678,"material":""}]})");
    run("set and clear result source without document side effects", [&] {
        CaseDocument doc;
        require(doc.load(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json"),
                "load case for display test");
        StackEditor editor(&doc);
        const auto before = doc.toJson();
        const auto stack = doc.stackJson();
        const auto dirty = doc.isDirty();
        const auto stale = editor.previewStale();
        int invalidated = 0, resets = 0;
        QObject::connect(
            &editor, &StackEditor::previewInvalidated, &editor, [&] { ++invalidated; });
        QObject::connect(
            editor.model(), &QAbstractItemModel::modelReset, &editor, [&] { ++resets; });
        for (const auto& clear : {Json::parse("{}"), Json::parse("{\"id\":\"no_layers\"}")}) {
            editor.setResultsStack(result);
            require(editor.model()->rowCount() == 2, "result source replaces document rows");
            require(editor.isReadOnly(), "result override forces read-only");
            for (int row = 0; row < 2; ++row) {
                require(editor.model()->index(row, LayerModel::Name).data().toString() ==
                            " Duplicate ",
                        "duplicate result names remain in order without trimming");
                require(editor.model()
                                ->index(row, LayerModel::Material)
                                .data()
                                .toString()
                                .toStdString() ==
                            result.at("layers").at(row).at("material").as<std::string>(),
                        "result material including empty value preserved");
                require(editor.model()->index(row, LayerModel::Thickness).data().toDouble() ==
                            result.at("layers").at(row).at("thickness").as<double>(),
                        "result thickness double preserved exactly");
            }
            editor.setResultsStack(clear);
            require(!editor.isReadOnly() &&
                        editor.model()->rowCount() == static_cast<int>(stack.at("layers").size()),
                    "clearing override restores editable case source");
            require(editor.model()->index(0, LayerModel::Name).data().toString().toStdString() ==
                        stack.at("layers").at(0).at("name").as<std::string>(),
                    "clearing override restores case name");
        }
        require(resets >= 4, "source changes reset the existing model");
        require(doc.toJson() == before && doc.stackJson() == stack && doc.isDirty() == dirty &&
                    editor.previewStale() == stale && invalidated == 0,
                "source changes leave document dirty stale and signals untouched");
        require(editor.model()->setData(editor.model()->index(0, LayerModel::Name), "case_edit"),
                "normal case editing works after clearing results");
    });
    run("read-only blocks flags direct edits save and revert", [&] {
        CaseDocument doc;
        require(doc.load(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json"), "load case for guards");
        StackEditor editor(&doc);
        require(editor.model()->setData(editor.model()->index(0, LayerModel::Name), "pending_edit"),
                "create pending case edit before read-only");
        const auto before = doc.toJson();
        const auto stack = doc.stackJson();
        const auto dirty = doc.isDirty();
        const auto stale = editor.previewStale();
        int invalidated = 0;
        QObject::connect(
            &editor, &StackEditor::previewInvalidated, &editor, [&] { ++invalidated; });
        editor.setReadOnly(true);
        require(editor.isReadOnly(), "explicit read-only state");
        for (int row = 0; row < editor.model()->rowCount(); ++row)
            for (int col = 0; col < editor.model()->columnCount(); ++col) {
                const auto index = editor.model()->index(row, col);
                require(!(editor.model()->flags(index) & Qt::ItemIsEditable),
                        "read-only flags on every cell");
                require(!editor.model()->setData(
                            index,
                            col == LayerModel::Thickness ? QVariant(0.003) : QVariant("blocked")),
                        "direct model editing blocked");
            }
        require(!editor.actionsAllowed(), "read-only disables case actions");
        bool foundRevert = false;
        for (auto* button : editor.findChildren<QPushButton*>())
            if (button->text().contains("Revert")) {
                foundRevert = true;
                require(!button->isEnabled(), "Revert button disabled");
                button->click();
            }
        require(foundRevert, "existing Revert button found");
        editor.revertEdit();
        QTemporaryDir dir;
        require(dir.isValid(), "guard temporary directory");
        const auto path = dir.filePath("blocked.case.json");
        require(!editor.save(path) && !QFileInfo::exists(path), "read-only save creates no output");
        require(doc.toJson() == before && doc.stackJson() == stack && doc.isDirty() == dirty &&
                    editor.previewStale() == stale && invalidated == 0,
                "rejected edit revert save preserve pending case and preview");
        editor.setReadOnly(false);
        require(!editor.isReadOnly() &&
                    editor.model()->setData(editor.model()->index(0, LayerModel::Name), "allowed"),
                "explicit unlock restores case editing");
    });
    run("result override cannot be unlocked or written", [&] {
        CaseDocument doc;
        StackEditor editor(&doc);
        const auto before = doc.toJson();
        const auto dirty = doc.isDirty();
        const auto stale = editor.previewStale();
        int invalidated = 0;
        QObject::connect(
            &editor, &StackEditor::previewInvalidated, &editor, [&] { ++invalidated; });
        editor.setResultsStack(result);
        editor.setReadOnly(false);
        require(editor.isReadOnly(), "result source remains read-only after unlock request");
        require(!editor.model()->setData(editor.model()->index(0, LayerModel::Name), "blocked"),
                "result direct edit rejected");
        editor.revertEdit();
        QTemporaryDir dir;
        require(dir.isValid(), "result save temporary directory");
        const auto path = dir.filePath("blocked.json");
        require(!editor.save(path) && !QFileInfo::exists(path) && !editor.actionsAllowed(),
                "result actions blocked");
        require(editor.model()->rowCount() == 2 && doc.toJson() == before &&
                    doc.isDirty() == dirty && editor.previewStale() == stale && invalidated == 0,
                "result actions leave display document and preview unchanged");
    });
    run("loading document clears the result override", [&] {
        CaseDocument doc;
        StackEditor editor(&doc);
        editor.setResultsStack(result);
        require(doc.load(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json"),
                "load document after results");
        require(!editor.isReadOnly() && editor.model()->rowCount() == 3 &&
                    editor.model()->index(0, LayerModel::Name).data().toString() == "substrate",
                "document stack replaces result override");
        require(editor.model()->setData(editor.model()->index(0, LayerModel::Name), "restored"),
                "case edit after document load");
    });
    return failures;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const int acceptanceFailures = resultsDisplayAcceptance();
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
        std::cout << "Existing stack editor tests passed\n";
        return acceptanceFailures ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
