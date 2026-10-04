#pragma once

// DLL export macro, in its own header so the package's headers (NodeFactory,
// NodeTypeRegistry, NodeGraphDomainRegistry, NodeGraphData) can use it without
// including DekiNode.h, which includes them.
//
// The node registries are shared singletons: node types register from
// whichever package or project DLL owns them (deki-fsm's states and actions, a
// game's own nodes), so every user must reach the one instance in
// deki-nodegraph.dll. Static runtime builds (embedded) have no DLLs and the
// macro is empty.
#ifdef DEKI_EDITOR
#ifdef _WIN32
#ifdef DEKI_NODEGRAPH_EXPORTS
#define DEKI_NODEGRAPH_API __declspec(dllexport)
#else
#define DEKI_NODEGRAPH_API __declspec(dllimport)
#endif
#else
#define DEKI_NODEGRAPH_API
#endif
#else
#define DEKI_NODEGRAPH_API
#endif
