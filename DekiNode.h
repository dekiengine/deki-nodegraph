#pragma once

// Node type metadata (DekiNodeMeta) and the self-registration macros for the
// node graph tool. Separate from components and scenes.
//
// A node type is a plain reflected struct marked DEKI_NODE (see DekiProperty.h)
// with DEKI_EXPORT fields. For each node the codegen emits:
//   - a runtime `bool DeserializeMsgPack(T&, SceneMsgPackParser&, uint32_t)` (all platforms)
//   - editor-only GetProperties()/GetPropertyCount()/GetNodeMeta()
//   - REGISTER_RUNTIME_NODE(T) and (editor) REGISTER_NODE(T), inline in T.gen.cpp
// so new node types register from their own DLL with no editor rebuild.

#include <deki/reflection/Property.h>
#include "deki-nodegraph/NodeFactory.h"
// For DekiHashString/DekiHashStringLen, used by the registration macros below
// and by every generated node .gen.cpp. Included here because package DLLs
// have no precompiled header to supply it.
#include <deki/Component.h>

#ifdef DEKI_EDITOR
#include "deki-nodegraph/NodeTypeRegistry.h"
#include "deki-nodegraph/NodeGraphDomainRegistry.h"
#endif

namespace DekiNodeGraph
{

/// Metadata for one node type (editor only; drives the node canvas).
struct DekiNodeMeta
{
    const char* name;                      // node type name (StaticNodeName); stable id
    const char* category;                  // "Domain/MenuGroup" (StaticNodeCategory)
    uint32_t typeId;                       // Deki::HashString(name)
    const Deki::PropertyInfo* properties;  // reflected fields (from GetProperties())
    int propertyCount;
    const char* const* inputPins;  // input pin labels (nullptr = no inputs)
    int inputPinCount;
    const char* const* outputPins;  // output pin labels (nullptr = no fixed outputs)
    int outputPinCount;
    const char* dynamicOutputsProperty;  // DEKI_EXPORT property driving output count
                                         // (nullptr = fixed outputs)
    void* (*createFunc)();               // new T()
    void (*destroyFunc)(void*);          // delete (T*)p
    size_t instanceSize;                 // sizeof(T)
    // APPEND-ONLY past this point. The editor reads metas from plugins across
    // hot reload, and a field inserted mid-struct shifts offsets and crashes an
    // editor not yet rebuilt. New fields go at the end, so an older editor reads
    // the fields it knows and ignores the rest (as with
    // Deki::PropertyInfo::physicalUnit).
    const char* displayName;                 // editor label (kStaticNodeDisplayName), or
                                             // nullptr to derive from name
    const char* childCategory = nullptr;     // full category of child node types this node
                                             // stacks (StaticNodeChildCategory), or nullptr
    const char* titleProperty = nullptr;     // String property used as the canvas title
                                             // (StaticNodeTitleProperty), or nullptr
    bool permanent = false;                  // fixed part of every graph of its domain
                                             // (StaticNodePermanent): auto-seeded, hidden
                                             // from the add menu, not deletable
    bool declaresVariables = false;          // DEKI_NODE_VARIABLES: this node's child stack
                                             // declares the graph's variables. Each child's
                                             // titleProperty is the variable NAME and its
                                             // other exported property is the initial VALUE,
                                             // whose reflected type is the variable's type.
    const char* subgraphCategory = nullptr;  // DEKI_NODE_SUBGRAPH: this node owns an INNER
                                             // GRAPH (double-click to descend) whose add-node
                                             // menu offers this category. nullptr = flat node.
    const char* subgraphEntry = nullptr;     // StaticNodeName of the node seeded inside every
                                             // instance's inner graph as its entry point
                                             // (nullptr/"" = seed nothing).
    const char* description = nullptr;       // one line on what the node does
                                             // (kStaticNodeDescription), shown under the
                                             // name in the add picker
};

// ---------------------------------------------------------------------------
// Self-registration (emitted inline in each node's .gen.cpp).
// ---------------------------------------------------------------------------

// Runtime (all platforms): registers create/deserialize/destroy in NodeFactory.
// DeserializeMsgPack(T&, ...) is the generated free function (declared in T.gen.h).
#define REGISTER_RUNTIME_NODE(ClassName)                                                                               \
    static struct ClassName##_NodeFactoryRegistrar                                                                     \
    {                                                                                                                  \
        ClassName##_NodeFactoryRegistrar()                                                                             \
        {                                                                                                              \
            ::DekiNodeGraph::SceneFormat::NodeFactory::Instance().Register(                                            \
                ::Deki::HashString(ClassName::StaticNodeName), []() -> void* { return new ClassName(); },              \
                [](void* p, ::Deki::SceneFormat::SceneMsgPackParser& parser, uint32_t mapSize) -> bool                 \
                { return DeserializeMsgPack(*static_cast<ClassName*>(p), parser, mapSize); },                          \
                [](void* p) { delete static_cast<ClassName*>(p); });                                                   \
        }                                                                                                              \
    } s_##ClassName##_NodeFactoryRegistrar

// Editor: registers the metadata in NodeTypeRegistry (add-node menu, inspector).
#ifdef DEKI_EDITOR
#define REGISTER_NODE(ClassName)                                                                                       \
    static struct ClassName##_NodeRegistrar                                                                            \
    {                                                                                                                  \
        ClassName##_NodeRegistrar()                                                                                    \
        {                                                                                                              \
            ::DekiNodeGraph::NodeTypeRegistry::Instance().Register(&ClassName::GetNodeMeta(),                          \
                                                                   sizeof(::DekiNodeGraph::DekiNodeMeta));             \
        }                                                                                                              \
    } s_##ClassName##_NodeRegistrar
#else
#define REGISTER_NODE(ClassName) /* editor-only */
#endif

// Editor: registers a node graph domain from the owning package or project
// DLL: which .asset type is a node graph, which category domain its nodes come
// from, and which node type new graphs start with. Place it at file scope in an
// editor-only, hand-written source file.
#ifdef DEKI_EDITOR
#define REGISTER_NODE_GRAPH_DOMAIN(VarName, AssetType, Display, DomainKey, EntryType)                                  \
    static const ::DekiNodeGraph::DekiNodeGraphDomain VarName{ AssetType, Display, DomainKey, EntryType };             \
    static struct VarName##_DomainRegistrar                                                                            \
    {                                                                                                                  \
        VarName##_DomainRegistrar() { ::DekiNodeGraph::NodeGraphDomainRegistry::Instance().Register(&VarName); }       \
    } s_##VarName##_DomainRegistrar

// The same, plus a live preview (NodeGraphPreviewOps), so the Node Graph
// window offers a Preview panel for this domain. `PreviewOps` is any
// expression giving a NodeGraphPreviewOps.
#define REGISTER_NODE_GRAPH_DOMAIN_PREVIEW(VarName, AssetType, Display, DomainKey, EntryType, PreviewOps)              \
    static const ::DekiNodeGraph::DekiNodeGraphDomain VarName{ AssetType, Display, DomainKey, EntryType, PreviewOps }; \
    static struct VarName##_DomainRegistrar                                                                            \
    {                                                                                                                  \
        VarName##_DomainRegistrar() { ::DekiNodeGraph::NodeGraphDomainRegistry::Instance().Register(&VarName); }       \
    } s_##VarName##_DomainRegistrar

// The same again, plus per-node gizmos (NodeGraphNodeGizmoOps), so the
// properties panel can illustrate the selected node.
#define REGISTER_NODE_GRAPH_DOMAIN_PREVIEW_GIZMOS(VarName, AssetType, Display, DomainKey, EntryType, PreviewOps,       \
                                                  GizmoOps)                                                            \
    static const ::DekiNodeGraph::DekiNodeGraphDomain VarName{ AssetType, Display,    DomainKey,                       \
                                                               EntryType, PreviewOps, GizmoOps };                      \
    static struct VarName##_DomainRegistrar                                                                            \
    {                                                                                                                  \
        VarName##_DomainRegistrar() { ::DekiNodeGraph::NodeGraphDomainRegistry::Instance().Register(&VarName); }       \
    } s_##VarName##_DomainRegistrar
#else
#define REGISTER_NODE_GRAPH_DOMAIN(VarName, AssetType, Display, DomainKey, EntryType) /* editor-only */
#define REGISTER_NODE_GRAPH_DOMAIN_PREVIEW(VarName, AssetType, Display, DomainKey, EntryType,                          \
                                           PreviewOps) /* editor-only */
#endif

}  // namespace DekiNodeGraph
