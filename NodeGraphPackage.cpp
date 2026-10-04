// Package entry point for the deki-nodegraph DLL.
//
// deki-nodegraph holds the whole node graph feature: the runtime container
// (NodeGraphData) and factory every platform needs, and the editor's
// registries, document, undo commands and generic Node Graph window.
//
// It declares no node types or components of its own. Node types and graph
// domains come from the packages and project DLLs that use it (deki-fsm's
// states and actions, a game's own nodes), through DEKI_NODE and the
// registration macros in DekiNode.h.

#include <deki/interop/Plugin.h>
#include "deki-nodegraph/DekiNode.h"
#include <deki/LogSystem.h>

extern void DekiNodeGraphRegisterComponents();
extern int DekiNodeGraphGetAutoComponentCount();
extern const Deki::ComponentMeta* DekiNodeGraphGetAutoComponentMeta(int index);

namespace DekiNodeGraph
{

#ifdef DEKI_EDITOR
// The codegen emits the three registration helpers declared above even with
// no components, so the plugin interface below has something to call.
#endif

static bool s_NodeGraphRegistered = false;

}  // namespace DekiNodeGraph
// The exports below are C symbols at global scope; the package's own
// registration helpers and statics live in its namespace.
using namespace DekiNodeGraph;

extern "C"
{
    DEKI_NODEGRAPH_API int DekiNodeGraphEnsureRegistered(void)
    {
#ifdef DEKI_EDITOR
        if (s_NodeGraphRegistered)
        {
            return ::DekiNodeGraphGetAutoComponentCount();
        }
        s_NodeGraphRegistered = true;
        ::DekiNodeGraphRegisterComponents();
        return ::DekiNodeGraphGetAutoComponentCount();
#else
        return 0;
#endif
    }

    DEKI_PLUGIN_API const char* DekiPluginGetName(void)
    {
        return "Deki Node Graph Package";
    }

    DEKI_PLUGIN_API const char* DekiPluginGetVersion(void)
    {
#ifdef DEKI_PACKAGE_VERSION
        return DEKI_PACKAGE_VERSION;
#else
        return "0.0.0-dev";
#endif
    }

    DEKI_PLUGIN_API int DekiPluginInit(void)
    {
        return 0;
    }

    DEKI_PLUGIN_API void DekiPluginShutdown(void)
    {
        s_NodeGraphRegistered = false;
    }

#ifdef DEKI_EDITOR
    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return ::DekiNodeGraphGetAutoComponentCount();
    }
    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int index)
    {
        return ::DekiNodeGraphGetAutoComponentMeta(index);
    }
#else
    DEKI_PLUGIN_API int DekiPluginGetComponentCount(void)
    {
        return 0;
    }
    DEKI_PLUGIN_API const Deki::ComponentMeta* DekiPluginGetComponentMeta(int)
    {
        return nullptr;
    }
#endif

    DEKI_PLUGIN_API void DekiPluginRegisterComponents(void)
    {
        DekiNodeGraphEnsureRegistered();
    }

#ifdef DEKI_EDITOR
    /// Drops every registered node type, factory thunk and graph domain.
    ///
    /// The editor calls this on each loaded package before it unloads plugin
    /// or package DLLs: the registries hold pointers into those DLLs, and
    /// reading them after FreeLibrary crashes. Doing it here keeps node graph
    /// knowledge out of the editor, which only asks each package to clean up.
    ///
    /// Users register again afterwards: a full reload reruns their static
    /// registrars, a plugin-only reload goes through
    /// ::DekiPluginRegisterComponents (see deki-fsm's DekiFsmRegisterGraphTypes).
    DEKI_PLUGIN_API void DekiPluginClearRegistries(void)
    {
        SceneFormat::NodeFactory::Instance().Clear();
        NodeTypeRegistry::Instance().Clear();
        NodeGraphDomainRegistry::Instance().Clear();
    }
#endif

    // No engine systems to install at load.

}  // extern "C"
