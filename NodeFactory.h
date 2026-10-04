#pragma once

// Type-erased factory for node graph node types: create, deserialize, destroy.
//
// Like ComponentFactory, but kept separate on purpose: node types are not
// components and must stay out of the scene systems. Nodes are plain reflected
// structs (DEKI_NODE) with no common base class, so the factory stores void*
// create/destroy functions and a deserialize thunk, keyed by the type's name
// hash.
//
// Each node's generated .gen.cpp registers itself with REGISTER_RUNTIME_NODE
// (DekiNode.h) when its DLL loads, so new node types come from package or
// project DLLs with no editor rebuild.

#include "deki-nodegraph/NodeGraphApi.h"

#include <cstdint>
#include <cstddef>
#include <unordered_map>

namespace Deki::SceneFormat
{
class SceneMsgPackParser;
}

namespace DekiNodeGraph
{

namespace SceneFormat
{

using NodeCreateFn = void* (*)();
using NodeDeserializeFn = bool (*)(void* node, ::Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize);
using NodeDestroyFn = void (*)(void* node);

struct NodeFactoryEntry
{
    NodeCreateFn create = nullptr;
    NodeDeserializeFn deserialize = nullptr;
    NodeDestroyFn destroy = nullptr;
};

/// Factory for node types, looked up by type id (the name hash).
class DEKI_NODEGRAPH_API NodeFactory
{
public:
    static NodeFactory& Instance();

    void Register(uint32_t typeId, NodeCreateFn create, NodeDeserializeFn deserialize, NodeDestroyFn destroy)
    {
        m_Entries[typeId] = { create, deserialize, destroy };
    }

    void Unregister(uint32_t typeId) { m_Entries.erase(typeId); }

    /// A default-constructed node of the type, or nullptr if the type is unknown.
    void* Create(uint32_t typeId)
    {
        auto it = m_Entries.find(typeId);
        return (it != m_Entries.end() && it->second.create) ? it->second.create() : nullptr;
    }

    /// Reads `node` (from Create) from a MessagePack map. Returns false if the
    /// type is unknown or has no deserialize thunk.
    bool Deserialize(uint32_t typeId, void* node, ::Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize)
    {
        auto it = m_Entries.find(typeId);
        return (it != m_Entries.end() && it->second.deserialize) ? it->second.deserialize(node, parser, mapSize)
                                                                 : false;
    }

    void Destroy(uint32_t typeId, void* node)
    {
        auto it = m_Entries.find(typeId);
        if (it != m_Entries.end() && it->second.destroy)
        {
            it->second.destroy(node);
        }
    }

    bool IsRegistered(uint32_t typeId) const { return m_Entries.count(typeId) > 0; }
    size_t GetRegisteredCount() const { return m_Entries.size(); }

    /// Removes every entry, for hot-reload teardown.
    void Clear() { m_Entries.clear(); }

private:
    NodeFactory() = default;
    std::unordered_map<uint32_t, NodeFactoryEntry> m_Entries;
};

}  // namespace SceneFormat

}  // namespace DekiNodeGraph
