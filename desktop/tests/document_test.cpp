#include "CaseDocument.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
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
        std::cout << "CaseDocument tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
