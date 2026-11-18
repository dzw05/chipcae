#include "CaseDocument.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace chipcae::desktop;
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
namespace {
void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create fixture file");
    require(file.write(bytes) == bytes.size() && file.flush(), "write fixture bytes");
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "open asset for byte comparison");
    const auto bytes = file.readAll();
    require(file.error() == QFileDevice::NoError, "read asset bytes");
    return bytes;
}

QByteArray sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

Json externalRef(const std::string& path, const QByteArray& hash) {
    Json ref = Json::object();
    ref["path"] = path;
    ref["sha256"] = hash.toStdString();
    return ref;
}

struct ImportFixture {
    QTemporaryDir root;
    CaseDocument document;
    const QString casePath = root.filePath("case.json");
    const QDir source{root.filePath("import-src")};
    const QByteArray payload{"polyset-fixture-v1"};
    const QByteArray hash = sha256(payload);
    Json stack;

    ImportFixture() {
        require(root.isValid(), "create temporary import fixture");
        require(document.load(QStringLiteral(DOCUMENT_FIXTURE_DIR "/fcbga_demo.case.json")),
                "load official import fixture");
        require(document.save(casePath), "save portable import working copy");
        require(document.load(casePath), "reload import working copy");
        require(!document.isDirty(), "import working copy starts clean");
        require(source.mkpath("."), "create import source directory");
        writeBytes(source.filePath("polyset.bin"), payload);
        stack = document.stackJson();
        stack["external_refs"] = Json::make_array({externalRef("polyset.bin", hash)});
        writeBytes(source.filePath("imported.stack.json"),
                   QByteArray::fromStdString(stack.to_string()));
    }
};

QDir stackDirectory(const CaseDocument& document, const QString& casePath) {
    const auto ref = document.toJson().at("stack").at("ref").as<std::string>();
    return QFileInfo(QFileInfo(casePath).absoluteDir().filePath(QString::fromStdString(ref)))
        .absoluteDir();
}

Json withoutExternalPaths(Json stack) {
    for (auto& ref : stack.at("external_refs").array_range())
        ref.erase("path");
    return stack;
}

void requireRelocatedAssets(const Json& original,
                            const Json& relocated,
                            const QDir& source,
                            const QDir& destination) {
    require(relocated.contains("external_refs"), "relocated stack retains external_refs");
    auto expected = withoutExternalPaths(original);
    const auto& originalRefs = original.at("external_refs");
    const auto& relocatedRefs = relocated.at("external_refs");
    require(relocatedRefs.size() == originalRefs.size(), "relocation preserves array length");
    for (size_t i = 0; i < originalRefs.size(); ++i) {
        const auto& originalRef = originalRefs.at(i);
        const auto& relocatedRef = relocatedRefs.at(i);
        const auto name = QString::fromStdString(relocatedRef.at("path").as<std::string>());
        require(!QDir::isAbsolutePath(name) && !name.contains('\\') && !name.contains(':') &&
                    !name.split('/').contains(".."),
                "relocated asset has a valid relative path");
        const auto destinationPath = destination.filePath(name);
        require(QFileInfo(destinationPath)
                    .canonicalFilePath()
                    .startsWith(destination.canonicalPath() + '/'),
                "relocated asset resolves inside the stack directory");
        const auto originalName = QString::fromStdString(originalRef.at("path").as<std::string>());
        const auto originalBytes = readBytes(source.filePath(originalName));
        const auto relocatedBytes = readBytes(destinationPath);
        const auto expectedHash =
            QByteArray::fromStdString(originalRef.at("sha256").as<std::string>()).toLower();
        require(relocatedRef.at("sha256") == sha256(relocatedBytes).toStdString(),
                "stored digest matches the relocated file's lowercase SHA-256");
        require(relocatedBytes == originalBytes, "relocated asset retains exact bytes");
        require(sha256(originalBytes) == expectedHash && sha256(relocatedBytes) == expectedHash,
                "both files match the declared SHA-256");
        expected["external_refs"][i]["sha256"] = expectedHash.toStdString();
    }
    require(withoutExternalPaths(relocated) == expected,
            "relocation changes only external_refs paths and hash casing");
}

bool sameErrors(const DocumentErrors& actual, const DocumentErrors& expected) {
    return std::ranges::equal(actual, expected, [](const auto& a, const auto& b) {
        return a.code == b.code && a.message == b.message && a.pointer == b.pointer &&
               a.source == b.source;
    });
}

DocumentErrors
requireRejectedImport(CaseDocument& document, const Json& stack, const QString& sourceDir = {}) {
    const auto previousStack = document.stackJson();
    const auto previousErrors = document.errors();
    const auto previousDirty = document.isDirty();
    int changeSignals = 0;
    QObject observer;
    QObject::connect(&document, &CaseDocument::changed, &observer, [&] { ++changeSignals; });
    QObject::connect(&document, &CaseDocument::dirtyChanged, &observer, [&] { ++changeSignals; });
    QObject::connect(&document, &CaseDocument::editStarted, &observer, [&] { ++changeSignals; });

    const auto issues = document.importedStackIssues(stack, sourceDir);
    require(!document.checkImportedStack(stack, sourceDir), "check rejects invalid import");
    require(document.stackJson() == previousStack && document.isDirty() == previousDirty,
            "read-only checks preserve stack and dirty state");
    require(sameErrors(document.errors(), previousErrors),
            "read-only checks preserve prior issues");
    require(changeSignals == 0, "read-only checks emit no document signals");

    require(!document.replaceImportedStack(stack, sourceDir), "replace rejects invalid import");
    require(document.stackJson() == previousStack && document.isDirty() == previousDirty,
            "rejected import preserves stack and dirty state");
    require(sameErrors(document.errors(), issues), "replace publishes the queried issues");
    return issues;
}

void requireIssueAt(const DocumentErrors& issues, const char* code, const char* pointer) {
    require(std::ranges::any_of(issues,
                                [&](const DocumentError& issue) {
                                    return issue.code == code && issue.pointer == pointer &&
                                           issue.source == "stack";
                                }),
            code);
}

void testMissingSource() {
    ImportFixture fixture;
    auto& document = fixture.document;
    require(!document.checkImportedStack(fixture.stack), "one-argument check requires sourceDir");
    const auto issues = document.importedStackIssues(fixture.stack);
    require(issues.size() == 1, "omitted sourceDir produces exactly one issue");
    requireIssueAt(issues, "INPUT_REF_SOURCE_MISSING", "/external_refs");
    require(issues.front().message.contains("sourceDir"), "source issue mentions sourceDir");
    require(sameErrors(requireRejectedImport(document, fixture.stack), issues),
            "omitted sourceDir produces the same issue across calls");
}

void testLoadedImportAndSaveAs() {
    ImportFixture fixture;
    auto& document = fixture.document;
    auto stack = fixture.stack;
    stack["external_refs"][0]["sha256"] = fixture.hash.toUpper().toStdString();
    require(fixture.source.mkpath("assets/copper"), "create nested source asset directory");
    writeBytes(fixture.source.filePath("assets/copper/nested.bin"), fixture.payload);
    stack["external_refs"].push_back(externalRef("assets/copper/nested.bin", fixture.hash));
    const auto source = fixture.source.absolutePath();
    const auto relativeSource = QDir::current().relativeFilePath(source);
    require(QDir::isRelativePath(relativeSource), "construct a relative sourceDir");
    require(document.checkImportedStack(stack, source),
            "check accepts external_refs when sourceDir contains the sidecar");
    require(document.importedStackIssues(stack, source).empty(), "valid import has no issues");
    require(document.checkImportedStack(stack, relativeSource),
            "relative sourceDir resolves from the current directory");
    require(document.replaceImportedStack(stack, relativeSource),
            "loaded document imports the sidecar");
    require(document.errors().empty() && document.validate().empty(),
            "imported document validates");
    require(document.isDirty(), "successful import marks the document dirty");
    const auto imported = document.stackJson();
    requireRelocatedAssets(
        stack, imported, fixture.source, stackDirectory(document, fixture.casePath));
    require(imported.at("external_refs").at(0).at("path") == "polyset.bin",
            "import preserves an available sidecar name");
    require(imported.at("external_refs").at(1).at("path") == "assets/copper/nested.bin",
            "import preserves an available nested sidecar path");

    const auto saveAs = fixture.root.filePath("save-as/case.json");
    require(document.save(saveAs), "Save As writes a new case and sidecar bundle");
    CaseDocument reloaded;
    require(reloaded.load(saveAs), "reload Save As case with sidecar");
    require(reloaded.validate().empty(), "reloaded case validates");
    const auto savedDir = stackDirectory(reloaded, saveAs);
    require(savedDir.dirName().startsWith("case-assets-") &&
                QFileInfo(savedDir.absolutePath()).absolutePath() ==
                    QFileInfo(saveAs).absolutePath(),
            "Save As creates a case-assets bundle in the new directory");
    const auto reloadedCase = reloaded.toJson();
    const auto bundlePrefix = savedDir.dirName().toStdString() + '/';
    for (const auto* ref : {"stack", "materials"})
        require(reloadedCase.at(ref).at("ref").as<std::string>().starts_with(bundlePrefix),
                "Save As case references resolve into the new asset bundle");
    const auto stackRef =
        QString::fromStdString(reloadedCase.at("stack").at("ref").as<std::string>());
    const auto savedStackPath = QFileInfo(saveAs).absoluteDir().filePath(stackRef);
    const auto savedStack = Json::parse(readBytes(savedStackPath).toStdString());
    requireRelocatedAssets(stack, savedStack, fixture.source, savedDir);
    require(reloaded.stackJson() == savedStack, "reload preserves the saved stack");
    require(withoutExternalPaths(savedStack) == withoutExternalPaths(imported),
            "Save As round-trips the imported stack apart from ref paths");
}

void testNoSidecars() {
    ImportFixture fixture;
    auto& document = fixture.document;
    auto stack = document.stackJson();
    stack.erase("external_refs");
    stack["id"] = "without_external_refs";
    require(document.checkImportedStack(stack), "one-argument check accepts absent external_refs");
    require(document.replaceImportedStack(stack) && document.stackJson() == stack,
            "replace with absent external_refs preserves the input stack");
    stack["id"] = "empty_external_refs";
    stack["external_refs"] = Json::array();
    require(document.checkImportedStack(stack), "one-argument check accepts an empty refs array");
    require(document.replaceImportedStack(stack) && document.stackJson() == stack,
            "replace with an empty refs array preserves the input stack");
    const auto source = fixture.source.absolutePath();
    require(document.checkImportedStack(stack, source),
            "check accepts an empty refs array with an explicit source directory");
    require(document.replaceImportedStack(stack, source) && document.stackJson() == stack,
            "replace accepts an empty refs array with an explicit source directory");
}

void testEmptyStack() {
    ImportFixture fixture;
    const auto issues = requireRejectedImport(fixture.document, Json::object());
    require(issues.size() == 1 && issues.front().code == "INPUT_STACK_EMPTY",
            "empty stack reports exactly INPUT_STACK_EMPTY");
}

void testMissingRefField(const char* property) {
    ImportFixture fixture;
    fixture.stack["external_refs"][0].erase(property);
    const auto issues =
        requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath());
    require(std::ranges::any_of(issues,
                                [](const DocumentError& issue) {
                                    return issue.code == "STACK_SCHEMA_INVALID" ||
                                           issue.code == "INPUT_REF_INVALID";
                                }),
            "missing ref field reports STACK_SCHEMA_INVALID or INPUT_REF_INVALID");
}

void testInvalidAssetPath(const char* name) {
    ImportFixture fixture;
    fixture.stack["external_refs"][0]["path"] = name;
    const auto issues =
        requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath());
    require(std::ranges::any_of(issues,
                                [](const DocumentError& issue) {
                                    return issue.code == "STACK_SCHEMA_INVALID" ||
                                           issue.code == "INPUT_REF_INVALID";
                                }),
            "invalid path reports STACK_SCHEMA_INVALID or INPUT_REF_INVALID");
}

void testEscapingAssetSymlink() {
    ImportFixture fixture;
    const auto outside = fixture.root.filePath("outside.bin");
    writeBytes(outside, fixture.payload);
    require(QFile::link(outside, fixture.source.filePath("escape.bin")), "create escaping symlink");
    fixture.stack["external_refs"][0]["path"] = "escape.bin";
    requireIssueAt(
        requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath()),
        "INPUT_REF_INVALID",
        "/external_refs/0/path");
}

void testReadOnlyImportChecks() {
    ImportFixture fixture;
    auto& document = fixture.document;
    auto library = document.materialLibrary();
    library["materials"].push_back(library.at("materials").at(0));
    require(!document.setMaterialLibrary(library), "seed prior material validation errors");
    require(!document.errors().empty(), "prior errors available for comparison");

    auto stack = fixture.stack;
    stack["external_refs"][0]["sha256"] = std::string(64, '0');
    const auto source = fixture.source.absolutePath();
    requireIssueAt(requireRejectedImport(document, stack, source),
                   "INPUT_HASH_MISMATCH",
                   "/external_refs/0/sha256");
    require(document.replaceImportedStack(fixture.stack, source), "valid import after a failure");
    require(document.errors().empty(), "successful import clears previous errors");
}

void testMissingCaseReference() {
    ImportFixture fixture;
    auto& document = fixture.document;
    const auto materials = document.toJson().at("materials").at("ref").as<std::string>();
    require(QFile::remove(fixture.root.filePath(QString::fromStdString(materials))),
            "remove the loaded case's materials file");

    const auto issues =
        requireRejectedImport(document, fixture.stack, fixture.source.absolutePath());
    require(issues.size() == 1 && issues.front().code == "INPUT_REF_MISSING" &&
                issues.front().source == "case",
            "preflight reports the missing case reference");
    require(!stackDirectory(document, fixture.casePath).exists("polyset.bin"),
            "a missing case reference prevents sidecar relocation");
}

void testDuplicatesAndCollision() {
    ImportFixture fixture;
    auto& document = fixture.document;
    auto& refs = fixture.stack["external_refs"];
    const QByteArray otherPayload("other-sidecar");
    writeBytes(fixture.source.filePath("other.bin"), otherPayload);
    refs.push_back(externalRef("other.bin", sha256(otherPayload)));
    refs.push_back(externalRef("polyset.bin", fixture.hash.toUpper()));

    const auto destination = stackDirectory(document, fixture.casePath);
    const QByteArray occupiedBytes("existing asset");
    writeBytes(destination.filePath("polyset.bin"), occupiedBytes);
    const auto source = fixture.source.absolutePath();
    require(document.importedStackIssues(fixture.stack, source).empty(),
            "duplicate hashes are compared case-insensitively");
    require(document.replaceImportedStack(fixture.stack, source),
            "import duplicate and distinct assets");
    const auto relocated = document.stackJson();
    requireRelocatedAssets(fixture.stack, relocated, fixture.source, destination);
    const auto& relocatedRefs = relocated.at("external_refs");
    require(relocatedRefs.at(0).at("path") == "case-1-polyset.bin", "rename the colliding asset");
    require(relocatedRefs.at(0).at("path") == relocatedRefs.at(2).at("path"),
            "duplicate refs share the single relocated path");
    require(relocatedRefs.at(1).at("path") == "other.bin", "keep the free distinct asset name");
    require(readBytes(destination.filePath("polyset.bin")) == occupiedBytes,
            "import leaves the occupied asset intact");
    require(document.validate().empty(), "case validates after collision relocation");

    const auto files = destination.entryList(QDir::Files | QDir::Hidden);
    require(document.replaceImportedStack(fixture.stack, source), "repeat a successful import");
    require(document.stackJson() == relocated, "repeat import keeps the selected names");
    require(destination.entryList(QDir::Files | QDir::Hidden) == files,
            "repeat import creates no additional assets");
}

void testConflictingRefs() {
    ImportFixture fixture;
    auto& refs = fixture.stack["external_refs"];
    refs.push_back(externalRef("polyset.bin", QByteArray(64, '0')));
    requireIssueAt(
        requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath()),
        "INPUT_REF_CONFLICT",
        "/external_refs/1/sha256");
}

void testCopyRollback() {
    ImportFixture fixture;
    auto& document = fixture.document;
    const auto source = fixture.source.absolutePath();
    const auto destination = stackDirectory(document, fixture.casePath);
    // The Linux fixture filesystem accepts 255-byte names. A collision prefix is too long.
    const QString longName(255, 'z');
    const QByteArray occupiedBytes("existing long-name asset");
    writeBytes(fixture.source.filePath(longName), fixture.payload);
    writeBytes(destination.filePath(longName), occupiedBytes);
    require(fixture.source.mkpath("new"), "create nested source asset directory");
    writeBytes(fixture.source.filePath("new/fresh.bin"), fixture.payload);
    auto& refs = fixture.stack["external_refs"];
    refs[0]["path"] = "new/fresh.bin";
    refs.push_back(externalRef(longName.toStdString(), fixture.hash));

    const auto entries =
        destination.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    const auto original = document.stackJson();
    const auto dirty = document.isDirty();
    require(document.checkImportedStack(fixture.stack, source),
            "copy failure fixture passes preflight");
    require(!document.replaceImportedStack(fixture.stack, source),
            "replace reports a copy failure");
    require(std::ranges::any_of(
                document.errors(),
                [](const DocumentError& issue) { return issue.code == "INPUT_REF_COPY_FAILED"; }),
            "copy failure reports INPUT_REF_COPY_FAILED");
    require(document.stackJson() == original && document.isDirty() == dirty,
            "failed copy preserves the current stack and dirty state");
    require(destination.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot) ==
                entries,
            "failed import removes its copied asset and created directory");
    require(readBytes(destination.filePath(longName)) == occupiedBytes,
            "copy failure leaves the occupied asset intact");
}

void testEscapingDestinationAncestor() {
    ImportFixture fixture;
    auto& document = fixture.document;
    const auto destination = stackDirectory(document, fixture.casePath);
    const auto outside = fixture.root.filePath("outside");
    require(QDir().mkdir(outside), "create directory outside the destination");
    require(fixture.source.mkdir("nested"), "create nested source asset directory");
    writeBytes(fixture.source.filePath("nested/copy.bin"), fixture.payload);
    const auto link = destination.filePath("nested");
    require(QFile::link(outside, link), "create an escaping destination symlink");
    fixture.stack["external_refs"].push_back(externalRef("nested/copy.bin", fixture.hash));

    const auto original = document.stackJson();
    const auto source = fixture.source.absolutePath();
    require(document.checkImportedStack(fixture.stack, source),
            "escaping destination fixture passes source preflight");
    require(!document.replaceImportedStack(fixture.stack, source),
            "replace rejects an escaping destination ancestor");
    requireIssueAt(document.errors(), "INPUT_REF_COPY_FAILED", "/external_refs/1/path");
    require(document.stackJson() == original && !document.isDirty(),
            "escaping destination preserves the current stack and dirty state");
    require(QFileInfo(link).isSymLink() && !QDir(outside).exists("copy.bin") &&
                !destination.exists("polyset.bin"),
            "rollback preserves the symlink and removes the earlier copy");
}

void testUnknownMaterial() {
    ImportFixture fixture;
    fixture.stack["layers"][0]["material"] = "unknown_import_material";
    requireIssueAt(
        requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath()),
        "MATERIAL_REFERENCE_INVALID",
        "/layers/0/material");
}

void testRemovedTargetLayer() {
    ImportFixture fixture;
    auto& document = fixture.document;
    auto currentCase = document.toJson();
    currentCase["constraints"] =
        Json::parse(R"({"fixed":[{"target":"layer:substrate","id":"fix"}]})");
    document.applyJson(currentCase);
    require(document.validate().empty(), "case has a valid existing layer target");

    auto& layers = fixture.stack.at("layers");
    layers.erase(layers.array_range().begin());
    fixture.stack.erase("activation_groups");
    const auto issues =
        requireRejectedImport(document, fixture.stack, fixture.source.absolutePath());
    require(std::ranges::any_of(
                issues,
                [](const DocumentError& issue) { return issue.code == "TARGET_LAYER_INVALID"; }),
            "import preserves current case layer target validation");
}

bool runImportAcceptance() {
    bool passed = true;
    const auto run = [&](const QString& name, auto test) {
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << "D17 " << name.toStdString() << ": " << error.what() << '\n';
            passed = false;
        }
    };

    run("omitted sourceDir", testMissingSource);
    for (const auto* name : {"nonexistent", "import-src/polyset.bin"}) {
        run("invalid sourceDir " + QString::fromLatin1(name), [=] {
            ImportFixture fixture;
            requireIssueAt(
                requireRejectedImport(fixture.document, fixture.stack, fixture.root.filePath(name)),
                "INPUT_REF_SOURCE_MISSING",
                "/external_refs");
        });
    }
    run("loaded import and Save As", testLoadedImportAndSaveAs);
    run("absent and empty refs", testNoSidecars);
    run("empty stack", testEmptyStack);
    for (const auto* field : {"path", "sha256"}) {
        run("missing ref field " + QString::fromLatin1(field), [=] { testMissingRefField(field); });
    }
    run("asset hash mismatch", [] {
        ImportFixture fixture;
        fixture.stack["external_refs"][0]["sha256"] = std::string(64, '0');
        requireIssueAt(
            requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath()),
            "INPUT_HASH_MISMATCH",
            "/external_refs/0/sha256");
    });
    for (const auto* name : {"missing.bin", "Polyset.bin"}) {
        run("missing asset " + QString::fromLatin1(name), [=] {
            ImportFixture fixture;
            fixture.stack["external_refs"][0]["path"] = name;
            requireIssueAt(requireRejectedImport(
                               fixture.document, fixture.stack, fixture.source.absolutePath()),
                           "INPUT_REF_MISSING",
                           "/external_refs/0/path");
        });
    }
    for (const auto* name : {"/tmp/x.bin", R"(..\escape.bin)", "../outside.bin"}) {
        run("invalid asset path " + QString::fromLatin1(name), [=] { testInvalidAssetPath(name); });
    }
    run("escaping asset symlink", testEscapingAssetSymlink);
    run("read-only queries and failure parity", testReadOnlyImportChecks);
    run("missing existing case reference", testMissingCaseReference);
    run("duplicates, collision, and retry", testDuplicatesAndCollision);
    run("conflicting refs", testConflictingRefs);
    run("copy rollback", testCopyRollback);
    run("escaping destination ancestor", testEscapingDestinationAncestor);
    run("unknown material", testUnknownMaterial);
    run("removed target layer", testRemovedTargetLayer);
    return passed;
}
} // namespace

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

        std::cout << "Existing CaseDocument assertions passed" << std::endl;
        if (!runImportAcceptance())
            return 1;
        std::cout << "D17 acceptance tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
