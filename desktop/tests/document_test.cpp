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
namespace chipcae::desktop {
struct CaseDocumentTestAccess {
    static void seedUnsavedCase(CaseDocument& document, const Json& data) { document.data_ = data; }
};
} // namespace chipcae::desktop
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
    require(issues.size() == 1, "missing required field produces one schema issue");
    requireIssueAt(issues, "STACK_SCHEMA_INVALID", "/external_refs/0");
}

void testInvalidAssetPath(const char* name, const char* code) {
    ImportFixture fixture;
    fixture.stack["external_refs"][0]["path"] = name;
    const auto issues =
        requireRejectedImport(fixture.document, fixture.stack, fixture.source.absolutePath());
    require(issues.size() == 1, "invalid path produces one issue");
    requireIssueAt(issues, code, "/external_refs/0/path");
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
    const auto duplicate = library.at("materials").at(0);
    library["materials"].push_back(duplicate);
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

void testMissingCaseReference(const char* caseReference, const QString& externalRefs) {
    ImportFixture fixture;
    auto& document = fixture.document;
    const auto reference = document.toJson().at(caseReference).at("ref").as<std::string>();
    require(QFile::remove(fixture.root.filePath(QString::fromStdString(reference))),
            "remove the loaded case's reference file");
    if (externalRefs == "absent")
        fixture.stack.erase("external_refs");
    else if (externalRefs == "empty")
        fixture.stack["external_refs"] = Json::array();

    if (externalRefs != "nonempty") {
        require(document.importedStackIssues(fixture.stack).empty() &&
                    document.checkImportedStack(fixture.stack),
                "one-argument preflight validates the candidate without reading case files");
        require(document.replaceImportedStack(fixture.stack) &&
                    document.stackJson() == fixture.stack && document.errors().empty(),
                "one-argument replace retains the no-sidecar success path");
    }
    const auto previousStack = document.stackJson();
    const auto previousDirty = document.isDirty();
    const auto previousErrors = document.errors();
    const auto source = fixture.source.absolutePath();
    const auto destination = stackDirectory(document, fixture.casePath);
    const auto entries = destination.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    require(document.importedStackIssues(fixture.stack, source).empty() &&
                document.checkImportedStack(fixture.stack, source),
            "explicit-source preflight validates cached case data without reading old files");
    require(document.stackJson() == previousStack && document.isDirty() == previousDirty &&
                sameErrors(document.errors(), previousErrors) &&
                destination.entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == entries,
            "successful preflight preserves document state and destination files");
    require(document.replaceImportedStack(fixture.stack, source) &&
                document.stackJson() == fixture.stack && document.errors().empty(),
            "explicit-source replace accepts a valid import with a missing old case file");
    if (externalRefs == "nonempty")
        requireRelocatedAssets(fixture.stack, document.stackJson(), fixture.source, destination);
    require(!QFileInfo::exists(fixture.root.filePath(QString::fromStdString(reference))),
            "import does not recreate unrelated case reference files");
    require(!document.save(fixture.root.filePath("save-as.json")) &&
                document.errors().size() == 1 && document.errors().front().code == "DOCUMENT_ERROR",
            "save still validates missing case references with the legacy error code");
}

void testLegacyCaseReferenceErrors(const char* caseReference, const QString& change) {
    ImportFixture fixture;
    auto& document = fixture.document;
    const auto previousCase = document.toJson();
    const auto previousStack = document.stackJson();
    const auto previousMaterials = document.materialLibrary();
    const auto previousHash = document.contentHash();
    auto invalid = previousCase;
    if (change == "hash")
        invalid[caseReference]["sha256"] = std::string(64, '0');
    else
        invalid[caseReference]["ref"] = change == "missing" ? "missing.json" : "../outside.json";
    const auto code = change == "hash" ? "INPUT_HASH_MISMATCH" : "DOCUMENT_ERROR";

    document.applyJson(invalid);
    require(document.errors().size() == 1 && document.errors().front().code == code,
            "applyJson preserves legacy reference error codes");
    const auto path = fixture.root.filePath("invalid.case.json");
    writeBytes(path, QByteArray::fromStdString(invalid.to_string()));
    require(!document.load(path) && document.errors().size() == 1 &&
                document.errors().front().code == code,
            "load preserves legacy reference error codes");
    require(document.toJson() == previousCase && document.stackJson() == previousStack &&
                document.materialLibrary() == previousMaterials &&
                document.contentHash() == previousHash && !document.isDirty(),
            "failed legacy reference changes preserve the document snapshot");
}

void testSaveChangedSidecar(const QString& change) {
    ImportFixture fixture;
    auto& document = fixture.document;
    require(document.replaceImportedStack(fixture.stack, fixture.source.absolutePath()),
            "import sidecar before saving a loaded case");
    require(document.save(fixture.casePath) && document.load(fixture.casePath),
            "save and reload the sidecar case before changing its asset");
    const auto destination = stackDirectory(document, fixture.casePath);
    const auto asset = destination.filePath("polyset.bin");
    require(QFile::remove(asset), "remove the loaded sidecar");
    if (change == "symlink") {
        const auto outside = fixture.root.filePath("outside.bin");
        writeBytes(outside, fixture.payload);
        require(QFile::link(outside, asset), "replace loaded sidecar with an escaping symlink");
    } else if (change == "hash") {
        writeBytes(asset, "changed sidecar bytes");
    }
    const auto previousCase = document.toJson();
    const auto previousStack = document.stackJson();
    const auto previousHash = document.contentHash();
    const auto previousBytes = readBytes(fixture.casePath);
    const QDir root(fixture.root.path());
    const auto previousEntries = root.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    const auto issues = document.validate();
    const auto code = change == "hash" ? "INPUT_HASH_MISMATCH" : "DOCUMENT_ERROR";
    require(issues.size() == 1 && issues.front().code == code,
            "validation identifies the changed loaded sidecar");
    for (const auto& path : {fixture.casePath, fixture.root.filePath("save-as.json")}) {
        require(!document.save(path), "save rejects a changed loaded sidecar");
        require(sameErrors(document.errors(), issues), "save preserves source validation errors");
        require(document.toJson() == previousCase && document.stackJson() == previousStack &&
                    document.contentHash() == previousHash && !document.isDirty(),
                "failed save preserves the document snapshot");
        require(readBytes(fixture.casePath) == previousBytes &&
                    root.entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == previousEntries,
                "failed save preserves case bytes and creates no bundle or Save As file");
    }
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

    for (const auto& path : {fixture.casePath, fixture.root.filePath("save-as.json")}) {
        require(document.save(path), "save accepts matching duplicate references");
        CaseDocument reloaded;
        require(reloaded.load(path) && reloaded.validate().empty(),
                "reload saved duplicate references");
        const auto savedDir = stackDirectory(reloaded, path);
        requireRelocatedAssets(fixture.stack, reloaded.stackJson(), fixture.source, savedDir);
        const auto savedStack = reloaded.stackJson();
        const auto& savedRefs = savedStack.at("external_refs");
        require(savedRefs.at(0).at("path") == savedRefs.at(2).at("path"),
                "saved duplicate references share one asset");
        require(savedDir.entryList(QDir::Files).size() == 4,
                "bundle contains two unique sidecars, stack and materials");
    }
}

void seedUnsavedDocument(CaseDocument& document, const ImportFixture& fixture) {
    // Public applyJson deliberately refuses to change references before load. Seed only the
    // case data here so the import/save API can be exercised with a genuinely empty path_.
    CaseDocumentTestAccess::seedUnsavedCase(document, fixture.document.toJson());
    require(document.setMaterialLibrary(fixture.document.materialLibrary()),
            "seed unsaved material library");
    require(document.setStackJson(fixture.document.stackJson()), "seed unsaved stack");
    require(document.validate().empty(), "unsaved document has valid case data");
}

void testUnsavedImportAndSave() {
    ImportFixture fixture;
    CaseDocument document;
    document.applyJson(fixture.document.toJson());
    require(document.toJson().empty() && !document.errors().empty(),
            "public applyJson still rejects reference changes before load");
    seedUnsavedDocument(document, fixture);
    writeBytes(fixture.source.filePath("first.polyset"), fixture.payload);
    writeBytes(fixture.source.filePath("second.polyset"), "second-polyset-fixture");
    auto stack = fixture.stack;
    stack["external_refs"][0]["path"] = "first.polyset";
    stack["external_refs"].push_back(
        externalRef("second.polyset", sha256("second-polyset-fixture")));
    stack["external_refs"].push_back(externalRef("first.polyset", fixture.hash.toUpper()));
    const auto source = fixture.source.absolutePath();
    require(document.replaceImportedStack(stack, source), "import multiple polysets before save");
    document.applyJson(document.toJson());
    document.applyStackJson(document.stackJson());
    require(document.setStackJson(document.stackJson()), "unchanged stack preserves import root");
    require(!document.setStackJson(Json::object()), "invalid stack replacement is rejected");
    requireRejectedImport(document, stack, fixture.root.filePath("nonexistent"));
    require(!document.save(fixture.source.filePath("polyset.bin/blocked.json")),
            "failed save preserves the pending source root");
    auto verificationSource = fixture.source;
    for (const auto* name : {"first/case.json", "second/case.json"}) {
        const auto path = fixture.root.filePath(name);
        require(document.save(path), "save imported assets from an unsaved document");
        CaseDocument reloaded;
        require(reloaded.load(path) && reloaded.validate().empty(),
                "reload multiple polysets from the saved bundle");
        requireRelocatedAssets(
            stack, reloaded.stackJson(), verificationSource, stackDirectory(reloaded, path));
        if (QString::fromLatin1(name).startsWith("first")) {
            verificationSource = stackDirectory(reloaded, path);
            require(QDir(source).removeRecursively(),
                    "remove the original source after the first successful save");
        }
    }
}

void testImportThenReplacement(const QString& replacement) {
    ImportFixture fixture;
    CaseDocument document;
    seedUnsavedDocument(document, fixture);
    require(document.replaceImportedStack(fixture.stack, fixture.source.absolutePath()),
            "import before replacing unsaved document contents");
    if (replacement == "applyJson") {
        auto data = document.toJson();
        data["x-replacement"] = true;
        document.applyJson(data);
        require(document.toJson() == data, "replace case data");
    } else if (replacement == "renameLayer") {
        require(document.renameLayer("substrate", "replacement"), "replace a layer name");
    } else {
        auto stack = document.stackJson();
        stack["id"] = "replacement";
        if (replacement == "setStackJson")
            require(document.setStackJson(stack), "replace stack through setStackJson");
        else
            document.applyStackJson(stack);
        require(document.stackJson() == stack, "replace stack contents");
    }
    const auto path = fixture.root.filePath("replacement.json");
    require(!document.save(path), "replacement cannot reuse the old import's source directory");
    require(document.errors().size() == 1 &&
                document.errors().front().message.contains("Missing original stack location"),
            "replacement clears the pending import location");
    require(!QFileInfo::exists(path), "failed replacement save creates no case file");
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
    for (const auto* name : {"/tmp/x.bin", R"(..\escape.bin)", "../outside.bin", "C:/x.bin"}) {
        run("invalid asset path " + QString::fromLatin1(name),
            [=] { testInvalidAssetPath(name, "STACK_SCHEMA_INVALID"); });
    }
    run("colon in asset path",
        [] { testInvalidAssetPath("asset:polyset.bin", "INPUT_REF_INVALID"); });
    run("escaping asset symlink", testEscapingAssetSymlink);
    run("read-only queries and failure parity", testReadOnlyImportChecks);
    for (const auto* caseReference : {"stack", "materials"})
        for (const auto* externalRefs : {"absent", "empty", "nonempty"})
            run("missing existing " + QString::fromLatin1(caseReference) + " with " + externalRefs +
                    " external refs",
                [=] { testMissingCaseReference(caseReference, externalRefs); });
    for (const auto* change : {"symlink", "hash", "missing"})
        run("save changed sidecar " + QString::fromLatin1(change),
            [=] { testSaveChangedSidecar(change); });
    for (const auto* caseReference : {"stack", "materials"})
        for (const auto* change : {"invalid", "hash", "missing"})
            run("legacy " + QString::fromLatin1(caseReference) + " reference " + change,
                [=] { testLegacyCaseReferenceErrors(caseReference, change); });
    run("duplicates, collision, and retry", testDuplicatesAndCollision);
    run("unsaved import and Save As with multiple polysets", testUnsavedImportAndSave);
    for (const auto* replacement : {"applyJson", "setStackJson", "applyStackJson", "renameLayer"})
        run("import then " + QString::fromLatin1(replacement),
            [=] { testImportThenReplacement(replacement); });
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
