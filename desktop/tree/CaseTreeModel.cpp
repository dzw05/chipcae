#include "CaseTreeModel.hpp"

#include "SchemaValidator.hpp"

#include <QJsonObject>
#include <QMap>
#include <QUuid>
#include <jsoncons_ext/jsonpointer/jsonpointer.hpp>

namespace chipcae::desktop {
CaseTreeModel::CaseTreeModel(CaseDocument* document, QObject* parent)
    : QStandardItemModel(parent), document_(document),
      caseSchemaPath_(QStringLiteral(CHIPCAE_REPOSITORY "/schema/case-1.0.schema.json")) {
    connect(document_, &CaseDocument::changed, this, &CaseTreeModel::refresh);
    refresh();
}
// TEST-FIRST interface stubs: apply() still uses its original static validator.
void CaseTreeModel::setCaseSchemaPath(const QString& path) {
    caseSchemaPath_ =
        path.isEmpty() ? QStringLiteral(CHIPCAE_REPOSITORY "/schema/case-1.0.schema.json") : path;
}
QString CaseTreeModel::caseSchemaPath() const {
    return caseSchemaPath_;
}
void CaseTreeModel::append(QStandardItem* parent,
                           const Json& value,
                           const QString& pointer,
                           const QString& source,
                           const QString& label) {
    auto* item = new QStandardItem(label);
    item->setEditable(false);
    item->setData(pointer, PointerRole);
    item->setData(source, SourceRole);
    item->setData(source + ":" +
                      (value.is_object() && value.contains("id")
                           ? QString::fromStdString(value.at("id").as<std::string>())
                           : pointer),
                  IdentityRole);
    parent->appendRow(item);
    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            const auto& child = value.at(i);
            QString name = QString::number(i);
            for (const auto* key : {"type", "target", "name", "id"})
                if (child.is_object() && child.contains(key) && child.at(key).is_string())
                    name = QString::fromStdString(child.at(key).as<std::string>());
            append(item, child, pointer + "/" + QString::number(i), source, name);
        }
    } else if (value.is_object()) {
        for (const auto& member : value.object_range()) {
            auto key = QString::fromStdString(member.key());
            append(item,
                   member.value(),
                   pointer + "/" + QString(key).replace("~", "~0").replace("/", "~1"),
                   source,
                   key);
        }
    }
}
void CaseTreeModel::refresh() {
    clear();
    const auto data = document_->toJson();
    const auto stack = document_->stackJson();
    auto displayNode = [](QStandardItem* parent, const QString& label, const QString& source) {
        auto* item = new QStandardItem(label);
        item->setEditable(false);
        item->setData(QString(), PointerRole);
        item->setData(source, SourceRole);
        item->setData(source + ":group:" + label, IdentityRole);
        parent->appendRow(item);
        return item;
    };
    auto* layers = displayNode(invisibleRootItem(), "Stack layers", "stack");
    auto* bodies = displayNode(invisibleRootItem(), "Bodies", "bodies-index");
    QMap<QString, QStandardItem*> indexGroups;
    if (stack.is_object() && stack.contains("layers") && stack.at("layers").is_array()) {
        const auto& values = stack.at("layers");
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& layer = values.at(i);
            const auto path = "/layers/" + QString::number(i);
            const auto name = layer.contains("name") && layer.at("name").is_string()
                                  ? QString::fromStdString(layer.at("name").as<std::string>())
                                  : QString::number(i);
            auto* node = displayNode(layers, name, "stack");
            node->setData(path, PointerRole);
            node->setData("stack:" + path, IdentityRole);
            for (const auto& member : layer.object_range()) {
                const auto key = QString::fromStdString(member.key());
                if (key != "regions")
                    append(node, member.value(), path + "/" + key, "stack", key);
            }
            if (!layer.contains("regions") || !layer.at("regions").is_array())
                continue;
            auto* regions = displayNode(node, "Regions", "stack");
            regions->setData(path + "/regions", PointerRole);
            QMap<QString, QStandardItem*> groups;
            const auto& entries = layer.at("regions");
            for (size_t j = 0; j < entries.size(); ++j) {
                const auto& region = entries.at(j);
                const auto body = region.contains("body") && region.at("body").is_string()
                                      ? QString::fromStdString(region.at("body").as<std::string>())
                                      : QString();
                if (!groups.contains(body))
                    groups[body] = displayNode(
                        regions, body.isEmpty() ? QString("（无 body）") : "body:" + body, "stack");
                const auto pointer = path + "/regions/" + QString::number(j);
                const auto label = region.contains("name") && region.at("name").is_string()
                                       ? QString::fromStdString(region.at("name").as<std::string>())
                                       : QString::number(j);
                append(groups[body], region, pointer, "stack", label);
                if (!body.isEmpty()) {
                    if (!indexGroups.contains(body)) {
                        indexGroups[body] = displayNode(bodies, body, "bodies-index");
                        indexGroups[body]->setData("bodies-index:body:" + body, IdentityRole);
                    }
                    // Index rows deliberately have no JSON property children.
                    auto* row = displayNode(
                        indexGroups[body], label + " （layer=" + name + "）", "bodies-index");
                    row->setData(pointer, PointerRole);
                    row->setData("bodies-index:" + pointer, IdentityRole);
                }
            }
        }
    }
    append(invisibleRootItem(), document_->materialLibrary(), "", "materials", "Materials");
    for (const auto* key : {"loads", "constraints", "solver", "mesh", "physics", "output"})
        append(invisibleRootItem(),
               data.contains(key) ? data.at(key) : Json::object(),
               "/" + QString(key),
               "case",
               key == std::string("output") ? "Results / output" : key);
}
void CaseTreeModel::applyPartSummary(const QJsonArray& parts) {
    QMap<QString, bool> approximate;
    for (const auto& part : parts) {
        const auto object = part.toObject();
        if (object.value("id").isString()) {
            const auto id = object.value("id").toString();
            approximate[id] =
                approximate.value(id) || object.value("material_approx").toBool(false);
        }
    }
    auto* bodies = itemFromIndex(locate("", "bodies-index"));
    if (!bodies)
        return;
    for (int i = 0; i < bodies->rowCount(); ++i) {
        auto* group = bodies->child(i);
        const auto body =
            group->data(IdentityRole).toString().mid(QString("bodies-index:body:").size());
        const auto annotation = approximate.value(body) ? QString("近似材料") : QString();
        group->setText(body + (annotation.isEmpty() ? QString() : " （" + annotation + "）"));
        group->setData(annotation, BodyAnnotationRole);
    }
}
QModelIndex CaseTreeModel::locate(const QString& pointer, const QString& source) const {
    QModelIndex best;
    std::function<void(const QModelIndex&)> visit = [&](const QModelIndex& parent) {
        for (int i = 0; i < rowCount(parent); ++i) {
            const auto child = index(i, 0, parent);
            const auto path = child.data(PointerRole).toString();
            if (child.data(SourceRole) == source &&
                (pointer == path || pointer.startsWith(path + "/")) &&
                (!best.isValid() || path.size() > best.data(PointerRole).toString().size()))
                best = child;
            visit(child);
        }
    };
    visit({});
    return best;
}
bool CaseTreeModel::apply(const Json& value) {
    static const SchemaValidator validator(
        QStringLiteral(CHIPCAE_REPOSITORY "/schema/case-1.0.schema.json"));
    if (!validator.validate(value).empty())
        return false;
    document_->applyJson(value);
    return document_->toJson() == value;
}
QString CaseTreeModel::freshId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QString CaseTreeModel::addLoad(Json load) {
    auto copy = document_->toJson();
    if (!copy.contains("physics") || !copy.contains("loads"))
        return {};
    if (!load.is_object() || !load.contains("type") || !load.at("type").is_string())
        return {};
    const auto type = load.at("type").as<std::string>();
    bool thermal = false;
    for (const auto& p : copy.at("physics").array_range())
        thermal |= p.at("id") == "thermal";
    if ((type == "uniform_temperature") == thermal)
        return {};
    if (!thermal && !copy.at("loads").empty())
        return {};
    auto id = freshId();
    load["id"] = id.toStdString();
    copy["loads"].push_back(load);
    return apply(copy) ? "case:" + id : QString();
}
QString CaseTreeModel::addFixedFace(const QString& target) {
    auto copy = document_->toJson();
    if (!copy.contains("constraints"))
        return {};
    if (!copy["constraints"].contains("fixed"))
        copy["constraints"] = Json::parse(R"({"fixed":[]})");
    const auto id = freshId();
    Json face = Json::object();
    face["target"] = target.toStdString();
    face["id"] = id.toStdString();
    copy["constraints"]["fixed"].push_back(face);
    return apply(copy) ? "case:" + id : QString();
}
bool CaseTreeModel::mutate(const QString& identity, const Json* value) {
    auto matches =
        match(index(0, 0), IdentityRole, identity, 1, Qt::MatchExactly | Qt::MatchRecursive);
    if (matches.empty())
        return false;
    const auto node = matches.front();
    auto path = node.data(PointerRole).toString().toStdString();
    if (node.data(SourceRole) != "case" ||
        !(path.starts_with("/loads/") || path.starts_with("/constraints/fixed/")))
        return false;
    auto copy = document_->toJson();
    const auto slash = path.find_last_of('/');
    if (path.substr(0, slash) != "/loads" && path.substr(0, slash) != "/constraints/fixed")
        return false;
    if (value) {
        if (!value->is_object())
            return false;
        auto replacement = *value;
        const auto old = jsoncons::jsonpointer::get(copy, path);
        if (old.contains("id"))
            replacement["id"] = old.at("id");
        else
            replacement.erase("id");
        jsoncons::jsonpointer::replace(copy, path, replacement);
    } else {
        jsoncons::jsonpointer::remove(copy, path);
        if (copy.at("constraints").contains("fixed") && copy.at("constraints").at("fixed").empty())
            copy["constraints"] = Json::parse(R"({"rigid":"3-2-1"})");
    }
    return apply(copy);
}
bool CaseTreeModel::remove(const QString& identity) {
    return mutate(identity, nullptr);
}
bool CaseTreeModel::edit(const QString& identity, Json value) {
    return mutate(identity, &value);
}
void CaseTreeModel::setThermalMode(bool thermal) {
    auto copy = document_->toJson();
    if (!copy.contains("physics"))
        return;
    bool alreadyThermal = false;
    for (const auto& p : copy.at("physics").array_range())
        alreadyThermal = alreadyThermal || p.at("id") == "thermal";
    if (alreadyThermal == thermal)
        return;
    Json physics = Json::array();
    for (auto p : copy.at("physics").array_range()) {
        if (p.at("id") == "thermal")
            continue;
        if (p.at("id") == "mech") {
            if (thermal) {
                p["temp_from"] = "thermal";
                if (!p.contains("T_ref"))
                    p["T_ref"] = 298.15;
            } else
                p.erase("temp_from");
        }
        physics.push_back(p);
    }
    if (thermal)
        physics.push_back(
            Json::parse(R"({"id":"thermal","solver":"fem.conduction","mode":"steady"})"));
    copy["physics"] = physics;
    Json loads = Json::array();
    for (const auto& l : copy.at("loads").array_range())
        if ((l.at("type") == "uniform_temperature") != thermal)
            loads.push_back(l);
    if (loads.empty()) {
        auto l = Json::parse(
            thermal ? R"({"type":"fixed_temperature","target":"face:zmin","T":298.15})"
                    : R"({"type":"uniform_temperature","T_ref":298.15,"T_target":398.15})");
        l["id"] = freshId().toStdString();
        loads.push_back(l);
    }
    copy["loads"] = loads;
    if (!copy["output"].contains("fields"))
        copy["output"]["fields"] =
            Json::parse(R"(["T","U","layer_id","material_id","cell_kind","elem_gid"])");
    apply(copy);
}
} // namespace chipcae::desktop
