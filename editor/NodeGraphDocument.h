#pragma once

// The editor's model of one open node graph .asset.
//
// Owns the live node instances (made from NodeTypeRegistry metas with
// createFunc), the link table and the layout. Only the commands in
// NodeGraphCommands.h call the mutators, so every edit can be undone, and
// every mutation sets `dirty`. Commands hold the document by weak_ptr: once
// the window closes (or hot reload destroys it), old undo entries log an
// error and do nothing instead of touching freed instances.
//
// Nesting: a document is a tree of graphs, the root plus one inner graph per
// node whose type has DEKI_NODE_SUBGRAPH (an FSM state's action flow, a group
// of states). Node ids are unique across the whole document, so a plain
// `uint32_t nodeId` names one node however deep it sits, and commands never
// carry a path. Links are always local: both ends are in one graph, the one
// containing `fromNode`.
//
// It saves in the node graph .asset schema (see NodeGraphData.h); the generic
// data-asset pipeline converts the saved JSON to the msgpack runtime cache.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DekiNodeGraph
{
struct DekiNodeMeta;
}
namespace DekiNodeGraph
{
struct DekiNodeGraphDomain;
}
namespace DekiEditor
{

// One entry in a node's ordered child stack (see DEKI_NODE_CHILDREN; an FSM
// graph's variable declarations, say). Children are live instances like nodes
// but have no id, canvas position or links; they are edited in the parent's
// inspector panel and saved inside the parent's entry.
struct NodeGraphDocChild
{
    const DekiNodeGraph::DekiNodeMeta* meta = nullptr;
    void* instance = nullptr;  // meta->createFunc(); owned by the document
    bool enabled = true;
};

struct NodeGraphDocGraph;

struct NodeGraphDocNode
{
    uint32_t id = 0;
    const DekiNodeGraph::DekiNodeMeta* meta = nullptr;
    void* instance = nullptr;                 // meta->createFunc(); owned by the document
    float x = 0.0f, y = 0.0f;                 // canvas layout (graph units)
    std::vector<NodeGraphDocChild> children;  // ordered child stack (may be empty)
    // Inner graph of a DEKI_NODE_SUBGRAPH node (owned by the document, freed by
    // DestroyInstances). nullptr for an ordinary flat node.
    NodeGraphDocGraph* inner = nullptr;
};

struct NodeGraphDocLink
{
    uint32_t fromNode = 0;
    int fromPin = 0;
    uint32_t toNode = 0;
    int toPin = 0;

    bool operator==(const NodeGraphDocLink& o) const
    {
        return fromNode == o.fromNode && fromPin == o.fromPin && toNode == o.toNode && toPin == o.toPin;
    }
};

// One graph level: the document root, or a subgraph node's contents. Both are
// edited the same way; the canvas draws whichever one is open.
struct NodeGraphDocGraph
{
    std::vector<NodeGraphDocNode> nodes;
    std::vector<NodeGraphDocLink> links;
};

class NodeGraphDocument : public std::enable_shared_from_this<NodeGraphDocument>
{
public:
    ~NodeGraphDocument() { DestroyInstances(); }

    std::string assetPath;
    std::string cachePath;
    const DekiNodeGraph::DekiNodeGraphDomain* domain = nullptr;

    NodeGraphDocGraph root;
    uint32_t nextNodeId = 1;  // document-wide: ids are unique across all levels
    bool dirty = false;

    // ---- Graph lookup ----

    /// The graph owned by `ownerNodeId` (0 = the document root), or nullptr if
    /// that node does not exist or is not a subgraph node.
    NodeGraphDocGraph* GraphOf(uint32_t ownerNodeId);
    const NodeGraphDocGraph* GraphOf(uint32_t ownerNodeId) const;

    /// The graph that contains `nodeId` (holds it in `nodes`).
    NodeGraphDocGraph* GraphContaining(uint32_t nodeId);
    const NodeGraphDocGraph* GraphContaining(uint32_t nodeId) const;

    /// Id of the subgraph node containing `nodeId`, or 0 at the root. Also 0
    /// when `nodeId` is unknown, so check with FindNode first.
    uint32_t OwnerOf(uint32_t nodeId) const;

    // ---- Mutators (commands only; every one sets dirty) ----

    /// Creates a node of `typeId` at (x, y) in `ownerNodeId`'s graph (0 =
    /// root). Returns the new id, or 0 (logged) if the type is not registered
    /// or the owner has no inner graph. A subgraph node gets its inner graph
    /// and its declared entry node.
    uint32_t AddNode(uint32_t typeId, float x, float y, uint32_t ownerNodeId = 0);

    /// For undo/redo: recreates a node with a given id and values in
    /// `ownerNodeId`'s graph, moving nextNodeId past `id` if needed. Returns
    /// false (logged) on an unknown type or bad values. Adds no subgraph entry
    /// node: the caller restores the exact inner graph with NodeFromJson.
    bool AddNodeWithId(uint32_t id, const std::string& typeName, float x, float y, const nlohmann::json& values,
                       uint32_t ownerNodeId = 0);

    /// Removes a node, every link touching it in its graph, and its inner
    /// graph (recursively). Returns false if there is no such node.
    bool RemoveNode(uint32_t id);

    bool AddLink(const NodeGraphDocLink& link);
    bool RemoveLink(const NodeGraphDocLink& link);
    /// Removes the link leaving (fromNode, fromPin) and returns it in outRemoved.
    bool RemoveLinkFromPin(uint32_t fromNode, int fromPin, NodeGraphDocLink* outRemoved);

    void SetNodePos(uint32_t id, float x, float y);

    // ---- Subgraph ----

    /// Gives `nodeId` an inner graph if its type declares one, adding the entry
    /// node named by meta->subgraphEntry when the graph is empty. Safe to call
    /// again; runs on add, on load, and when the canvas enters a node.
    bool EnsureSubgraph(uint32_t nodeId);

    /// One node as a complete subtree (values, child stack, inner graph and its
    /// links): everything undo needs to bring a deleted subgraph node back as
    /// it was.
    nlohmann::json NodeToJson(const NodeGraphDocNode& node) const;

    /// Recreates a subtree from NodeToJson in `ownerNodeId`'s graph. Fails
    /// (logged) on unknown types or bad values.
    bool NodeFromJson(uint32_t ownerNodeId, const nlohmann::json& jn);

    // ---- Child stack mutators (commands only; see NodeGraphDocChild) ----

    /// Inserts a default child of `childTypeId` at `index` (clamped; -1 =
    /// append). Returns the index used, or -1 on an unknown type or node.
    int AddChild(uint32_t nodeId, uint32_t childTypeId, int index);

    /// For undo/redo: recreates one child with the given values and enabled flag.
    bool AddChildFromJson(uint32_t nodeId, int index, const std::string& typeName, const nlohmann::json& values,
                          bool enabled);

    bool RemoveChild(uint32_t nodeId, int index);
    bool MoveChild(uint32_t nodeId, int from, int to);
    bool SetChildEnabled(uint32_t nodeId, int index, bool enabled);

    /// The whole child stack as the saved "children" array (for undo and
    /// ToJson); an empty array when there are none.
    nlohmann::json ChildrenToJson(const NodeGraphDocNode& node) const;

    /// Replaces a node's child stack from a "children" array (for undo). Fails
    /// (logged, stack left empty) on unknown types or bad values.
    bool ChildrenFromJson(uint32_t nodeId, const nlohmann::json& children);

    // ---- Queries ----

    /// Searches every graph level: one id is one node, anywhere.
    NodeGraphDocNode* FindNode(uint32_t id);
    const NodeGraphDocNode* FindNode(uint32_t id) const;

    /// Links touching `nodeId` within the graph that contains it.
    std::vector<NodeGraphDocLink> AttachedLinks(uint32_t nodeId) const;
    const NodeGraphDocLink* FindLinkFromPin(uint32_t fromNode, int fromPin) const;

    /// A node's output pin count, reading meta->dynamicOutputsProperty from the
    /// live instance (an Int32 gives its value, an Array its size).
    int OutputPinCount(const NodeGraphDocNode& node) const;

    // ---- Serialization ----

    /// Full .asset body, including "type" (the domain's asset type name).
    nlohmann::json ToJson() const;

    /// Rebuilds from a .asset body. On unknown node types or bad values it
    /// fails, sets outError and empties the document; never a partial load.
    bool FromJson(const nlohmann::json& j, std::string& outError);

    /// Destroys every node instance and inner graph; safe to call again. The
    /// window's destructor runs it while the owning DLLs are still loaded
    /// (ToolHost::ClearWindows).
    void DestroyInstances();
};

}  // namespace DekiEditor
