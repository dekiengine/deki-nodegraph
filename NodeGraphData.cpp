#include "deki-nodegraph/NodeGraphData.h"

#include "deki-nodegraph/NodeFactory.h"
#include <deki/Component.h>  // DekiHashString
#include <deki/SceneMessagePack.h>
#include <deki/LogSystem.h>

#include <string>

namespace DekiNodeGraph
{

using Deki::SceneFormat::SceneMsgPackParser;
using SceneFormat::NodeFactory;

namespace
{

// Reads a msgpack string (a key or short value) into a std::string.
bool ReadStdString(SceneMsgPackParser& parser, std::string& out)
{
    const char* str = nullptr;
    uint16_t len = 0;
    if (!parser.ReadString(str, len))
    {
        return false;
    }
    out.assign(str, len);
    return true;
}

void DestroyGraph(NodeGraphData::Graph& graph);

// Destroys a partly built node: its instance, children and inner graph so far.
// Recursive, since an inner graph's nodes may have inner graphs.
void DestroyPartialNode(NodeGraphData::NodeInstance& node)
{
    auto& factory = NodeFactory::Instance();
    for (auto& child : node.children)
    {
        if (child.instance)
        {
            factory.Destroy(child.typeId, child.instance);
        }
    }
    node.children.clear();
    if (node.inner)
    {
        DestroyGraph(*node.inner);
        delete node.inner;
        node.inner = nullptr;
    }
    if (node.instance)
    {
        factory.Destroy(node.typeId, node.instance);
    }
    node.instance = nullptr;
}

void DestroyGraph(NodeGraphData::Graph& graph)
{
    for (auto& node : graph.nodes)
    {
        DestroyPartialNode(node);
    }
    graph.nodes.clear();
    graph.links.clear();
}

// Parses one child map ({enabled, type, values}, alphabetical, so "type"
// creates the instance before "values" fills it, as in ParseNode). Appends to
// `node.children`, or logs and returns false; the caller's DestroyPartialNode
// then destroys any instance made here.
bool ParseChild(SceneMsgPackParser& parser, uint32_t mapSize, NodeGraphData::NodeInstance& node)
{
    NodeGraphData::ChildInstance child;
    auto& factory = NodeFactory::Instance();

    for (uint32_t i = 0; i < mapSize; ++i)
    {
        std::string key;
        if (!ReadStdString(parser, key))
        {
            DEKI_LOG_ERROR("NodeGraphData: failed to read child map key");
            if (child.instance)
            {
                factory.Destroy(child.typeId, child.instance);
            }
            return false;
        }

        if (key == "enabled")
        {
            bool enabled = true;
            if (!parser.ReadBool(enabled))
            {
                DEKI_LOG_ERROR("NodeGraphData: child 'enabled' is not a bool");
                if (child.instance)
                {
                    factory.Destroy(child.typeId, child.instance);
                }
                return false;
            }
            child.enabled = enabled;
        }
        else if (key == "type")
        {
            std::string typeName;
            if (!ReadStdString(parser, typeName))
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to read child type name");
                return false;
            }
            child.typeId = Deki::HashString(typeName.c_str());
            child.instance = factory.Create(child.typeId);
            if (!child.instance)
            {
                DEKI_LOG_ERROR("NodeGraphData: unknown child type '%s' (not registered in NodeFactory)",
                               typeName.c_str());
                return false;
            }
        }
        else if (key == "values")
        {
            if (!child.instance)
            {
                DEKI_LOG_ERROR("NodeGraphData: child 'values' encountered before 'type' (corrupt asset)");
                return false;
            }
            uint32_t valuesSize = 0;
            if (!parser.ReadMapSize(valuesSize))
            {
                DEKI_LOG_ERROR("NodeGraphData: child 'values' is not a map");
                factory.Destroy(child.typeId, child.instance);
                return false;
            }
            if (!factory.Deserialize(child.typeId, child.instance, parser, valuesSize))
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to deserialize child values in node id %u", node.id);
                factory.Destroy(child.typeId, child.instance);
                return false;
            }
        }
        else
        {
            if (!parser.SkipValue())
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to skip child key '%s'", key.c_str());
                if (child.instance)
                {
                    factory.Destroy(child.typeId, child.instance);
                }
                return false;
            }
        }
    }

    if (!child.instance)
    {
        DEKI_LOG_ERROR("NodeGraphData: child map missing 'type'");
        return false;
    }

    node.children.push_back(child);
    return true;
}

bool ParseGraph(SceneMsgPackParser& parser, uint32_t mapSize, NodeGraphData::Graph& graph);

// Parses one node map and appends it to `graph.nodes`, or logs and returns
// false; the caller then destroys everything created so far.
bool ParseNode(SceneMsgPackParser& parser, uint32_t mapSize, NodeGraphData::Graph& graph)
{
    NodeGraphData::NodeInstance node;
    auto& factory = NodeFactory::Instance();

    for (uint32_t i = 0; i < mapSize; ++i)
    {
        std::string key;
        if (!ReadStdString(parser, key))
        {
            DEKI_LOG_ERROR("NodeGraphData: failed to read node map key");
            DestroyPartialNode(node);
            return false;
        }

        if (key == "id")
        {
            int32_t id = 0;
            if (!parser.ReadInt(id) || id <= 0)
            {
                DEKI_LOG_ERROR("NodeGraphData: invalid node id");
                DestroyPartialNode(node);
                return false;
            }
            node.id = (uint32_t)id;
        }
        else if (key == "children")
        {
            uint32_t count = 0;
            if (!parser.ReadArraySize(count))
            {
                DEKI_LOG_ERROR("NodeGraphData: node 'children' is not an array");
                DestroyPartialNode(node);
                return false;
            }
            // Exact size up front: growing would leave up to half the block
            // unused and copy the children at each step. The same holds for
            // every reserve in this file; on a device with a few hundred KB of
            // heap, the waste and the copying matter more than parse time.
            node.children.reserve(count);
            for (uint32_t c = 0; c < count; ++c)
            {
                uint32_t childMapSize = 0;
                if (!parser.ReadMapSize(childMapSize))
                {
                    DEKI_LOG_ERROR("NodeGraphData: child %u is not a map", c);
                    DestroyPartialNode(node);
                    return false;
                }
                if (!ParseChild(parser, childMapSize, node))
                {
                    DestroyPartialNode(node);
                    return false;
                }
            }
        }
        else if (key == "graph")
        {
            // Inner graph (DEKI_NODE_SUBGRAPH), parsed like the root, to any
            // depth. "graph" sorts before "type", but nothing here needs the
            // owning node's instance.
            uint32_t innerSize = 0;
            if (!parser.ReadMapSize(innerSize))
            {
                DEKI_LOG_ERROR("NodeGraphData: node 'graph' is not a map");
                DestroyPartialNode(node);
                return false;
            }
            node.inner = new NodeGraphData::Graph();
            if (!ParseGraph(parser, innerSize, *node.inner))
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to parse inner graph of node id %u", node.id);
                DestroyPartialNode(node);
                return false;
            }
        }
        else if (key == "type")
        {
            std::string typeName;
            if (!ReadStdString(parser, typeName))
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to read node type name");
                DestroyPartialNode(node);
                return false;
            }
            node.typeId = Deki::HashString(typeName.c_str());
            node.instance = factory.Create(node.typeId);
            if (!node.instance)
            {
                DEKI_LOG_ERROR("NodeGraphData: unknown node type '%s' (not registered in NodeFactory)",
                               typeName.c_str());
                DestroyPartialNode(node);
                return false;
            }
        }
        else if (key == "values")
        {
            // Alphabetical key order puts "type" before "values"; a file
            // where it does not is corrupt.
            if (!node.instance)
            {
                DEKI_LOG_ERROR("NodeGraphData: node 'values' encountered before 'type' (corrupt asset)");
                DestroyPartialNode(node);
                return false;
            }
            uint32_t valuesSize = 0;
            if (!parser.ReadMapSize(valuesSize))
            {
                DEKI_LOG_ERROR("NodeGraphData: node 'values' is not a map");
                DestroyPartialNode(node);
                return false;
            }
            if (!factory.Deserialize(node.typeId, node.instance, parser, valuesSize))
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to deserialize values of node id %u", node.id);
                DestroyPartialNode(node);
                return false;
            }
        }
        else
        {
            // x / y (editor layout) and unknown keys are skipped.
            if (!parser.SkipValue())
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to skip node key '%s'", key.c_str());
                DestroyPartialNode(node);
                return false;
            }
        }
    }

    if (node.id == 0 || !node.instance)
    {
        DEKI_LOG_ERROR("NodeGraphData: node map missing 'id' or 'type'");
        DestroyPartialNode(node);
        return false;
    }

    graph.nodes.push_back(std::move(node));
    return true;
}

// Parses one link map ({"from", "fromPin", "to", "toPin"} in any order).
bool ParseLink(SceneMsgPackParser& parser, uint32_t mapSize, NodeGraphData::Link& out)
{
    for (uint32_t i = 0; i < mapSize; ++i)
    {
        std::string key;
        if (!ReadStdString(parser, key))
        {
            DEKI_LOG_ERROR("NodeGraphData: failed to read link map key");
            return false;
        }
        int32_t v = 0;
        if (!parser.ReadInt(v))
        {
            DEKI_LOG_ERROR("NodeGraphData: link key '%s' is not an int", key.c_str());
            return false;
        }
        if (key == "from")
        {
            out.fromNode = (uint32_t)v;
        }
        else if (key == "fromPin")
        {
            out.fromPin = v;
        }
        else if (key == "to")
        {
            out.toNode = (uint32_t)v;
        }
        else if (key == "toPin")
        {
            out.toPin = v;
        }
        else
        {
            DEKI_LOG_ERROR("NodeGraphData: unknown link key '%s'", key.c_str());
            return false;
        }
    }
    return true;
}

// Parses one graph map ("nodes" and "links"), for the root and every inner
// graph. The root also has "nextNodeId", an editor counter, skipped like any
// unknown key.
bool ParseGraph(SceneMsgPackParser& parser, uint32_t mapSize, NodeGraphData::Graph& graph)
{
    for (uint32_t i = 0; i < mapSize; ++i)
    {
        std::string key;
        if (!ReadStdString(parser, key))
        {
            DEKI_LOG_ERROR("NodeGraphData: failed to read graph map key");
            return false;
        }

        if (key == "nodes")
        {
            uint32_t count = 0;
            if (!parser.ReadArraySize(count))
            {
                DEKI_LOG_ERROR("NodeGraphData: 'nodes' is not an array");
                return false;
            }
            // Reserved, not grown: a NodeInstance owns vectors, so every
            // reallocation moves them all, and unused capacity here is the
            // costliest in the graph.
            graph.nodes.reserve(count);
            for (uint32_t n = 0; n < count; ++n)
            {
                uint32_t nodeMapSize = 0;
                if (!parser.ReadMapSize(nodeMapSize))
                {
                    DEKI_LOG_ERROR("NodeGraphData: node %u is not a map", n);
                    return false;
                }
                if (!ParseNode(parser, nodeMapSize, graph))
                {
                    return false;
                }
            }
        }
        else if (key == "links")
        {
            uint32_t count = 0;
            if (!parser.ReadArraySize(count))
            {
                DEKI_LOG_ERROR("NodeGraphData: 'links' is not an array");
                return false;
            }
            graph.links.reserve(count);
            for (uint32_t l = 0; l < count; ++l)
            {
                uint32_t linkMapSize = 0;
                if (!parser.ReadMapSize(linkMapSize))
                {
                    DEKI_LOG_ERROR("NodeGraphData: link %u is not a map", l);
                    return false;
                }
                NodeGraphData::Link link;
                if (!ParseLink(parser, linkMapSize, link))
                {
                    return false;
                }
                graph.links.push_back(link);
            }
        }
        else
        {
            // nextNodeId (editor counter) and unknown keys are skipped.
            if (!parser.SkipValue())
            {
                DEKI_LOG_ERROR("NodeGraphData: failed to skip graph key '%s'", key.c_str());
                return false;
            }
        }
    }
    return true;
}

}  // namespace

NodeGraphData* NodeGraphData::LoadFromMemory(const uint8_t* data, size_t size)
{
    if (!data || size == 0)
    {
        DEKI_LOG_ERROR("NodeGraphData: empty buffer");
        return nullptr;
    }

    SceneMsgPackParser parser(data, size);

    uint32_t rootSize = 0;
    if (!parser.ReadMapSize(rootSize))
    {
        DEKI_LOG_ERROR("NodeGraphData: root is not a map");
        return nullptr;
    }

    NodeGraphData* graph = new NodeGraphData();
    if (!ParseGraph(parser, rootSize, graph->m_Root))
    {
        delete graph;  // destroys the instances created so far
        return nullptr;
    }
    return graph;
}

NodeGraphData::~NodeGraphData()
{
    DestroyGraph(m_Root);
}

const NodeGraphData::NodeInstance* NodeGraphData::Graph::FindNode(uint32_t id) const
{
    for (const auto& node : nodes)
    {
        if (node.id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

const NodeGraphData::NodeInstance* NodeGraphData::Graph::FindFirstOfType(uint32_t typeId) const
{
    for (const auto& node : nodes)
    {
        if (node.typeId == typeId)
        {
            return &node;
        }
    }
    return nullptr;
}

const NodeGraphData::NodeInstance* NodeGraphData::Graph::Next(uint32_t nodeId, int32_t fromPin) const
{
    for (const auto& link : links)
    {
        if (link.fromNode == nodeId && link.fromPin == fromPin)
        {
            return FindNode(link.toNode);
        }
    }
    return nullptr;
}

}  // namespace DekiNodeGraph
