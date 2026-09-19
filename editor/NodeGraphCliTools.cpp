/**
 * @file NodeGraphCliTools.cpp
 * @brief Command-line / MCP tools that author node-graph assets.
 *
 * The editor knows nothing about node graphs, so the tools that build one come
 * with the package that defines them (deki-editor/CliTool.h). They open the
 * asset as a NodeGraphDocument - the same model, the same validation and the
 * same save format the Node Graph window uses - make one change, and save.
 *
 * Node ids are document-wide, so a node is named by its id however deep it
 * sits. A caller may pick ids (graph_add_node's `id`, and `entry_id` for the
 * entry a subgraph node is seeded with), which is what lets a `--script` wire
 * nodes it has just added without reading earlier results back.
 *
 * The rules the window's menus enforce are enforced here too: a node's type
 * must belong to the graph's domain and to the canvas it goes on (actions only
 * inside a state), permanent nodes are seeded rather than added, and a link
 * joins two nodes on one canvas through pins that exist.
 */
#ifdef DEKI_EDITOR

#include <deki-editor/CliTool.h>
#include <deki-editor/EditorRegistry.h>

#include "NodeGraphDocument.h"
#include "NodeGraphEditorWindow.h"
#include "NodePropertyJson.h"
#include "deki-nodegraph/DekiNode.h"
#include <deki/reflection/Property.h>
#include "deki-nodegraph/NodeGraphDomainRegistry.h"
#include "deki-nodegraph/NodeTypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace DekiEditor
{
namespace
{
using json = nlohmann::json;
namespace fs = std::filesystem;
using DekiNodeGraph::DekiNodeMeta;
using DekiNodeGraph::NodeTypeRegistry;

std::string DomainOf(const char* category)
{
    if (!category)
        return {};
    const char* slash = std::strchr(category, '/');
    return slash ? std::string(category, slash - category) : std::string(category);
}

// Categories that only ever appear inside something: a child stack's entries,
// and a subgraph's contents when they differ from the owner's own category.
bool IsInnerOnlyCategory(const char* category)
{
    for (const DekiNodeMeta* meta : NodeTypeRegistry::Instance().GetAllNodes())
    {
        if (meta->childCategory && std::strcmp(meta->childCategory, category) == 0)
            return true;
        if (meta->subgraphCategory && meta->subgraphCategory[0] != '\0' &&
            std::strcmp(meta->subgraphCategory, meta->category) != 0 &&
            std::strcmp(meta->subgraphCategory, category) == 0)
            return true;
    }
    return false;
}

struct OpenGraph
{
    std::string relativePath;
    std::shared_ptr<NodeGraphDocument> doc;
};

bool Load(const CliToolContext& ctx, const json& args, OpenGraph& out, std::string& error)
{
    if (ctx.projectPath.empty())
    {
        error = "no project is open";
        return false;
    }
    out.relativePath = args.value("asset", std::string());
    if (out.relativePath.empty())
    {
        error = "name the graph asset (project-relative path) in 'asset'";
        return false;
    }
    const fs::path abs = fs::path(ctx.projectPath) / out.relativePath;
    std::ifstream in(abs);
    if (!in)
    {
        error = "cannot read '" + out.relativePath + "'";
        return false;
    }
    const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        error = "'" + out.relativePath + "' is not JSON";
        return false;
    }
    const std::string type = j.value("type", std::string());
    const auto* domain = DekiNodeGraph::NodeGraphDomainRegistry::Instance().Get(type);
    if (!domain)
    {
        error = "'" + out.relativePath + "' is not a node graph ('" + type + "' is not a registered graph type)";
        return false;
    }
    out.doc = std::make_shared<NodeGraphDocument>();
    out.doc->domain = domain;
    out.doc->assetPath = abs.string();
    return out.doc->FromJson(j, error);
}

bool Save(const CliToolContext& ctx, OpenGraph& graph, std::string& error)
{
    std::ofstream f(graph.doc->assetPath, std::ios::trunc);
    f << graph.doc->ToJson().dump(2) << "\n";
    if (!f)
    {
        error = "could not write '" + graph.relativePath + "'";
        return false;
    }
    f.close();
    if (ctx.refreshAsset)
        ctx.refreshAsset(graph.relativePath);
    return true;
}

// An enum field may be given by its name ("SineOut"), as the inspector shows
// it; the document stores the index. Anything else passes through as given.
bool ResolveEnumNames(const DekiNodeMeta& meta, json& values, std::string& error)
{
    for (int i = 0; i < meta.propertyCount; ++i)
    {
        const Deki::PropertyInfo& p = meta.properties[i];
        if (p.type != Deki::PropertyType::Enum || !values.contains(p.name) || !values[p.name].is_string())
            continue;
        const std::string name = values[p.name].get<std::string>();
        int index = -1;
        for (int e = 0; e < p.enumCount; ++e)
            if (p.enumValues && p.enumValues[e] && name == p.enumValues[e])
                index = e;
        if (index < 0)
        {
            std::string names;
            for (int e = 0; e < p.enumCount; ++e)
                names += (names.empty() ? "" : ", ") + std::string(p.enumValues[e] ? p.enumValues[e] : "");
            error = std::string("'") + name + "' is not a value of " + meta.name + "." + p.name + " (" + names + ")";
            return false;
        }
        values[p.name] = index;
    }
    return true;
}

json CurrentValues(const NodeGraphDocNode& node)
{
    json values = json::object();
    if (node.meta && node.instance)
        NodePropertiesToJson(node.instance, *node.meta, values);
    return values;
}

// A pin given as an index or by its label. Output labels of a node with
// dynamic outputs (an FSM state's transitions) are the entries of the property
// that drives them.
bool ResolvePin(const NodeGraphDocument& doc, const NodeGraphDocNode& node, const json& pin, bool output,
                int& outIndex, std::string& error)
{
    std::vector<std::string> labels;
    if (output && node.meta->dynamicOutputsProperty)
    {
        const json values = CurrentValues(node);
        const json list = values.value(node.meta->dynamicOutputsProperty, json::array());
        if (list.is_array())
            for (const auto& entry : list)
                labels.push_back(entry.is_string() ? entry.get<std::string>() : std::string());
    }
    else
    {
        const char* const* names = output ? node.meta->outputPins : node.meta->inputPins;
        const int count = output ? node.meta->outputPinCount : node.meta->inputPinCount;
        for (int i = 0; i < count && names; ++i)
            labels.push_back(names[i] ? names[i] : "");
    }
    const int count = output ? doc.OutputPinCount(node) : node.meta->inputPinCount;

    if (pin.is_number_integer())
        outIndex = pin.get<int>();
    else if (pin.is_string())
    {
        const auto it = std::find(labels.begin(), labels.end(), pin.get<std::string>());
        outIndex = it == labels.end() ? -1 : static_cast<int>(it - labels.begin());
    }
    else
        outIndex = -1;

    if (outIndex < 0 || outIndex >= count)
    {
        std::string names;
        for (const auto& l : labels)
            names += (names.empty() ? "" : ", ") + l;
        error = std::string("node ") + std::to_string(node.id) + " (" + node.meta->name + ") has no " +
                (output ? "output" : "input") + " pin " + pin.dump() + " (it has " + std::to_string(count) +
                (names.empty() ? "" : ": " + names) + ")";
        return false;
    }
    return true;
}

// Base for the tools: parse, run, report.
class GraphTool : public CliTool
{
   public:
    bool Run(const CliToolContext& context, const std::string& argsJson, std::string& resultJson,
             std::string& error) override
    {
        const json args = json::parse(argsJson, nullptr, /*allow_exceptions=*/false);
        if (!args.is_object())
        {
            error = "arguments must be a JSON object";
            return false;
        }
        OpenGraph graph;
        if (!Load(context, args, graph, error))
            return false;
        json result;
        if (!Apply(graph, args, result, error))
            return false;
        if (Mutates() && !Save(context, graph, error))
            return false;
        resultJson = result.dump();
        return true;
    }

   protected:
    virtual bool Mutates() const { return true; }
    virtual bool Apply(OpenGraph& graph, const json& args, json& result, std::string& error) = 0;
};

class GraphAddNodeTool : public GraphTool
{
   public:
    const char* GetToolName() const override { return "graph_add_node"; }
    const char* GetToolDescription() const override
    {
        return "Add a node to a node-graph asset, at the root or inside a subgraph node (e.g. an FSM state's "
               "action flow). Optional `id` picks the node's id; a subgraph node is seeded with its entry node, "
               "whose id `entry_id` may pick. `values` sets the node's fields. Returns the ids.";
    }
    const char* GetInputSchema() const override
    {
        return R"json({"type":"object","required":["asset","type"],"properties":{
            "asset":{"type":"string","description":"Graph asset, project-relative"},
            "type":{"type":"string","description":"Node type name (e.g. FsmState, FsmTweenProperty)"},
            "owner":{"type":"integer","description":"Subgraph node to add it inside; 0 or absent = the root"},
            "id":{"type":"integer","description":"Id for the node (unused anywhere in the graph); default: the next free id"},
            "entry_id":{"type":"integer","description":"Id for the entry node a subgraph node is seeded with"},
            "x":{"type":"number"},"y":{"type":"number"},
            "values":{"type":"object","description":"Field values, as the node's inspector names them"}}})json";
    }

   protected:
    bool Apply(OpenGraph& graph, const json& args, json& result, std::string& error) override
    {
        NodeGraphDocument& doc = *graph.doc;
        const std::string typeName = args.value("type", std::string());
        const DekiNodeMeta* meta = NodeTypeRegistry::Instance().GetMeta(typeName);
        if (!meta || !meta->createFunc)
        {
            error = "no node type '" + typeName + "' is registered";
            return false;
        }
        if (DomainOf(meta->category) != doc.domain->domainKey)
        {
            error = "node type '" + typeName + "' does not belong to " + doc.domain->displayName + " graphs";
            return false;
        }
        if (meta->permanent)
        {
            error = "'" + typeName + "' is a permanent node: every graph has it already";
            return false;
        }

        const uint32_t owner = args.value("owner", 0u);
        if (owner == 0)
        {
            if (IsInnerOnlyCategory(meta->category))
            {
                error = "'" + typeName + "' (" + meta->category + ") only goes inside another node, not at the root";
                return false;
            }
        }
        else
        {
            const NodeGraphDocNode* ownerNode = doc.FindNode(owner);
            if (!ownerNode || !ownerNode->meta || !ownerNode->meta->subgraphCategory)
            {
                error = "node " + std::to_string(owner) + " does not exist or has no inside to add to";
                return false;
            }
            if (std::strcmp(ownerNode->meta->subgraphCategory, meta->category) != 0)
            {
                error = "'" + typeName + "' (" + meta->category + ") does not go inside a " + ownerNode->meta->name +
                        " (which takes " + ownerNode->meta->subgraphCategory + ")";
                return false;
            }
        }

        const float x = args.value("x", 0.0f);
        const float y = args.value("y", 0.0f);
        uint32_t id = 0;
        if (args.contains("id"))
        {
            id = args["id"].get<uint32_t>();
            if (id == 0 || doc.FindNode(id))
            {
                error = "id " + std::to_string(id) + " is taken (or 0)";
                return false;
            }
            if (!doc.AddNodeWithId(id, typeName, x, y, json::object(), owner))
            {
                error = "could not add the node (see the log)";
                return false;
            }
            doc.EnsureSubgraph(id);
        }
        else
        {
            id = doc.AddNode(meta->typeId, x, y, owner);
            if (id == 0)
            {
                error = "could not add the node (see the log)";
                return false;
            }
        }

        NodeGraphDocNode* node = doc.FindNode(id);
        if (args.contains("values"))
        {
            json values = CurrentValues(*node);
            for (auto it = args["values"].begin(); it != args["values"].end(); ++it)
                values[it.key()] = it.value();
            if (!ResolveEnumNames(*meta, values, error))
            {
                doc.RemoveNode(id);
                return false;
            }
            if (!NodePropertiesFromJson(node->instance, *meta, values))
            {
                doc.RemoveNode(id);
                error = "a value does not fit its field on '" + typeName + "' (see the log)";
                return false;
            }
        }

        result = { { "id", id } };
        if (node->inner && !node->inner->nodes.empty())
        {
            uint32_t entry = node->inner->nodes.front().id;
            if (args.contains("entry_id"))
            {
                const uint32_t wanted = args["entry_id"].get<uint32_t>();
                if (wanted != entry)
                {
                    if (wanted == 0 || doc.FindNode(wanted))
                    {
                        error = "entry_id " + std::to_string(wanted) + " is taken (or 0)";
                        return false;
                    }
                    const NodeGraphDocNode& seeded = node->inner->nodes.front();
                    const std::string entryType = seeded.meta->name;
                    const float ex = seeded.x, ey = seeded.y;
                    doc.RemoveNode(entry);
                    if (!doc.AddNodeWithId(wanted, entryType, ex, ey, json::object(), id))
                    {
                        error = "could not re-create the entry node with id " + std::to_string(wanted);
                        return false;
                    }
                    entry = wanted;
                }
            }
            result["entryId"] = entry;
        }
        return true;
    }
};

class GraphConnectTool : public GraphTool
{
   public:
    const char* GetToolName() const override { return "graph_connect"; }
    const char* GetToolDescription() const override
    {
        return "Wire an output pin of one node to an input pin of another on the same canvas of a node-graph "
               "asset. Pins by index or label (an FSM state's outputs are its transitions, e.g. \"FINISHED\"). "
               "An output drives one wire: an existing wire from it is replaced.";
    }
    const char* GetInputSchema() const override
    {
        return R"json({"type":"object","required":["asset","from","to"],"properties":{
            "asset":{"type":"string"},
            "from":{"type":"integer","description":"Source node id"},
            "from_pin":{"description":"Output pin index or label (default 0)"},
            "to":{"type":"integer","description":"Destination node id"},
            "to_pin":{"description":"Input pin index or label (default 0)"}}})json";
    }

   protected:
    bool Apply(OpenGraph& graph, const json& args, json& result, std::string& error) override
    {
        NodeGraphDocument& doc = *graph.doc;
        const uint32_t from = args.value("from", 0u);
        const uint32_t to = args.value("to", 0u);
        const NodeGraphDocNode* fromNode = doc.FindNode(from);
        const NodeGraphDocNode* toNode = doc.FindNode(to);
        if (!fromNode || !toNode)
        {
            error = "no node " + std::to_string(fromNode ? to : from) + " in this graph";
            return false;
        }
        if (doc.GraphContaining(from) != doc.GraphContaining(to))
        {
            error = "nodes " + std::to_string(from) + " and " + std::to_string(to) +
                    " are on different canvases; a wire joins two nodes on one";
            return false;
        }
        NodeGraphDocLink link;
        link.fromNode = from;
        link.toNode = to;
        if (!ResolvePin(doc, *fromNode, args.value("from_pin", json(0)), true, link.fromPin, error) ||
            !ResolvePin(doc, *toNode, args.value("to_pin", json(0)), false, link.toPin, error))
            return false;

        NodeGraphDocLink replaced;
        const bool hadOne = doc.RemoveLinkFromPin(from, link.fromPin, &replaced);
        if (!doc.AddLink(link))
        {
            error = "could not add the wire";
            return false;
        }
        result = { { "from", from }, { "fromPin", link.fromPin }, { "to", to }, { "toPin", link.toPin } };
        if (hadOne)
            result["replaced"] = { { "to", replaced.toNode }, { "toPin", replaced.toPin } };
        return true;
    }
};

class GraphSetValuesTool : public GraphTool
{
   public:
    const char* GetToolName() const override { return "graph_set_values"; }
    const char* GetToolDescription() const override
    {
        return "Set fields of a node in a node-graph asset; fields not named keep their values. A PropertyRef "
               "field takes {\"object\":\"\",\"component\":\"Transform\",\"field\":\"y\"}.";
    }
    const char* GetInputSchema() const override
    {
        return R"json({"type":"object","required":["asset","node","values"],"properties":{
            "asset":{"type":"string"},"node":{"type":"integer"},"values":{"type":"object"}}})json";
    }

   protected:
    bool Apply(OpenGraph& graph, const json& args, json& result, std::string& error) override
    {
        NodeGraphDocNode* node = graph.doc->FindNode(args.value("node", 0u));
        if (!node || !node->meta || !node->instance)
        {
            error = "no node " + std::to_string(args.value("node", 0u)) + " in this graph";
            return false;
        }
        json values = CurrentValues(*node);
        const json& changes = args["values"];
        for (auto it = changes.begin(); it != changes.end(); ++it)
        {
            if (!values.contains(it.key()))
            {
                error = std::string("a ") + node->meta->name + " has no field '" + it.key() + "'";
                return false;
            }
            values[it.key()] = it.value();
        }
        if (!ResolveEnumNames(*node->meta, values, error))
            return false;
        if (!NodePropertiesFromJson(node->instance, *node->meta, values))
        {
            error = std::string("a value does not fit its field on ") + node->meta->name + " (see the log)";
            return false;
        }
        result = { { "node", node->id }, { "values", CurrentValues(*node) } };
        return true;
    }
};

class GraphAddChildTool : public GraphTool
{
   public:
    const char* GetToolName() const override { return "graph_add_child"; }
    const char* GetToolDescription() const override
    {
        return "Append an entry to a node's child stack in a node-graph asset - e.g. a variable (FsmNumberVar, "
               "FsmBoolVar, FsmTextVar) on an FSM graph's Variables node.";
    }
    const char* GetInputSchema() const override
    {
        return R"json({"type":"object","required":["asset","node","type"],"properties":{
            "asset":{"type":"string"},"node":{"type":"integer"},"type":{"type":"string"},
            "values":{"type":"object"}}})json";
    }

   protected:
    bool Apply(OpenGraph& graph, const json& args, json& result, std::string& error) override
    {
        NodeGraphDocument& doc = *graph.doc;
        const uint32_t nodeId = args.value("node", 0u);
        const NodeGraphDocNode* node = doc.FindNode(nodeId);
        if (!node || !node->meta || !node->meta->childCategory)
        {
            error = "node " + std::to_string(nodeId) + " does not exist or has no child stack";
            return false;
        }
        const std::string typeName = args.value("type", std::string());
        const DekiNodeMeta* meta = NodeTypeRegistry::Instance().GetMeta(typeName);
        if (!meta || !meta->category || std::strcmp(meta->category, node->meta->childCategory) != 0)
        {
            error = "'" + typeName + "' is not a child a " + node->meta->name + " takes (" +
                    node->meta->childCategory + ")";
            return false;
        }
        // Defaults, with the given values over them.
        void* scratch = meta->createFunc();
        json values = json::object();
        NodePropertiesToJson(scratch, *meta, values);
        meta->destroyFunc(scratch);
        if (args.contains("values"))
            for (auto it = args["values"].begin(); it != args["values"].end(); ++it)
                values[it.key()] = it.value();
        if (!ResolveEnumNames(*meta, values, error))
            return false;
        if (!doc.AddChildFromJson(nodeId, -1, typeName, values, true))
        {
            error = "a value does not fit its field on '" + typeName + "' (see the log)";
            return false;
        }
        result = { { "node", nodeId }, { "index", doc.FindNode(nodeId)->children.size() - 1 } };
        return true;
    }
};

class GraphGetTool : public GraphTool
{
   public:
    const char* GetToolName() const override { return "graph_get"; }
    const char* GetToolDescription() const override { return "Return a node-graph asset's whole content."; }
    const char* GetInputSchema() const override
    {
        return R"json({"type":"object","required":["asset"],"properties":{"asset":{"type":"string"}}})json";
    }

   protected:
    bool Mutates() const override { return false; }
    bool Apply(OpenGraph& graph, const json&, json& result, std::string&) override
    {
        result = graph.doc->ToJson();
        return true;
    }
};

// Point the Node Graph window at part of the graph it has open - for a
// screenshot, or to hand a person the exact place to look.
class GraphViewTool : public CliTool
{
   public:
    const char* GetToolName() const override { return "graph_view"; }
    const char* GetToolDescription() const override
    {
        return "In the Node Graph window (open the asset first with open_asset), show the canvas inside "
               "`canvas` (a subgraph node id, e.g. an FSM state; 0 = the root) and select node `select` so "
               "its fields show.";
    }
    const char* GetInputSchema() const override
    {
        return R"json({"type":"object","required":["asset"],"properties":{
            "asset":{"type":"string","description":"The graph the window must have open, project-relative"},
            "canvas":{"type":"integer","description":"Subgraph node whose inside to show; 0 = the root"},
            "select":{"type":"integer","description":"Node to select on that canvas; 0 = none"}}})json";
    }

    bool Run(const CliToolContext& context, const std::string& argsJson, std::string& resultJson,
             std::string& error) override
    {
        const json args = json::parse(argsJson, nullptr, /*allow_exceptions=*/false);
        if (!args.is_object())
        {
            error = "arguments must be a JSON object";
            return false;
        }
        NodeGraphEditorWindow* window = NodeGraphEditorWindow::Live();
        if (!window)
        {
            error = "there is no Node Graph window in this session";
            return false;
        }
        const std::string rel = args.value("asset", std::string());
        std::error_code ec;
        if (window->OpenAssetPath().empty() ||
            !fs::equivalent(fs::path(window->OpenAssetPath()), fs::path(context.projectPath) / rel, ec))
        {
            error = "the Node Graph window does not have '" + rel + "' open; open it with open_asset";
            return false;
        }
        if (!window->ShowCanvas(args.value("canvas", 0u), args.value("select", 0u), error))
            return false;
        resultJson = json({ { "canvas", args.value("canvas", 0u) }, { "selected", args.value("select", 0u) } }).dump();
        return true;
    }
};

}  // namespace

REGISTER_EDITOR(GraphViewTool)
REGISTER_EDITOR(GraphAddNodeTool)
REGISTER_EDITOR(GraphConnectTool)
REGISTER_EDITOR(GraphSetValuesTool)
REGISTER_EDITOR(GraphAddChildTool)
REGISTER_EDITOR(GraphGetTool)

}  // namespace DekiEditor

#endif  // DEKI_EDITOR
