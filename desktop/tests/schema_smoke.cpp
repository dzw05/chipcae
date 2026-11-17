#include <iostream>
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
int main() {
    using jsoncons::json;
    const auto schema = json::parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema",
        "type":"array","prefixItems":[{"type":"integer"}],"items":false})");
    auto validator = jsoncons::jsonschema::make_json_schema(schema);
    if (!validator.is_valid(json::parse("[1]")) || validator.is_valid(json::parse("[1,2]")) ||
        validator.is_valid(json::parse("[\"bad\"]"))) {
        return 1;
    }
    const auto object = json::parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema",
        "allOf":[{"properties":{"x":{"type":"integer"}}}],"unevaluatedProperties":false})");
    auto objectValidator = jsoncons::jsonschema::make_json_schema(object);
    std::cout << "jsoncons " << jsoncons::version() << " draft 2020-12\n";
    return objectValidator.is_valid(json::parse("{\"x\":1}")) &&
                   !objectValidator.is_valid(json::parse("{\"y\":1}"))
               ? 0
               : 1;
}
