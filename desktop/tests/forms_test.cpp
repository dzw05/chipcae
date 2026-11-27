#include "CaseOutline.hpp"
#include "DesktopConfig.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStringList>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace chipcae::desktop;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
bool hasPath(const QList<FieldError>& errors, const QString& path) {
    for (const auto& error : errors) {
        if (error.path.startsWith(path)) {
            return true;
        }
    }
    return false;
}

void schemaPathTests(const QString& root) {
    QTemporaryDir temporary;
    require(temporary.isValid(), "valid schema path fixture directory");
    const QProcessEnvironment emptyEnv;
    const auto makeDir = [&](const QString& relative) {
        const auto path = temporary.filePath(relative);
        require(QDir().mkpath(path), "create schema path fixture directory");
        return path;
    };
    const auto writeSchema = [](const QString& path) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "open schema path fixture");
        require(file.write("{}") == 2, "write complete schema path fixture");
    };
    const auto makeSchema = [&](const QString& relative) {
        const auto path = makeDir(relative);
        writeSchema(path + "/case-1.0.schema.json");
        return path;
    };
    const auto repositorySchema = makeSchema("repository/schema");
    const auto repository = temporary.filePath("repository");
    const auto cli = makeSchema("cli/schema");
    const auto envSchema = makeSchema("env/schema");
    const auto bin = makeDir("prefix/bin");
    const auto installed = makeSchema("prefix/share/chipcae/schema");
    const auto fallback = makeSchema("fallback/schema");
    const auto unmarked = makeDir("unmarked/schema");
    const auto missing = temporary.filePath("missing/schema");
    const QStringList cliArgs{"desktop", "--schema-dir", cli + "/./"};
    QProcessEnvironment schemaEnv;
    schemaEnv.insert("CHIPCAE_SCHEMA_DIR", envSchema);

    int failures = 0;
    int scenarios = 0;
    const auto run = [&](const std::string& scenario, auto operation) {
        ++scenarios;
        try {
            operation();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << scenario << ": " << error.what() << '\n';
        }
    };
    run("single-argument repository regression", [&] {
        const auto legacy = root + "/schema/case.schema.json";
        require(SchemaForm::caseSchemaPath(root) ==
                    (QFileInfo::exists(legacy) ? legacy : root + "/schema/case-1.0.schema.json"),
                "single-argument case repository path");
        require(SchemaForm::resultSchemaPath(root) == root + "/schema/result.schema.json",
                "single-argument result repository path");
        require(SchemaForm::caseSchemaPath(repository) ==
                    repositorySchema + "/case-1.0.schema.json",
                "single-argument case uses versioned schema when legacy is absent");
    });
    writeSchema(repositorySchema + "/case.schema.json");
    run("single-argument legacy and raw path regression", [&] {
        const auto rawRepository = repository + "/./";
        require(SchemaForm::caseSchemaPath(repository) == repositorySchema + "/case.schema.json",
                "single-argument case prefers legacy schema");
        require(SchemaForm::caseSchemaPath(rawRepository) ==
                    rawRepository + "/schema/case.schema.json",
                "single-argument case preserves raw concatenation");
        require(!QFileInfo::exists(repositorySchema + "/result.schema.json"),
                "repository result fixture must be absent");
        require(SchemaForm::resultSchemaPath(rawRepository) ==
                    rawRepository + "/schema/result.schema.json",
                "single-argument result preserves raw path even when absent");
    });

    struct PathMethod {
        const char* name;
        const char* fileName;
        QString (*legacy)(const QString&);
        QString (*locate)(const QString&,
                          const QStringList&,
                          const QProcessEnvironment&,
                          const QString&,
                          const QString&);
    };
    const PathMethod methods[]{
        {"case", "case-1.0.schema.json", &SchemaForm::caseSchemaPath, &SchemaForm::caseSchemaPath},
        {"result",
         "result.schema.json",
         &SchemaForm::resultSchemaPath,
         &SchemaForm::resultSchemaPath}};
    const auto runtimeError = [](auto operation) {
        try {
            operation();
        } catch (const std::runtime_error& error) {
            return std::string(error.what());
        }
        throw std::runtime_error("expected std::runtime_error for invalid schema override");
    };
    for (const auto& method : methods) {
        const auto test = [&](const char* scenario, auto operation) {
            run(std::string(method.name) + " " + scenario, operation);
        };
        const auto expectLocated = [&](const QStringList& args,
                                       const QProcessEnvironment& env,
                                       const QString& executableDir,
                                       const QString& compileFallback,
                                       const QString& expectedDir) {
            const auto expected = DesktopConfig::locateSchemaFile(
                method.fileName, args, env, executableDir, compileFallback);
            require(!expected.isEmpty(), "fixture must produce a locator hit");
            require(expected == QFileInfo(expectedDir).canonicalFilePath() + "/" + method.fileName,
                    "locator must choose the intended fixture directory and basename");
            require(method.locate(root, args, env, executableDir, compileFallback) == expected,
                    "runtime schema path must equal DesktopConfig::locateSchemaFile");
            require(expected != method.legacy(root), "located schema must differ from repository");
        };
        test("CLI hit with normalized path and marker only", [&] {
            require(!QFileInfo::exists(cli + "/result.schema.json"),
                    "CLI result fixture must be absent");
            expectLocated(cliArgs, emptyEnv, {}, {}, cli);
        });
        test("environment hit", [&] { expectLocated({}, schemaEnv, {}, {}, envSchema); });
        test("CLI precedence over environment, install and fallback",
             [&] { expectLocated(cliArgs, schemaEnv, bin, fallback, cli); });
        test("executable directory forwarding",
             [&] { expectLocated({}, emptyEnv, bin, {}, installed); });
        test("compile-time fallback forwarding",
             [&] { expectLocated({}, emptyEnv, {}, fallback, fallback); });
        test("implicit miss preserves raw repository fallback", [&] {
            require(DesktopConfig::locateSchemaFile(method.fileName, {}, emptyEnv).isEmpty(),
                    "empty inputs must produce a locator miss");
            for (const auto& source : {root, repository + "/./", QString()}) {
                require(method.locate(source, {}, emptyEnv, {}, {}) == method.legacy(source),
                        "implicit miss must equal single-argument path including empty repository");
            }
        });
        const auto expectRejected = [&](const QStringList& args, const QProcessEnvironment& env) {
            const auto expected =
                runtimeError([&] { DesktopConfig::locateSchemaFile(method.fileName, args, env); });
            const auto actual = runtimeError([&] { method.locate(repository, args, env, {}, {}); });
            require(actual == expected,
                    "invalid override must preserve runtime_error what() exactly");
        };
        test("missing CLI override rejects valid repository",
             [&] { expectRejected({"desktop", "--schema-dir", missing}, emptyEnv); });
        test("unmarked CLI override rejects valid repository",
             [&] { expectRejected({"desktop", "--schema-dir", unmarked}, emptyEnv); });
        for (const auto& value : {QString(), missing, unmarked}) {
            run(std::string(method.name) + " invalid environment override [" + value.toStdString() +
                    "]",
                [&] {
                    QProcessEnvironment invalid;
                    invalid.insert("CHIPCAE_SCHEMA_DIR", value);
                    expectRejected({}, invalid);
                });
        }
        test("repeated calls are stable", [&] {
            const auto expected =
                DesktopConfig::locateSchemaFile(method.fileName, cliArgs, emptyEnv);
            require(!expected.isEmpty(), "repeat fixture must produce a locator hit");
            for (int i = 0; i < 2; ++i) {
                require(method.locate(root, cliArgs, emptyEnv, {}, {}) == expected,
                        "repeated calls must return the located path");
            }
        });
        test("valid A invalid valid B A does not cache result or error", [&] {
            const auto first = method.locate(root, cliArgs, emptyEnv, {}, {});
            bool rejected = false;
            try {
                method.locate(repository, {"desktop", "--schema-dir", missing}, emptyEnv, {}, {});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            const auto second = method.locate(root, {}, schemaEnv, {}, {});
            const auto again = method.locate(root, cliArgs, emptyEnv, {}, {});
            require(first == DesktopConfig::locateSchemaFile(method.fileName, cliArgs, emptyEnv),
                    "initial A lookup must match locator");
            require(rejected, "invalid lookup between valid calls must throw");
            require(second == DesktopConfig::locateSchemaFile(method.fileName, {}, schemaEnv),
                    "valid B lookup must recover after error");
            require(again == first && again != second, "returning to A must not reuse B");
        });
#ifdef Q_OS_LINUX
        test("lowercase environment name is ignored", [&] {
            QProcessEnvironment lowercase;
            lowercase.insert("chipcae_schema_dir", cli);
            require(DesktopConfig::locateSchemaFile(method.fileName, {}, lowercase).isEmpty(),
                    "lowercase variable must not produce a locator hit");
            require(method.locate(root, {}, lowercase, {}, {}) == method.legacy(root),
                    "lowercase variable must fall back to single-argument path");
        });
        test("wrong-case marker rejects valid repository", [&] {
            const auto wrongCase = makeDir(QString(method.name) + "/wrong-case");
            writeSchema(wrongCase + "/Case-1.0.schema.json");
            expectRejected({"desktop", "--schema-dir", wrongCase}, emptyEnv);
        });
#endif
    }
    run("default executable directory and fallback arguments", [&] {
        require(DesktopConfig::locateSchemaFile("case-1.0.schema.json", {}, emptyEnv).isEmpty(),
                "default-argument fixture must produce a locator miss");
        require(SchemaForm::caseSchemaPath(root, {}, emptyEnv) == SchemaForm::caseSchemaPath(root),
                "case default arguments must preserve repository fallback");
        require(SchemaForm::resultSchemaPath(root, {}, emptyEnv) ==
                    SchemaForm::resultSchemaPath(root),
                "result default arguments must preserve repository fallback");
    });
    std::cout << "Schema path scenarios: " << scenarios - failures << " passed, " << failures
              << " failed\n";
    require(failures == 0, "schema path acceptance failures");
}

// These scenarios catch unavailable choices leaking into menus and filtered-index writes.
static void hiddenEnumAcceptance() {
    QTemporaryDir schemas;
    require(schemas.isValid(), "hidden enum temporary schema directory");
    const auto schemaPath = schemas.filePath("enum.schema.json");
    const auto schema = Json::parse(R"({"type":"object","properties":{
        "s":{"enum":["fem.solid","fvm.solid","fem_vem.solid"]},
        "pc":{"enum":["column_gmg","direct","jacobi","gamg","pmg_colagg","boomeramg"]}
    }})");
    auto load = [&](SchemaForm& form, const Json& source, const Json& document) {
        QFile file(schemaPath);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open enum schema");
        const auto bytes = QByteArray::fromStdString(source.to_string());
        require(file.write(bytes) == bytes.size(), "write complete enum schema");
        file.close();
        require(form.load(schemaPath, document), "load enum schema and document");
    };
    auto editor = [](SchemaForm& form, const char* path) {
        auto* combo = form.findChild<QComboBox*>(path);
        require(combo != nullptr, "enum editor exists at exact JSON pointer");
        return combo;
    };
    auto choices = [](QComboBox* combo, const QStringList& expected, const QString& selected) {
        require(combo->count() == expected.size(), "enum count matches retained values exactly");
        for (int i = 0; i < combo->count(); ++i)
            require(combo->itemText(i) == expected.at(i),
                    "enum text and schema order match exactly");
        require(combo->currentText() == selected, "enum current text matches exactly");
    };
    int failures = 0;
    auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "PASS D14n-1 " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL D14n-1 " << name << ": " << error.what() << '\n';
        }
    };
    const auto ordinary = Json::parse(R"({"s":"fem.solid","pc":"column_gmg"})");
    run("ordinary solver hides fem_vem", [&] {
        SchemaForm form;
        load(form, schema, ordinary);
        choices(editor(form, "/s"), {"fem.solid", "fvm.solid"}, "fem.solid");
    });
    run("ordinary preconditioner hides unavailable values", [&] {
        SchemaForm form;
        load(form, schema, ordinary);
        choices(editor(form, "/pc"), {"column_gmg", "direct", "jacobi", "gamg"}, "column_gmg");
    });
    run("repeated displayed activations write their values", [&] {
        SchemaForm form;
        load(form, schema, ordinary);
        auto* solver = editor(form, "/s");
        auto* pc = editor(form, "/pc");
        for (int i = 0; i < 2; ++i) {
            solver->setCurrentIndex(1);
            solver->activated(1);
            require(form.document().at("s") == "fvm.solid", "displayed solver index 1 writes fvm");
            pc->setCurrentIndex(3);
            pc->activated(3);
            require(form.document().at("pc") == "gamg",
                    "displayed preconditioner index 3 writes gamg");
        }
    });
    run("current fem_vem is retained and selected", [&] {
        SchemaForm form;
        load(form, schema, Json::parse(R"({"s":"fem_vem.solid","pc":"pmg_colagg"})"));
        choices(editor(form, "/s"), {"fem.solid", "fvm.solid", "fem_vem.solid"}, "fem_vem.solid");
    });
    run("current pmg_colagg retains only itself among hidden preconditioners", [&] {
        SchemaForm form;
        load(form, schema, Json::parse(R"({"s":"fem_vem.solid","pc":"pmg_colagg"})"));
        choices(editor(form, "/pc"),
                {"column_gmg", "direct", "jacobi", "gamg", "pmg_colagg"},
                "pmg_colagg");
    });
    run("current boomeramg retains only itself among hidden preconditioners", [&] {
        SchemaForm form;
        load(form, schema, Json::parse(R"({"s":"fem.solid","pc":"boomeramg"})"));
        choices(editor(form, "/pc"),
                {"column_gmg", "direct", "jacobi", "gamg", "boomeramg"},
                "boomeramg");
    });
    run("interleaved hidden solver does not shift activation mapping", [&] {
        SchemaForm form;
        load(form,
             Json::parse(R"({"type":"object","properties":{"s":{
            "enum":["fem.solid","fem_vem.solid","fvm.solid"]}}})"),
             Json::parse(R"({"s":"fem.solid"})"));
        auto* combo = editor(form, "/s");
        for (int i = 0; i < 2; ++i) {
            combo->setCurrentIndex(1);
            combo->activated(1);
            require(form.document().at("s") == "fvm.solid",
                    "interleaved displayed index 1 writes fvm.solid");
        }
        choices(combo, {"fem.solid", "fvm.solid"}, "fvm.solid");
    });
    run("case variants remain visible in schema order", [&] {
        SchemaForm form;
        load(form,
             Json::parse(R"({"type":"object","properties":{"s":{
            "enum":["fem.solid","FEM_VEM.SOLID","Fem_vem.solid"]}}})"),
             Json::parse(R"({"s":"fem.solid"})"));
        choices(editor(form, "/s"), {"fem.solid", "FEM_VEM.SOLID", "Fem_vem.solid"}, "fem.solid");
    });
    run("all hidden keeps inert Invalid item", [&] {
        SchemaForm form;
        const auto source = Json::parse(R"({"type":"object","properties":{"s":{
            "enum":["fem_vem.solid"]}}})");
        const auto document = Json::parse(R"({"s":"fem.solid"})");
        load(form, source, document);
        auto* combo = editor(form, "/s");
        choices(combo, {"Invalid: fem.solid"}, "Invalid: fem.solid");
        for (int i = 0; i < 2; ++i) {
            combo->activated(0);
            require(form.document() == document, "activating synthetic Invalid item cannot write");
        }
    });
    run("empty enum keeps inert Invalid item", [&] {
        SchemaForm form;
        const auto source = Json::parse(R"({"type":"object","properties":{"s":{"enum":[]}}})");
        const auto document = Json::parse(R"({"s":"fem.solid"})");
        load(form, source, document);
        auto* combo = editor(form, "/s");
        // The renderer adds the invalid current value even when the schema enum is empty.
        choices(combo, {"Invalid: fem.solid"}, "Invalid: fem.solid");
        for (int i = 0; i < 2; ++i) {
            combo->activated(0);
            require(form.document() == document,
                    "activating empty enum's Invalid item cannot write");
        }
    });
    require(failures == 0, "D14n-1 hidden enum acceptance failures");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        const QString root = CHIPCAE_REPOSITORY;
        const auto schemaFile = SchemaForm::caseSchemaPath(root);
        const auto sample = SchemaValidator::read(root + "/schema/examples/fcbga_demo.case.json");
        SchemaValidator validator(schemaFile);
        require(validator.validate(sample).isEmpty(), "delivered case sample must validate");
        auto bad = sample;
        bad.erase("stack");
        require(!validator.validate(bad).isEmpty(), "missing required field");
        bad = sample;
        bad["unknown"] = true;
        require(!validator.validate(bad).isEmpty(), "additional property");
        bad = sample;
        bad["mesh"]["max_ratio"] = 1;
        require(hasPath(validator.validate(bad), "/mesh/max_ratio"), "nested range path");
        bad = sample;
        bad["output"]["fields"][0] = "bad";
        require(hasPath(validator.validate(bad), "/output/fields/0"), "invalid enum path");
        bad = sample;
        bad["loads"][0]["T_ref"] = -1;
        require(hasPath(validator.validate(bad), "/loads/0/T_ref"), "nested oneOf details");
        bad = sample;
        bad["loads"] = Json::array();
        require(!validator.validate(bad).isEmpty(), "if/then no thermal requires uniform load");
        const auto thermal = SchemaValidator::read(root + "/schema/examples/thermal.case.json");
        require(validator.validate(thermal).isEmpty(), "thermal case");
        for (const auto& filename : {"negative-case-mixed_load.json",
                                     "negative-case-duplicate_uniform.json",
                                     "negative-case-thermal_missing_temp_from.json",
                                     "negative-case-bad_target.json"}) {
            require(
                !validator.validate(SchemaValidator::read(root + "/schema/examples/" + filename))
                     .isEmpty(),
                "delivered negative fixture");
        }
        bad = sample;
        bad["solver"] = Json::parse(R"({"linear":{"preconditioner":"bad"}})");
        require(hasPath(validator.validate(bad), "/solver/linear/preconditioner"), "external ref");
        SchemaForm form;
        bad = sample;
        bad["x-hidden"] = Json::parse(R"({"payload":[1,2,3]})");
        require(form.load(schemaFile, bad), "load form");
        require(form.errors().isEmpty(), "extension accepted");
        require(!form.findChild<QLineEdit*>("/x-hidden"), "extension hidden");
        auto* number = form.findChild<QLineEdit*>("/mesh/max_ratio");
        require(number, "numeric editor");
        number->setText("1");
        number->textEdited("1");
        require(hasPath(form.errors(), "/mesh/max_ratio"), "edit revalidates");
        require(form.document().at("x-hidden") == bad.at("x-hidden"), "extension preserved");
        number->textEdited("-");
        require(hasPath(form.errors(), "/mesh/max_ratio"), "incomplete number path error");
        require(form.findChild<QLineEdit*>("/schema")->isReadOnly(), "const read only");
        auto* combo = form.findChild<QComboBox*>("/output/fields/0");
        require(combo, "enum combo");
        combo->setCurrentIndex(0);
        combo->activated(0);
        require(form.document().at("output").at("fields").at(0) == "T", "enum edit");
        auto* present = form.findChild<QCheckBox*>("/mesh/nx/@present");
        require(present, "optional presence");
        present->setChecked(false);
        QApplication::processEvents();
        require(!form.document().at("mesh").contains("nx"), "optional omission");
        const auto fixture = root + "/desktop/tests/data/forms.schema.json";
        SchemaValidator relativeValidator(fixture);
        auto extension = Json::parse(
            R"({"x-hidden":{"payload":1},"x-visible":"yes","nested":{"n":2},"byId":{"n":2}})");
        require(form.load(fixture, extension), "relative schema load");
        require(form.errors().isEmpty(), "relative schema validation");
        require(form.findChild<QLineEdit*>("/x-visible"), "visible extension");
        require(!form.findChild<QLineEdit*>("/x-hidden/payload"), "hidden nested extension");
        require(form.findChild<QLineEdit*>("/nested/n"), "relative schema rendering");
        require(form.findChild<QLineEdit*>("/byId/n"), "schema id rendering");
        form.setValue("/nested/n", 0);
        require(hasPath(form.errors(), "/nested/n"), "relative schema nested error");
        auto convection = thermal;
        convection["loads"][0] =
            Json::parse(R"({"type":"convection","target":"face:zmin","h":2,"T_inf":298.15})");
        require(form.load(schemaFile, convection), "thermal form");
        for (std::size_t i = 0; i < convection.at("loads").size(); ++i) {
            const auto path = QString("/loads/%1").arg(i);
            form.setObjectPath(path);
            const auto type = convection.at("loads").at(i).at("type").as<std::string>();
            if (type == "convection") {
                require(form.findChild<QLineEdit*>(path + "/h"), "convection branch selected");
            }
        }
        form.setObjectPath("/loads/0");
        auto* branch = form.findChild<QComboBox*>("/loads/0/@variant");
        require(branch && branch->currentText() == "convection", "oneOf discriminator");
        branch->setCurrentIndex(3);
        branch->activated(3);
        QApplication::processEvents();
        require(form.document().at("loads").at(0).at("type") == "heat_source", "branch switch");
        require(form.findChild<QCheckBox*>("/loads/0/power/@present"), "heat source power editor");
        require(form.load(schemaFile, sample), "reload array test");
        form.setObjectPath("/loads");
        auto* add = form.findChild<QPushButton*>("/loads/@add");
        require(add, "array add button");
        add->click();
        QApplication::processEvents();
        require(form.document().at("loads").size() == 2, "array add");
        require(!form.errors().isEmpty(), "array add validates duplicate uniform load");
        auto* remove = form.findChild<QPushButton*>("/loads/1/@remove");
        require(remove, "array remove button");
        remove->click();
        QApplication::processEvents();
        require(form.errors().isEmpty(), "array remove revalidates");
        CaseOutline outline(&form);
        require(outline.setCaseDocument(schemaFile, sample), "outline case");
        const auto stackFile = root + "/schema/stack-0.2.schema.json";
        auto stack = SchemaValidator::read(root + "/schema/examples/fcbga_demo.stack.json");
        SchemaValidator stackValidator(stackFile);
        require(stackValidator.validate(stack).isEmpty(), "stack fixture");
        stack["layers"][0]["kind"] = "bad";
        require(hasPath(stackValidator.validate(stack), "/layers/0/kind"), "layer enum path");
        outline.setStackDocument(stackFile, stack);
        require(outline.model()->rowCount() >= 2, "outline roots");
        const auto caseRoot = outline.model()->index(0, 0);
        for (int i = 0; i < outline.model()->rowCount(caseRoot); ++i) {
            const auto node = outline.model()->index(i, 0, caseRoot);
            if (node.data(Qt::UserRole).toString() == "/loads") {
                outline.setCurrentIndex(node);
                require(form.objectPath() == "/loads", "outline selection binds form");
            }
        }
        require(form.load(stackFile, stack), "stack form");
        form.setObjectPath("/layers/0");
        require(form.findChild<QComboBox*>("/layers/0/kind"), "layer editor");
        form.setObjectPath("/outline");
        require(form.findChild<QLineEdit*>("/outline/box/0"), "local ref editor");
        const auto librarySchema = root + "/material/schema/material-library.v1.schema.json";
        const auto library =
            SchemaValidator::read(root + "/schema/examples/fcbga_demo.materials.json");
        require(!form.load(librarySchema, library),
                "dangling material frame ref must fail explicitly");
        require(!form.errors().isEmpty() && form.errors().front().message.contains("frame"),
                "dangling material ref diagnostic");
        require(!form.load(root + "/desktop/tests/data/not-delivered.schema.json", Json::object()),
                "missing schema explicit");
        require(form.load(schemaFile, sample), "reload sample for stale-error check");
        auto* nx = form.findChild<QLineEdit*>("/mesh/nx");
        require(nx != nullptr, "mesh nx editor");
        nx->setText("-");
        emit nx->textEdited("-");
        require(!form.errors().isEmpty(), "invalid numeric input reported");
        form.setObjectPath("/loads");
        form.setObjectPath("");
        bool stale = false;
        for (const auto& error : form.errors())
            stale = stale || error.message.contains("Invalid JSON value");
        require(!stale, "rebuilt editor must drop discarded input errors");
        std::cout
            << "Schema forms: sample, refs, enum, required, extra, nested, conditional and UI OK\n";
        schemaPathTests(root);
        hiddenEnumAcceptance();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
