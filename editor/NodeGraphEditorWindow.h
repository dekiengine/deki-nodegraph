#pragma once

#include <deki-editor/EditorWindow.h>
#include <deki-editor/NodeCanvas.h>

#include "deki-nodegraph/editor/NodeGraphDocument.h"

#include <deki/reflection/Property.h>  // DekiPropertyType (by value below)

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace DekiNodeGraph
{
struct DekiNodeMeta;
}
namespace DekiEditor
{

/// The generic node graph editor window (Tools > Node Graph).
///
/// Lives in deki-nodegraph.dll with the node registries it uses, so changing
/// it never rebuilds the engine or the editor. Undo (CommandHistory), the
/// theme and the NodeCanvas widget come from deki-editor.dll.
///
/// Opens any .asset whose JSON "type" is a registered node graph domain
/// (NodeGraphDomainRegistry); the add-node menu offers the node types whose
/// category's first segment is the domain's. The window knows nothing about
/// any tool's node set.
class NodeGraphEditorWindow : public EditorWindow
{
public:
    const char* GetTitle() override { return "Node Graph"; }
    const char* GetMenuPath() override { return "Tools/Node Graph"; }

    NodeGraphEditorWindow();
    ~NodeGraphEditorWindow() override;

    // ---- For the command line (graph_view in NodeGraphCliTools.cpp) ----
    /// The window, or null when there is none (the tool host creates it; hot
    /// reload destroys it).
    static NodeGraphEditorWindow* Live();
    /// The open graph asset's path, or "" when none is open.
    std::string OpenAssetPath() const;
    /// Shows the graph owned by `canvasOwner` (0 = the root) on the canvas,
    /// entering every node above it, and selects `node` (0 = nothing), as
    /// double-clicking down to it and clicking it would.
    bool ShowCanvas(uint32_t canvasOwner, uint32_t node, std::string& error);

    void OnGUI() override;

    bool CanOpenAssetType(const char* assetType) override;
    void OpenFile(const char* filePath, const char* cachePath) override;

    /// Session hooks for hot reload: node instances die with the DLLs, so the
    /// state crosses the reload as plain JSON.
    bool SaveSession(std::string& outJson) override;
    void RestoreSession(const std::string& json) override;

private:
    // Document lifecycle.
    bool LoadDocument(const std::string& filePath, const std::string& cachePath, std::string& outError);
    void SaveDocument();
    void CloseDocument();

    // OnGUI pieces.
    void DrawToolbar();
    void DrawBreadcrumb();
    void DrawCanvas(float width, float height);
    void DrawPropertiesPanel(float width, float height);
    void DrawAddNodeMenu();
    void DrawModals();

    // ---- Live preview (NodeGraphPreview.h) ----
    // Drawn only for a domain that supplies preview ops. The instance belongs
    // to the domain's DLL, so it must be destroyed before that DLL unloads:
    // CloseDocument and SaveSession (run before hot reload) both do.
    //
    // Drawn over the canvas's bottom-left corner, so the effect and the nodes
    // making it are in view together. Drawn after the canvas so its controls
    // get the hover before the canvas's pan and select.
    void DrawPreviewOverlay(float canvasX, float canvasY, float canvasW, float canvasH);
    void DestroyPreview();

    // ---- Per-node gizmo (NodeGraphNodeGizmoOps) ----
    // A picture of the selected node (a shape, a ramp, a gradient) between its
    // title and its fields, for a domain that supplies gizmo ops. Takes no
    // space for a node without one, which is most of them.
    void DrawNodeGizmo(const NodeGraphDocNode& node);
    void HandleCanvasEvents(const NodeCanvasEvents& events);
    void DeleteSelection();

    // ---- Nesting (DEKI_NODE_SUBGRAPH) ----
    // The graph currently on the canvas: the root, or the inner graph of the
    // last node in m_GraphPath. Never null while a document is open.
    NodeGraphDocGraph& OpenGraph();
    const NodeGraphDocGraph& OpenGraph() const;
    // Id of the node owning the open graph (0 = root), i.e. where new nodes go.
    uint32_t OpenGraphOwner() const { return m_GraphPath.empty() ? 0u : m_GraphPath.back(); }
    // Enters a subgraph node (nothing happens for a node without one), or goes
    // back out to `depth` levels of nesting (0 = root). Both clear the
    // selection and frame the canvas on the new graph.
    void EnterSubgraph(uint32_t nodeId);
    void NavigateToDepth(size_t depth);
    // Drops trailing path entries whose node no longer exists (after undoing
    // an add, a delete while inside, or hot reload), so the canvas stays on a
    // real graph.
    void ValidateGraphPath();

    // The selected node's actions (Open its subgraph, Delete it) as a toolbar
    // strip. Returns true when the action left the node invalid, so the caller
    // must stop drawing the panel this frame.
    bool DrawNodeActionsToolbar(NodeGraphDocNode& node);

    // The node's title block: its title property as the heading with the type
    // name under it, bounded by a separator. Not collapsible, and the heading
    // is a plain label until it is clicked, which turns it into the rename
    // field. `titleProp` null (no title property) heads the block with the type
    // name and nothing is editable.
    void DrawNodeHeader(NodeGraphDocNode& node, const Deki::PropertyInfo* titleProp);

    // Property widgets. DrawPropertyControl edits a value of any reflected
    // instance (node or child): `commit` receives (old, new) JSON and pushes the
    // right command; `editKey` identifies the control for capturing the old
    // value on activate; `selfNodeId` leaves the owning node out of NodeRef
    // dropdowns.
    using CommitFn = std::function<void(const nlohmann::json&, const nlohmann::json&)>;
    void DrawPropertyControl(void* instance, const DekiNodeGraph::DekiNodeMeta& meta, const Deki::PropertyInfo& p,
                             const std::string& editKey, uint32_t selfNodeId, const CommitFn& commit);
    // A chevron button and popup listing the open scene's objects, optionally
    // only those with `componentFilter`. Call it right after its name field;
    // `onPick` receives the chosen object's name ("" = the object the graph
    // runs on), which is what the runtime looks up.
    void DrawObjectNamePicker(const char* componentFilter, const std::string& current,
                              const std::function<void(const std::string&)>& onPick);

    // A PropertyRef property: three labeled rows (object / component / field),
    // each a dropdown over what actually exists, so an invalid reference cannot
    // be authored. Commits the whole reference as one undo step.
    void DrawPropertyRefControl(const Deki::PropertyInfo& p, void* instance, const std::string& label,
                                const std::string& editKey, const CommitFn& commit);

    // A DEKI_VALUE_OF String property: the value written to, or compared with,
    // whatever its PropertyRef points at. Drawn to suit that field (drag for
    // numbers, checkbox for bool, dropdown for enums) and stored as text. A
    // plain text field while nothing is picked.
    void DrawTypedLiteralControl(const Deki::PropertyInfo& p, void* instance, const DekiNodeGraph::DekiNodeMeta& meta,
                                 const std::string& editKey, const CommitFn& commit);
    void DrawPropertyWidget(NodeGraphDocNode& node, const Deki::PropertyInfo& p);
    void DrawWeightsWidget(NodeGraphDocNode& node, const Deki::PropertyInfo& p);
    // A dynamic-outputs String array (FSM transition events, say): rows rename
    // in place; add and remove change the pins (ResizeDynamicOutputsCommand
    // removes their links).
    void DrawTransitionsWidget(NodeGraphDocNode& node, const Deki::PropertyInfo& p);
    // Ordered child stack (DEKI_NODE_CHILDREN): PlayMaker-style action list.
    void DrawChildStack(NodeGraphDocNode& node);

    // The canvas title: the meta->titleProperty value when set and not empty,
    // else the type's display name.
    std::string NodeTitle(const NodeGraphDocNode& node) const;

    // One variable declared by this document (DEKI_NODE_VARIABLES): the child's
    // title property is the name, its other exported property gives the type.
    struct GraphVariable
    {
        std::string name;
        Deki::PropertyType type = Deki::PropertyType::Float;
    };
    // Every variable the open document declares, in stack order. Empty when
    // the domain has no variables node or none are declared.
    std::vector<GraphVariable> CollectGraphVariables() const;

    // Makes sure every permanent node type of the domain exists (fixed
    // lifecycle nodes like an FSM's Awake/Start/Update). Runs after load and
    // restore; adds missing ones and marks the document dirty.
    void EnsurePermanentNodes();

    std::shared_ptr<NodeGraphDocument> m_Doc;
    NodeCanvas m_Canvas;

    // Canvas size the pan was set for, so a resize can adjust it (see
    // DrawCanvas). 0 before the first draw.
    float m_CanvasPannedForW = 0.0f;
    float m_CanvasPannedForH = 0.0f;

    // Selection, by id or index, never by pointer. A link index is into the
    // open graph's links, so it is valid only for the graph on the canvas;
    // navigating clears it.
    uint32_t m_SelectedNode = 0;
    int m_SelectedLink = -1;

    // Node ids from the root down to the open graph's owner; empty at the
    // root. Kept across hot reload in the session JSON.
    std::vector<uint32_t> m_GraphPath;

    // Add-node context menu (OpenPopup waits for the parent ID scope).
    bool m_AddMenuPending = false;
    float m_AddMenuGraphX = 0.0f;
    float m_AddMenuGraphY = 0.0f;

    // The property being edited and its value when the widget activated.
    std::string m_EditingProperty;
    nlohmann::json m_EditingOldValue;

    // Rename in the title block: the node whose heading is a text field (0 =
    // none), and whether that field still needs keyboard focus (it appears
    // the frame after the click).
    uint32_t m_RenamingNode = 0;
    bool m_RenameFocusPending = false;

    // The child-stack add popup is the editor's shared picker; this tells it
    // to clear the query and take keyboard focus on the frame it opens.
    bool m_AddChildJustOpened = true;

    // Object-name picker search box (one picker is open at a time).
    char m_ObjectPickerSearch[128] = {};

    // Live preview. m_Preview is opaque: the domain's ops create and destroy
    // it, and this window never looks inside.
    void* m_Preview = nullptr;
    const NodeGraphPreviewOps* m_PreviewOps = nullptr;  // the ops that created it
    bool m_PreviewPlaying = true;
    bool m_PreviewCollapsed = false;
    float m_PreviewZoom = 48.0f;  // preview pixels per world meter

    // Unsaved-changes confirmation: what to do after Save/Discard.
    enum class ConfirmAction
    {
        None,
        CloseWindow,
        OpenPending
    };
    ConfirmAction m_ConfirmAction = ConfirmAction::None;
    bool m_ConfirmPopupPending = false;
    // Set by Save/Discard when the confirmation was for closing the window.
    bool m_CloseAfterConfirm = false;

    // The document as on disk (ToJson().dump()); "" when it must be saved
    // anyway (a migrated asset). Unsaved means different from this, checked
    // whenever the undo history moves, so undoing back to the saved state
    // clears the mark.
    std::string m_SavedSnapshot;
    uint64_t m_SnapshotGeneration = 0;
    void TakeSavedSnapshot();
    void RefreshDirtyFromSnapshot();

    // A file waiting to open until the unsaved-changes modal is answered, and
    // the error modal.
    std::string m_PendingOpenPath;
    std::string m_PendingOpenCache;
    std::string m_ErrorText;
    bool m_ErrorPopupPending = false;
};

}  // namespace DekiEditor
