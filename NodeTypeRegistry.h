#pragma once

#ifdef DEKI_EDITOR
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include "deki-nodegraph/NodeGraphApi.h"
#endif

namespace DekiNodeGraph
{

#ifdef DEKI_EDITOR

// Editor-only registry of node type metadata (DekiNodeMeta), which drives the
// node canvas's add-node menu and per-node inspector.
//
// Like ComponentRegistry but kept separate on purpose, since nodes are not
// components. Each node's generated REGISTER_NODE fills it when its DLL loads;
// Clear() runs on hot-reload teardown, so no meta pointers into an unloaded DLL
// survive.

struct DekiNodeMeta;  // full definition in deki-nodegraph/DekiNode.h

class DEKI_NODEGRAPH_API NodeTypeRegistry
{
public:
    static NodeTypeRegistry& Instance();

    /// `metaSize` is the registering DLL's compiled sizeof(DekiNodeMeta); pass
    /// it literally (REGISTER_NODE in DekiNode.h does), never a cached value.
    /// Both sides must agree on the layout, or fields are read at the wrong
    /// offset, and a const char* read from mid-struct is non-null garbage that
    /// passes null checks and faults far away. DekiNodeMeta is append-only, so
    /// an older DLL has a readable prefix, but "older and shorter" cannot be
    /// told from "reordered", so any mismatch is refused.
    ///
    /// Required on purpose: this header only forward-declares DekiNodeMeta
    /// (DekiNode.h includes this header), so there can be no default argument,
    /// and a caller who must pass it cannot skip the check.
    void Register(const DekiNodeMeta* meta, size_t metaSize);

    const DekiNodeMeta* GetMeta(uint32_t typeId) const;
    const DekiNodeMeta* GetMeta(const std::string& name) const;

    /// True once Register() has refused a meta over a layout mismatch. That
    /// package's node types are missing, which otherwise shows up only as
    /// "unknown node type 'X'" when a graph loads, pointing at the asset rather
    /// than the build. A loaded DLL's layout is fixed for the process, so hot
    /// reload cannot fix it and only an editor restart can. That is why
    /// Clear() never resets it.
    bool HasLayoutMismatch() const { return m_LayoutMismatch; }
    const std::string& LayoutMismatchDetail() const { return m_MismatchDetail; }

    /// Every registered node type, in registration order (for the add-node menu).
    const std::vector<const DekiNodeMeta*>& GetAllNodes() const { return m_Nodes; }

    /// Removes every node type, for hot-reload teardown.
    void Clear();

private:
    NodeTypeRegistry() = default;
    NodeTypeRegistry(const NodeTypeRegistry&) = delete;
    NodeTypeRegistry& operator=(const NodeTypeRegistry&) = delete;

    std::vector<const DekiNodeMeta*> m_Nodes;
    std::unordered_map<uint32_t, const DekiNodeMeta*> m_ByType;
    std::unordered_map<std::string, const DekiNodeMeta*> m_ByName;

    bool m_LayoutMismatch = false;
    std::string m_MismatchDetail;
};

#endif  // DEKI_EDITOR

}  // namespace DekiNodeGraph
