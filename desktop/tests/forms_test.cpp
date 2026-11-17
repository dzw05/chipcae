#include "CaseOutline.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <iostream>
#include <stdexcept>

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
        std::cout
            << "Schema forms: sample, refs, enum, required, extra, nested, conditional and UI OK\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
