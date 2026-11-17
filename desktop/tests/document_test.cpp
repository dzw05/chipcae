#include "CaseDocument.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        CaseDocument d;
        require(d.load(QStringLiteral(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json")),
                "load official fixture");
        require(!d.isDirty(), "clean after load");
        auto original = d.toJson();
        auto changed = original;
        changed["x-user"] = Json::parse(R"({"中文":[1,"铜"]})");
        changed["mesh"].erase("levels");
        changed["mesh"].erase("max_ratio");
        d.applyJson(changed);
        require(d.isDirty(), "dirty after field change");
        auto options = d.meshOptions();
        require(options.maxRatio == 2 && options.cellBudget == 50000 && options.levels == 0,
                "mesh defaults");
        require(!d.finalizeMeshForSubmit(), "omitted levels rejected");
        options.levels = 0;
        d.setMeshOptions(options);
        require(!d.finalizeMeshForSubmit(), "zero levels rejected");
        options.levels = 1;
        d.setMeshOptions(options);
        require(d.finalizeMeshForSubmit(), "positive levels accepted");
        QTemporaryDir dir;
        auto path = dir.filePath("case.json");
        require(d.save(path), "atomic Save As with reference relocation");
        require(!d.isDirty(), "clean after save");
        QFile f(path);
        require(f.open(QIODevice::ReadOnly), "read saved bytes");
        require(d.contentHash() ==
                    QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex(),
                "saved hash");
        CaseDocument roundTrip;
        require(roundTrip.load(path), "reload saved snapshot");
        require(roundTrip.toJson() == d.toJson(), "semantic round trip");
        require(roundTrip.stackJson() == d.stackJson(), "stack preservation");
        require(roundTrip.materialLibrary() == d.materialLibrary(), "material preservation");
        require(roundTrip.toJson().at("x-user") == changed.at("x-user"),
                "user extension preservation");
        auto library = d.materialLibrary();
        library["materials"].push_back(library.at("materials").at(0));
        require(!d.setMaterialLibrary(library), "duplicate rejected");
        require(d.errors().front().code == "MATERIAL_ID_DUPLICATE", "official duplicate code");
        auto invalid = d.toJson();
        invalid["loads"] = Json::array();
        d.applyJson(invalid);
        require(!d.validate().empty() && !d.save(path),
                "uniform temperature required on validate/save");
        require(roundTrip.load(path), "failed save preserved original");
        auto badHash = roundTrip.toJson();
        badHash["stack"]["sha256"] = std::string(64, '0');
        QFile hashFile(dir.filePath("bad.json"));
        require(hashFile.open(QIODevice::WriteOnly), "bad fixture");
        hashFile.write(QByteArray::fromStdString(badHash.to_string()));
        hashFile.close();
        require(!d.load(hashFile.fileName()) && d.errors().front().code == "INPUT_HASH_MISMATCH",
                "reference hash mismatch");
        auto traversal = roundTrip.toJson();
        traversal["stack"]["ref"] = "../outside.json";
        QFile traversalFile(dir.filePath("traversal.json"));
        require(traversalFile.open(QIODevice::WriteOnly), "traversal fixture");
        traversalFile.write(QByteArray::fromStdString(traversal.to_string()));
        traversalFile.close();
        require(!d.load(traversalFile.fileName()), "reject reference path traversal");
        require(roundTrip.renameLayer("substrate", "renamed"), "rename layer");
        auto targeting = roundTrip.toJson();
        targeting["constraints"] =
            Json::parse(R"({"fixed":[{"target":"layer:renamed","id":"fix"}]})");
        roundTrip.applyJson(targeting);
        require(roundTrip.renameLayer("renamed", "renamed_again"), "rename targeted layer");
        require(roundTrip.toJson().at("constraints").at("fixed").at(0).at("target") ==
                    "layer:renamed_again",
                "rename syncs target");
        require(roundTrip.save(path), "save renamed stack");
        require(d.load(path), "reload renamed stack");
        require(d.loadRunProfile(QStringLiteral(DOCUMENT_FIXTURE_DIR "/local.run-profile.json")),
                "load separate profile");
        require(d.saveRunProfile(dir.filePath("profile.json")), "save separate profile");
        require(!d.toJson().contains("ranks"), "profile stays outside case");
        const auto digestJson = Json::parse(
            R"({"nx0":1,"ny0":1,"L":1,"padded_cells":0,"nz":1,"npoints":8,"ncells":1,"mesh_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})");
        auto meshDigest = MeshDigest::fromJson(digestJson);
        require(meshDigest == MeshDigest::fromJson(digestJson), "digest equality");
        meshDigest.L = 2;
        require(!(meshDigest == MeshDigest::fromJson(digestJson)), "digest difference");

        QTemporaryDir extra;
        CaseDocument regionDoc;
        require(regionDoc.load(QStringLiteral(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json")),
                "load fixture for extra cases");
        require(regionDoc.save(extra.filePath("base.json")), "stage portable case");
        auto regionStack = regionDoc.stackJson();
        bool cleared = false;
        for (auto& layer : regionStack.at("layers").array_range()) {
            if (layer.contains("regions")) {
                layer.erase("material");
                cleared = true;
            }
        }
        require(cleared, "demo stack has a region layer");
        QFile regionFile(extra.filePath("region.stack.json"));
        require(regionFile.open(QIODevice::WriteOnly), "write region stack");
        const auto regionBytes = QByteArray::fromStdString(regionStack.to_string());
        regionFile.write(regionBytes);
        regionFile.close();
        auto regionCase = regionDoc.toJson();
        regionCase["stack"]["ref"] = "region.stack.json";
        regionCase["stack"]["sha256"] =
            QCryptographicHash::hash(regionBytes, QCryptographicHash::Sha256).toHex().toStdString();
        regionDoc.applyJson(regionCase);
        require(regionDoc.validate().empty(), "region-only layer validates");
        require(regionDoc.save(extra.filePath("region.json")), "save region-only layer case");
        CaseDocument regionReload;
        require(regionReload.load(extra.filePath("region.json")), "reload region-only layer case");

        auto altStack = regionReload.stackJson();
        altStack["layers"][0]["name"] = "alt_substrate";
        QFile altFile(extra.filePath("alt.stack.json"));
        require(altFile.open(QIODevice::WriteOnly), "write alt stack");
        const auto altBytes = QByteArray::fromStdString(altStack.to_string());
        altFile.write(altBytes);
        altFile.close();
        auto altCase = regionReload.toJson();
        altCase["stack"]["ref"] = "alt.stack.json";
        altCase["stack"]["sha256"] =
            QCryptographicHash::hash(altBytes, QCryptographicHash::Sha256).toHex().toStdString();
        regionReload.applyJson(altCase);
        require(regionReload.stackJson()["layers"][0]["name"] == "alt_substrate",
                "applyJson refreshes cached stack");
        require(regionReload.save(extra.filePath("alt.json")), "save after ref change");
        CaseDocument altReload;
        require(altReload.load(extra.filePath("alt.json")), "reload after ref change");
        require(altReload.stackJson()["layers"][0]["name"] == "alt_substrate",
                "saved snapshot follows the new stack ref");

        QDir src(extra.filePath("collision-src"));
        require(src.mkpath("."), "collision source dir");
        QFile dummy(src.filePath("stack.json"));
        require(dummy.open(QIODevice::WriteOnly), "write dummy external stack.json");
        dummy.write("payload-not-a-stack");
        dummy.close();
        const auto dummyHash =
            QCryptographicHash::hash(QByteArray("payload-not-a-stack"), QCryptographicHash::Sha256)
                .toHex()
                .toStdString();
        auto collisionStack = altReload.stackJson();
        collisionStack["external_refs"] = Json::array();
        Json ext = Json::object();
        ext["path"] = "stack.json";
        ext["sha256"] = dummyHash;
        collisionStack["external_refs"].push_back(ext);
        QFile collisionFile(src.filePath("model.stack.json"));
        require(collisionFile.open(QIODevice::WriteOnly), "write colliding stack");
        const auto collisionBytes = QByteArray::fromStdString(collisionStack.to_string());
        collisionFile.write(collisionBytes);
        collisionFile.close();
        auto collisionCase = altReload.toJson();
        collisionCase["stack"]["ref"] = "collision-src/model.stack.json";
        collisionCase["stack"]["sha256"] =
            QCryptographicHash::hash(collisionBytes, QCryptographicHash::Sha256)
                .toHex()
                .toStdString();
        altReload.applyJson(collisionCase);
        require(altReload.save(extra.filePath("collision.json")),
                "save with stack.json asset collision");
        CaseDocument collisionReload;
        require(collisionReload.load(extra.filePath("collision.json")),
                "reload after stack.json asset collision");

        std::cout << "CaseDocument tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
