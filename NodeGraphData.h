#pragma once

// Runtime container for a compiled node graph asset (all platforms).
//
// The editor's NodeGraphEditorWindow writes a node graph .asset as JSON, and
// the generic data-asset path compiles it to MessagePack (json::to_msgpack
// without the "type" key). This class parses that into type-erased node
// instances (through NodeFactory) and a link table. What pins and links mean
// is up to the tool's interpreter; the container knows no domain.
//
// Wire schema (keys in nlohmann's alphabetical order):
//   { "links":      [ { "from": u32, "fromPin": i32, "to": u32, "toPin": i32 }, ... ],
//     "nextNodeId": u32,   // editor's id counter; kept but unused at runtime
//     "nodes":      [ { "children": [ { "enabled": bool, "type": "...", "values": { ... } }, ... ],
//                       "graph": { "links": [ ... ], "nodes": [ ... ] },
//                       "id": u32, "type": "NodeTypeName", "values": { ... },
//                       "x": f, "y": f }, ... ] }
//
// In a node map the alphabetical order children < graph < id < type < values
// < x < y means "type" is read (instance created) before "values" (instance
// filled) in one pass; the same enabled < type < values order holds in each
// child map. A file where "values" comes before "type" is corrupt and fails to
// load with an error.
//
// A node carries at most one of two kinds of contents:
//   - "children": an ordered stack of child node instances the parent owns
//     (DEKI_NODE_CHILDREN, such as an FSM graph's variable declarations).
//     Children are full NodeFactory instances but have no links.
//   - "graph": an inner graph the parent owns (DEKI_NODE_SUBGRAPH, such as an
//     FSM state's action flow, or a group of states). An inner graph is a Graph
//     like the root, nested to any depth, with its own links.
//
// Node ids are unique across the whole document, so an id names one node
// however deep it sits. A link's ends are always in the link's own graph, so
// following pins never leaves a graph; crossing into or out of an inner graph
// is the interpreter's job.
//
// Loading is all-or-nothing: any error destroys every instance created so far
// and returns nullptr.

#include "deki-nodegraph/NodeGraphApi.h"

#include <cstdint>
#include <cstddef>
#include <vector>

namespace DekiNodeGraph
{

class DEKI_NODEGRAPH_API NodeGraphData
{
public:
    // One child in a node's ordered stack (see DEKI_NODE_CHILDREN). Children
    // are NodeFactory instances like top-level nodes but have no id and no
    // links; what they mean ("these are the variables") is up to the consumer.
    struct ChildInstance
    {
        uint32_t typeId = 0;       // Deki::HashString(child type name)
        void* instance = nullptr;  // made by NodeFactory, filled from "values"
        bool enabled = true;       // editor toggle; disabled children are data only
    };

    struct Graph;

    struct NodeInstance
    {
        uint32_t id = 0;           // document-unique node id (link endpoints)
        uint32_t typeId = 0;       // Deki::HashString(node type name)
        void* instance = nullptr;  // made by NodeFactory, filled from "values"
        // Appended last (the append-only rule of DekiNodeMeta), so an older
        // reader still finds id/typeId/instance at their offsets.
        std::vector<ChildInstance> children;
        Graph* inner = nullptr;  // DEKI_NODE_SUBGRAPH contents (owned), or nullptr
    };

    struct Link
    {
        uint32_t fromNode = 0;
        int32_t fromPin = 0;
        uint32_t toNode = 0;
        int32_t toPin = 0;
    };

    /// One graph level: the document root, or a node's inner graph. Every
    /// query stays within this level, as every link does.
    struct DEKI_NODEGRAPH_API Graph
    {
        std::vector<NodeInstance> nodes;
        std::vector<Link> links;

        const NodeInstance* FindNode(uint32_t id) const;
        const NodeInstance* FindFirstOfType(uint32_t typeId) const;

        /// Follows the link leaving (nodeId, fromPin) to its node in this
        /// graph, or nullptr if there is none. If several links leave one pin
        /// (the editor never makes that), the first wins.
        const NodeInstance* Next(uint32_t nodeId, int32_t fromPin) const;
    };

    /// Parses a compiled node graph MessagePack blob. Logs an error and
    /// returns nullptr on a structural error, an unknown node type, or a node
    /// that fails to deserialize.
    static NodeGraphData* LoadFromMemory(const uint8_t* data, size_t size);

    ~NodeGraphData();

    NodeGraphData(const NodeGraphData&) = delete;
    NodeGraphData& operator=(const NodeGraphData&) = delete;

    /// The top-level graph. Inner graphs are reached through NodeInstance::inner.
    const Graph& Root() const { return m_Root; }

    /// Shorthands for the root graph, for interpreters that never descend.
    const std::vector<NodeInstance>& Nodes() const { return m_Root.nodes; }
    const std::vector<Link>& Links() const { return m_Root.links; }
    const NodeInstance* FindNode(uint32_t id) const { return m_Root.FindNode(id); }
    const NodeInstance* FindFirstOfType(uint32_t typeId) const { return m_Root.FindFirstOfType(typeId); }
    const NodeInstance* Next(uint32_t nodeId, int32_t fromPin) const { return m_Root.Next(nodeId, fromPin); }

private:
    NodeGraphData() = default;

    Graph m_Root;
};

}  // namespace DekiNodeGraph
