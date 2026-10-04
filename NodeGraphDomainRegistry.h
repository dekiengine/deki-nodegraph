#pragma once

#ifdef DEKI_EDITOR
#include <string>
#include <vector>
#include <unordered_map>
#include "deki-nodegraph/NodeGraphApi.h"
#include "deki-nodegraph/NodeGraphPreview.h"
#endif

namespace DekiNodeGraph
{

#ifdef DEKI_EDITOR

// Editor-only registry of node graph domains: which .asset types are node
// graphs, and which node categories belong to each.
//
// The owning package or project DLL registers a domain (with
// REGISTER_NODE_GRAPH_DOMAIN in DekiNode.h); the editor hardcodes none.
// NodeGraphEditorWindow opens an asset type only if a domain exists for it,
// and offers the node types whose category's first segment
// ("Domain/MenuGroup") is the domain's domainKey. Clear() runs on hot-reload
// teardown, so no pointers into an unloaded DLL survive.

/// One node graph domain. Its strings live in the owning DLL's static storage.
struct DekiNodeGraphDomain
{
    const char* assetTypeName;      // .asset "type" + AssetManager loader key
    const char* displayName;        // for people ("Hero Behavior Graph")
    const char* domainKey;          // first category segment of its nodes ("HeroBehavior")
    const char* entryNodeTypeName;  // node type new graphs start with ("FsmEntry")

    // APPEND-ONLY past this point, as in DekiNodeMeta: the editor reads domains
    // from package DLLs across hot reload, so a field inserted mid-struct would
    // shift offsets under a running editor.

    // Optional live preview (see NodeGraphPreview.h). All null means no
    // Preview panel for this domain.
    NodeGraphPreviewOps preview{};

    // Optional per-node illustration in the properties panel (see
    // NodeGraphNodeGizmoOps). Null means the panel shows fields only.
    NodeGraphNodeGizmoOps gizmos{};
};

class DEKI_NODEGRAPH_API NodeGraphDomainRegistry
{
public:
    static NodeGraphDomainRegistry& Instance();

    void Register(const DekiNodeGraphDomain* domain);

    const DekiNodeGraphDomain* Get(const std::string& assetTypeName) const;

    /// Every registered domain, in registration order.
    const std::vector<const DekiNodeGraphDomain*>& GetAll() const { return m_Domains; }

    /// Removes every domain, for hot-reload teardown.
    void Clear();

private:
    NodeGraphDomainRegistry() = default;
    NodeGraphDomainRegistry(const NodeGraphDomainRegistry&) = delete;
    NodeGraphDomainRegistry& operator=(const NodeGraphDomainRegistry&) = delete;

    std::vector<const DekiNodeGraphDomain*> m_Domains;
    std::unordered_map<std::string, const DekiNodeGraphDomain*> m_ByAssetType;
};

#endif  // DEKI_EDITOR

}  // namespace DekiNodeGraph
