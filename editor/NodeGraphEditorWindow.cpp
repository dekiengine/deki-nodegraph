#include "deki-nodegraph/editor/NodeGraphEditorWindow.h"

#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/CommandHistory.h>
#include "deki-nodegraph/editor/NodeGraphCommands.h"
#include "deki-nodegraph/editor/NodePropertyJson.h"
#include <deki-editor/EditorNaming.h>  // shared property-name nicifier

#include <deki-editor/EditorUI.h>
#include <deki-editor/EditorApplication.h>

#include "deki-nodegraph/DekiNode.h"
#include <deki/reflection/NodeRef.h>
#include <deki/reflection/PropertyRef.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Component.h>
#include <deki/Object.h>
#include <deki/LogSystem.h>
#include <deki/Scene.h>

// For ImGui types only (the tree-node flags SchematicCollapsingHeader takes).
// This package must never call ImGui: a package DLL links its own copy, whose
// context pointer is null unless the package exports DekiPluginSetImGuiContext,
// so the first call would dereference null. Everything goes through EditorUI
// and EditorTheme, which run inside deki-editor.dll where the context lives.
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace DekiEditor
{

namespace
{
// A palette colour packed for EditorUI's style and draw calls, with an
// optional alpha scale for tints.
uint32_t PackPalette(const ImVec4& c, float alphaScale = 1.0f)
{
    auto ch = [](float v) { return static_cast<uint8_t>(v * 255.0f + 0.5f); };
    return EditorUI::Rgba(ch(c.x), ch(c.y), ch(c.z), ch(c.w * alphaScale));
}

// Enums are stored in 1, 2 or 4 bytes (Deki::PropertyInfo::enumSize).
int ReadEnumIndex(const void* field, uint8_t enumSize)
{
    switch (enumSize)
    {
        case 1:
        {
            uint8_t v;
            std::memcpy(&v, field, 1);
            return v;
        }
        case 2:
        {
            uint16_t v;
            std::memcpy(&v, field, 2);
            return v;
        }
        default:
        {
            uint32_t v;
            std::memcpy(&v, field, 4);
            return static_cast<int>(v);
        }
    }
}

// "HeroBehavior/Logic" -> domain "HeroBehavior", group "Logic".
bool SplitCategory(const char* category, std::string& outDomain, std::string& outGroup)
{
    if (!category)
    {
        return false;
    }
    const char* slash = std::strchr(category, '/');
    if (slash)
    {
        outDomain.assign(category, slash - category);
        outGroup.assign(slash + 1);
    }
    else
    {
        outDomain = category;
        outGroup.clear();
    }
    return !outDomain.empty();
}

// Categories whose node types exist only inside another node, so they are
// never offered on the root canvas or added there as permanent nodes:
//   - a child stack's category (DEKI_NODE_CHILDREN), edited in an inspector,
//   - a subgraph category that differs from its owner's own category
//     (DEKI_NODE_SUBGRAPH). It must differ: a group holding its own kind of
//     node (states containing states) must not hide that category from the
//     root.
std::vector<std::string> InnerOnlyCategories()
{
    std::vector<std::string> result;
    for (const DekiNodeMeta* meta : NodeTypeRegistry::Instance().GetAllNodes())
    {
        if (meta->childCategory && meta->childCategory[0] != '\0')
        {
            result.push_back(meta->childCategory);
        }
        if (meta->subgraphCategory && meta->subgraphCategory[0] != '\0' &&
            std::strcmp(meta->subgraphCategory, meta->category) != 0)
        {
            result.push_back(meta->subgraphCategory);
        }
    }
    return result;
}

bool Contains(const std::vector<std::string>& list, const char* value)
{
    return value && std::find(list.begin(), list.end(), value) != list.end();
}

// True if this type is another type's declared subgraph entry (a state's
// action Entry, a group's Group In). Such a node is permanent and cannot be
// deleted, but it belongs inside its owner: each subgraph gets one, and the
// root never does.
bool IsSubgraphEntryType(const DekiNodeMeta* meta)
{
    for (const DekiNodeMeta* other : NodeTypeRegistry::Instance().GetAllNodes())
    {
        if (other->subgraphEntry && other->subgraphEntry[0] != '\0' &&
            std::strcmp(other->subgraphEntry, meta->name) == 0)
        {
            return true;
        }
    }
    return false;
}

// One property's current value as JSON, for undo. Works for any reflected
// instance: top-level nodes and child stack entries alike.
nlohmann::json PropertyValueJsonOf(const void* instance, const DekiNodeMeta& meta, const Deki::PropertyInfo& p)
{
    nlohmann::json all;
    if (NodePropertiesToJson(instance, meta, all) && all.contains(p.name))
    {
        return all[p.name];
    }
    return nlohmann::json();
}

nlohmann::json PropertyValueJson(const NodeGraphDocNode& node, const Deki::PropertyInfo& p)
{
    return PropertyValueJsonOf(node.instance, *node.meta, p);
}

// The meta's property with this name, or nullptr.
const Deki::PropertyInfo* FindProperty(const DekiNodeMeta& meta, const char* name)
{
    if (!name)
    {
        return nullptr;
    }
    for (int i = 0; i < meta.propertyCount; ++i)
    {
        if (std::strcmp(meta.properties[i].name, name) == 0)
        {
            return &meta.properties[i];
        }
    }
    return nullptr;
}

// A node type's label: its display name (kStaticNodeDisplayName), else its
// name turned from camelCase/snake_case into Title Case.
std::string NodeDisplayName(const DekiNodeMeta* meta)
{
    if (meta->displayName && meta->displayName[0] != '\0')
    {
        return meta->displayName;
    }
    return EditorNaming::NicifyName(meta->name);
}

// ImGui asserts at End() when a window's last act was moving the cursor
// without submitting an item, which is how EndToolbar and the rename field's
// cursor restore end. Any item clears that, but every item also adds an
// ItemSpacing.y, so this one goes at the bottom of the panel, where the gap
// does no harm.
void CloseSetCursor(EditorUI& ui)
{
    ui.Dummy(0.0f, 0.0f);
}

// The property a node's canvas title comes from, when the panel can edit it
// as text. Null for a type without a title property; the panel then heads
// the section with the type name alone.
const Deki::PropertyInfo* TitleProperty(const NodeGraphDocNode& node)
{
    if (!node.meta || !node.instance || !node.meta->titleProperty)
    {
        return nullptr;
    }
    const Deki::PropertyInfo* p = FindProperty(*node.meta, node.meta->titleProperty);
    return (p && p->type == Deki::PropertyType::String) ? p : nullptr;
}

// The label of one output pin: the dynamic-outputs string array's value when
// the type has one (an FSM transition's event name), else the type's fixed
// pin label, else the 1-based index. The canvas uses the same rule, so a link
// reads the same in the inspector as on screen.
std::string OutputPinLabel(const NodeGraphDocNode& node, int pin)
{
    if (!node.meta || pin < 0)
    {
        return {};
    }
    if (node.meta->dynamicOutputsProperty)
    {
        const Deki::PropertyInfo* p = FindProperty(*node.meta, node.meta->dynamicOutputsProperty);
        if (p && p->type == Deki::PropertyType::Array && p->elementType == Deki::PropertyType::String)
        {
            const auto* names =
                reinterpret_cast<const std::vector<std::string>*>(static_cast<const char*>(node.instance) + p->offset);
            if (pin < static_cast<int>(names->size()) && !(*names)[pin].empty())
            {
                return (*names)[pin];
            }
        }
    }
    else if (node.meta->outputPins && pin < node.meta->outputPinCount)
    {
        return node.meta->outputPins[pin];
    }
    return std::to_string(pin + 1);
}

// --- Object-name picker helpers -------------------------------------
// Case-insensitive substring match; an empty needle matches everything.
bool MatchesSearch(const std::string& text, const char* search)
{
    if (!search || search[0] == '\0')
    {
        return true;
    }
    std::string t = text, s = search;
    for (char& c : t)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (char& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return t.find(s) != std::string::npos;
}

// "Root/Parent/" for a child of Parent; empty for a root object.
std::string HierarchyPath(Deki::Object* obj)
{
    std::vector<std::string> parts;
    for (Deki::Object* cur = obj->GetParent(); cur; cur = cur->GetParent())
    {
        parts.push_back(cur->GetName());
    }
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
    {
        path += *it + "/";
    }
    return path;
}

// True when the object has a `typeName` component or a subclass of it
// (following the registered base chain, like the inspector's ObjectRef filter).
bool ObjectHasComponent(Deki::Object* obj, const char* typeName)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    for (Deki::Component* c : obj->GetComponents())
    {
        for (uint32_t typeId = c->GetType(); typeId != 0;)
        {
            const Deki::ComponentMeta* meta = registry.GetMeta(typeId);
            if (!meta)
            {
                break;
            }
            if (meta->name && std::strcmp(meta->name, typeName) == 0)
            {
                return true;
            }
            typeId = meta->baseTypeId;
        }
    }
    return false;
}

// --- PropertyRef helpers ---------------------------------------------
// A reference as saved; the ids are derived, never stored.
nlohmann::json PropertyRefToJson(const Deki::PropertyRef& r)
{
    nlohmann::json j = nlohmann::json::object();
    j["object"] = r.object;
    j["component"] = r.component;
    j["field"] = r.field;
    return j;
}

// A field a value can be written into as text. Asset and object references,
// arrays and colours have no sensible text form, so the picker leaves them
// out. Vector2 is in: it is written "x, y" and drawn as two drag fields.
bool IsLiteralWritable(const Deki::PropertyInfo& p)
{
    switch (p.type)
    {
        case Deki::PropertyType::Int8:
        case Deki::PropertyType::Int16:
        case Deki::PropertyType::Int32:
        case Deki::PropertyType::Int64:
        case Deki::PropertyType::UInt8:
        case Deki::PropertyType::UInt16:
        case Deki::PropertyType::UInt32:
        case Deki::PropertyType::UInt64:
        case Deki::PropertyType::Float:
        case Deki::PropertyType::Double:
        case Deki::PropertyType::Bool:
        case Deki::PropertyType::String:
        case Deki::PropertyType::Enum:
        case Deki::PropertyType::Vector2: return true;
        default: return false;
    }
}

// A component's own properties, then its base classes', so inherited fields
// (RendererComponent::sorting_order and such) are offered too. The device's
// field table is flattened the same way.
template <typename Fn>
void ForEachComponentField(const Deki::ComponentMeta* meta, Fn&& fn)
{
    auto& registry = Deki::ComponentRegistry::Instance();
    for (const Deki::ComponentMeta* m = meta; m;)
    {
        for (int i = 0; i < m->propertyCount; ++i)
        {
            fn(m->properties[i]);
        }
        if (m->baseTypeId == 0 || m->baseTypeId == m->typeId)
        {
            break;
        }
        m = registry.GetMeta(m->baseTypeId);
    }
}

const Deki::PropertyInfo* FindComponentField(const Deki::ComponentMeta* meta, const std::string& fieldName)
{
    if (!meta || fieldName.empty())
    {
        return nullptr;
    }
    const Deki::PropertyInfo* found = nullptr;
    ForEachComponentField(meta,
                          [&](const Deki::PropertyInfo& p)
                          {
                              if (!found && p.name && fieldName == p.name)
                              {
                                  found = &p;
                              }
                          });
    return found;
}

// The component type a reference names, by the type GUID it stores.
const Deki::ComponentMeta* MetaOfRef(const Deki::PropertyRef& r)
{
    if (r.component.empty())
    {
        return nullptr;
    }
    return Deki::ComponentRegistry::Instance().GetMeta(r.component);
}

// The open scene's object with this name. An empty name means "the object
// running this graph", unknown at edit time, so nullptr.
Deki::Object* FindSceneObjectByName(const std::string& name)
{
    if (name.empty())
    {
        return nullptr;
    }
    ::Deki::Scene* scene = EditorApplication::Get().GetActiveScene();
    if (!scene)
    {
        return nullptr;
    }
    for (Deki::Object* obj : scene->GetObjects())
    {
        if (obj && obj->GetName() == name)
        {
            return obj;
        }
    }
    return nullptr;
}

uint32_t PaletteRgba(const ImVec4& c, float alphaOverride = -1.0f)
{
    const float a = alphaOverride >= 0.0f ? alphaOverride : c.w;
    return EditorUI::Rgba(static_cast<uint8_t>(c.x * 255.0f), static_cast<uint8_t>(c.y * 255.0f),
                          static_cast<uint8_t>(c.z * 255.0f), static_cast<uint8_t>(a * 255.0f));
}

// Canvas colours from the theme palette, to match the other panels.
NodeCanvasPalette ThemeCanvasPalette()
{
    NodeCanvasPalette p;
    p.background = PaletteRgba(Palette::Bg);
    p.grid = PaletteRgba(Palette::Line);
    p.nodeBody = PaletteRgba(Palette::Panel, 0.96f);
    p.nodeBorder = PaletteRgba(Palette::Line2);
    p.nodeBorderSelected = PaletteRgba(Palette::Accent);
    p.title = PaletteRgba(Palette::Fg);
    p.pinLabel = PaletteRgba(Palette::Dim);
    p.pin = PaletteRgba(Palette::FolderIcon);
    p.pinHovered = PaletteRgba(Palette::Accent);
    p.link = PaletteRgba(Palette::Dim);
    p.linkSelected = PaletteRgba(Palette::Accent);
    p.linkDrag = PaletteRgba(Palette::Accent);
    return p;
}
}  // namespace

// ============================================================================
// EditorWindow overrides
// ============================================================================

bool NodeGraphEditorWindow::CanOpenAssetType(const char* assetType)
{
    return assetType && NodeGraphDomainRegistry::Instance().Get(assetType) != nullptr;
}

void NodeGraphEditorWindow::OpenFile(const char* filePath, const char* cachePath)
{
    if (!filePath)
    {
        return;
    }

    if (m_Doc && m_Doc->dirty)
    {
        m_PendingOpenPath = filePath;
        m_PendingOpenCache = cachePath ? cachePath : "";
        m_ConfirmAction = ConfirmAction::OpenPending;
        m_ConfirmPopupPending = true;
        return;
    }

    std::string error;
    if (!LoadDocument(filePath, cachePath ? cachePath : "", error))
    {
        m_ErrorText = error;
        m_ErrorPopupPending = true;
    }
}

void NodeGraphEditorWindow::OnGUI()
{
    auto& ui = EditorUI::Get();
    bool isOpen = IsOpen();

    ui.SetNextWindowSize(900, 600, true);
    ui.SetNextWindowSizeConstraints(400, 300, FLT_MAX, FLT_MAX);

    char title[512];
    if (m_Doc)
    {
        const std::string name = std::filesystem::path(m_Doc->assetPath).filename().string();
        std::snprintf(title, sizeof(title), "Node Graph - %s%s###NodeGraph", name.c_str(), m_Doc->dirty ? "*" : "");
    }
    else
    {
        std::snprintf(title, sizeof(title), "Node Graph###NodeGraph");
    }

    if (ui.Begin(title, &isOpen, 0))
    {
        // A refused meta means node types are missing, and everything else
        // (gaps in the add menu, "unknown node type" on load) shows only the
        // symptom. So the cause is always shown at the top.
        if (NodeTypeRegistry::Instance().HasLayoutMismatch())
        {
            ui.Spacing();
            ui.TextColored(Palette::Red.x, Palette::Red.y, Palette::Red.z, Palette::Red.w,
                           "Some node types are missing: stale package build");
            ui.TextWrapped(NodeTypeRegistry::Instance().LayoutMismatchDetail().c_str());
            ui.Spacing();
            ui.Separator();
        }

        if (!m_Doc)
        {
            ui.Spacing();
            if (NodeGraphDomainRegistry::Instance().GetAll().empty())
            {
                ui.TextDisabled("No node graph domains registered.");
                ui.TextDisabled("Load a project whose plugin registers one (e.g. a Hero Behavior Graph).");
            }
            else
            {
                ui.TextDisabled("No graph open.");
                ui.TextDisabled("Double-click a node graph .asset in the Asset Browser,");
                ui.TextDisabled("or create one via right-click > Create.");
            }
        }
        else
        {
            // Undo can delete the node whose graph is on screen, so fix the
            // path before anything reads the open graph this frame.
            ValidateGraphPath();
            RefreshDirtyFromSnapshot();
            DrawToolbar();  // hosts the breadcrumb

            const float dpi = ui.GetDpiScale();
            float availW, availH;
            ui.GetContentRegionAvail(&availW, &availH);
            float panelW = 300.0f * dpi;
            // The canvas and the inspector meet at a 1px seam with no item
            // spacing, so the panel's toolbar and full-width bands start right
            // on the border, like a dock seam.
            const float seamW = 1.0f;
            float canvasW = availW - panelW;
            if (canvasW < 120.0f)
            {
                canvasW = availW * 0.5f;
            }

            if (ui.BeginChild("##ng_canvas", canvasW, 0.0f, false))
            {
                float w, h;
                ui.GetContentRegionAvail(&w, &h);
                float canvasX = 0.0f, canvasY = 0.0f;
                ui.GetCursorScreenPos(&canvasX, &canvasY);
                DrawCanvas(w, h);
                // After the canvas, so the overlay draws over the nodes and
                // its controls get the hover before the canvas's pan.
                DrawPreviewOverlay(canvasX, canvasY, w, h);
            }
            ui.EndChild();

            ui.SameLine(0.0f, 0.0f);

            float propsX = 0.0f, propsY = 0.0f;
            ui.GetCursorScreenPos(&propsX, &propsY);

            if (ui.BeginChild("##ng_props", 0.0f, 0.0f, false))
            {
                float w, h;
                ui.GetContentRegionAvail(&w, &h);
                DrawPropertiesPanel(w, h);

                // The seam, drawn last and inside the panel, as its first pixel
                // column. A child window draws over its parent, so a line drawn
                // outside is hidden; inside, every full-width section band fills
                // this column with an opaque background (ImGuiCol_Header =
                // Palette::Bg). Drawing after all content keeps it on top.
                const uint32_t colLine = EditorUI::Rgba(
                    static_cast<uint8_t>(Palette::Line2.x * 255.0f), static_cast<uint8_t>(Palette::Line2.y * 255.0f),
                    static_cast<uint8_t>(Palette::Line2.z * 255.0f), static_cast<uint8_t>(Palette::Line2.w * 255.0f));
                ui.DrawRectFilled(propsX, propsY, propsX + seamW, propsY + availH, colLine);
            }
            ui.EndChild();

            DrawAddNodeMenu();

            // Ctrl+S saves the graph while this window or a child has focus.
            if (ui.IsWindowFocused(true) && ui.IsKeyCtrl() && ui.IsKeyPressed(EditorUI::Key::S, false) && m_Doc->dirty)
            {
                SaveDocument();
            }
        }

        DrawModals();
    }
    ui.End();

    if (m_CloseAfterConfirm)
    {
        m_CloseAfterConfirm = false;
        isOpen = false;
    }

    // Closing with unsaved changes: stay open and ask first.
    if (!isOpen && m_Doc && m_Doc->dirty)
    {
        isOpen = true;
        m_ConfirmAction = ConfirmAction::CloseWindow;
        m_ConfirmPopupPending = true;
    }

    SetOpen(isOpen);
}

// ============================================================================
// Document lifecycle
// ============================================================================

bool NodeGraphEditorWindow::LoadDocument(const std::string& filePath, const std::string& cachePath,
                                         std::string& outError)
{
    std::ifstream f(filePath);
    if (!f.is_open())
    {
        outError = "Cannot open file: " + filePath;
        return false;
    }

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception& e)
    {
        outError = std::string("JSON parse error: ") + e.what();
        return false;
    }

    const std::string assetType = j.value("type", "");
    const DekiNodeGraphDomain* domain = NodeGraphDomainRegistry::Instance().Get(assetType);
    if (!domain)
    {
        outError = "'" + assetType + "' is not a registered node graph type";
        return false;
    }

    auto doc = std::make_shared<NodeGraphDocument>();
    doc->domain = domain;
    std::string error;
    if (!doc->FromJson(j, error))
    {
        outError = error;
        return false;
    }
    doc->assetPath = filePath;
    doc->cachePath = cachePath;

    m_Doc = std::move(doc);
    m_SelectedNode = 0;
    m_SelectedLink = -1;
    m_EditingProperty.clear();
    m_GraphPath.clear();  // a newly opened graph starts at the root
    EnsurePermanentNodes();
    // An asset that just gained permanent nodes stays unsaved until saved.
    if (m_Doc->dirty)
    {
        m_SavedSnapshot.clear();
    }
    else
    {
        TakeSavedSnapshot();
    }
    m_SnapshotGeneration = CommandHistory::Instance().GetGeneration();
    m_Canvas.FocusContent();
    return true;
}

void NodeGraphEditorWindow::TakeSavedSnapshot()
{
    m_SavedSnapshot = m_Doc ? m_Doc->ToJson().dump() : std::string();
}

void NodeGraphEditorWindow::RefreshDirtyFromSnapshot()
{
    if (!m_Doc || !m_Doc->dirty)
    {
        return;
    }
    const uint64_t gen = CommandHistory::Instance().GetGeneration();
    if (gen == m_SnapshotGeneration)
    {
        return;
    }
    m_SnapshotGeneration = gen;
    if (!m_SavedSnapshot.empty() && m_Doc->ToJson().dump() == m_SavedSnapshot)
    {
        m_Doc->dirty = false;
    }
}

void NodeGraphEditorWindow::EnsurePermanentNodes()
{
    if (!m_Doc)
    {
        return;
    }

    // Not undoable on purpose: this restores the domain's fixed lifecycle
    // nodes, not an edit the user made. Assets saved before a permanent type
    // existed gain it here and become unsaved.
    const std::vector<std::string> innerOnly = InnerOnlyCategories();
    float seedY = 40.0f;
    for (const DekiNodeMeta* meta : NodeTypeRegistry::Instance().GetAllNodes())
    {
        if (!meta->permanent)
        {
            continue;
        }
        std::string metaDomain, group;
        if (!SplitCategory(meta->category, metaDomain, group))
        {
            continue;
        }
        if (metaDomain != m_Doc->domain->domainKey)
        {
            continue;
        }
        if (Contains(innerOnly, meta->category) || IsSubgraphEntryType(meta))
        {
            continue;  // permanent inside a subgraph (a state's action Entry, a
                       // group's Group In): NodeGraphDocument::EnsureSubgraph
                       // adds one to each subgraph, never to the root
        }

        // Permanent nodes belong to the domain's root, not to a category: an
        // inner graph gets only the entry node its owner declares
        // (meta->subgraphEntry, see NodeGraphDocument::EnsureSubgraph).
        bool present = false;
        for (const NodeGraphDocNode& n : m_Doc->root.nodes)
        {
            if (n.meta->typeId == meta->typeId)
            {
                present = true;
                break;
            }
        }
        if (!present)
        {
            m_Doc->AddNode(meta->typeId, 40.0f, seedY, 0);
        }
        seedY += 130.0f;
    }
}

// ============================================================================
// Nesting: which graph the canvas is showing
// ============================================================================

NodeGraphDocGraph& NodeGraphEditorWindow::OpenGraph()
{
    NodeGraphDocGraph* graph = m_Doc->GraphOf(OpenGraphOwner());
    // ValidateGraphPath runs before every use, so a missing graph here is a
    // bug; falling back to the root keeps the window drawable.
    if (!graph)
    {
        DEKI_LOG_ERROR("NodeGraphEditorWindow: open graph %u vanished; returning to the root", OpenGraphOwner());
        m_GraphPath.clear();
        graph = &m_Doc->root;
    }
    return *graph;
}

const NodeGraphDocGraph& NodeGraphEditorWindow::OpenGraph() const
{
    return const_cast<NodeGraphEditorWindow*>(this)->OpenGraph();
}

void NodeGraphEditorWindow::ValidateGraphPath()
{
    if (!m_Doc)
    {
        m_GraphPath.clear();
        return;
    }
    // Cut the path at the first entry that is gone or owns no graph: undo can
    // delete the node you are inside.
    for (size_t i = 0; i < m_GraphPath.size(); ++i)
    {
        const NodeGraphDocNode* node = m_Doc->FindNode(m_GraphPath[i]);
        if (!node || !node->inner)
        {
            m_GraphPath.resize(i);
            m_SelectedNode = 0;
            m_SelectedLink = -1;
            return;
        }
    }
}

void NodeGraphEditorWindow::EnterSubgraph(uint32_t nodeId)
{
    if (!m_Doc)
    {
        return;
    }
    const NodeGraphDocNode* node = m_Doc->FindNode(nodeId);
    if (!node || !node->meta || !node->meta->subgraphCategory)
    {
        return;  // ordinary node: double-clicking it does nothing
    }

    m_Doc->EnsureSubgraph(nodeId);  // older assets may lack the inner graph
    m_GraphPath.push_back(nodeId);
    m_SelectedNode = 0;
    m_SelectedLink = -1;
    m_EditingProperty.clear();
    m_Canvas.FocusContent();
}

void NodeGraphEditorWindow::NavigateToDepth(size_t depth)
{
    if (depth >= m_GraphPath.size())
    {
        return;
    }
    m_GraphPath.resize(depth);
    m_SelectedNode = 0;
    m_SelectedLink = -1;
    m_EditingProperty.clear();
    m_Canvas.FocusContent();
}

void NodeGraphEditorWindow::SaveDocument()
{
    if (!m_Doc)
    {
        return;
    }

    std::ofstream f(m_Doc->assetPath);
    if (!f.is_open())
    {
        m_ErrorText = "Cannot write " + m_Doc->assetPath;
        m_ErrorPopupPending = true;
        return;
    }
    f << m_Doc->ToJson().dump(2) << "\n";
    if (!f.good())
    {
        m_ErrorText = "Write failed: " + m_Doc->assetPath;
        m_ErrorPopupPending = true;
        return;
    }
    m_Doc->dirty = false;
    TakeSavedSnapshot();
    // The asset pipeline's file watcher sees the change and recompiles the
    // msgpack cache through the generic data-asset path.
}

void NodeGraphEditorWindow::CloseDocument()
{
    DestroyPreview();  // before the document: the preview reads its instances
    m_Doc.reset();     // destroys the node instances
    m_SelectedNode = 0;
    m_SelectedLink = -1;
    m_EditingProperty.clear();
    m_GraphPath.clear();
}

namespace
{
NodeGraphEditorWindow* s_LiveWindow = nullptr;
}

NodeGraphEditorWindow::NodeGraphEditorWindow()
{
    s_LiveWindow = this;
}

NodeGraphEditorWindow::~NodeGraphEditorWindow()
{
    if (s_LiveWindow == this)
    {
        s_LiveWindow = nullptr;
    }
    DestroyPreview();
}

NodeGraphEditorWindow* NodeGraphEditorWindow::Live()
{
    return s_LiveWindow;
}

std::string NodeGraphEditorWindow::OpenAssetPath() const
{
    return m_Doc ? m_Doc->assetPath : std::string();
}

bool NodeGraphEditorWindow::ShowCanvas(uint32_t canvasOwner, uint32_t node, std::string& error)
{
    if (!m_Doc)
    {
        error = "the Node Graph window has no graph open";
        return false;
    }
    // The owners from the root down to `canvasOwner`.
    std::vector<uint32_t> path;
    for (uint32_t id = canvasOwner; id != 0; id = m_Doc->OwnerOf(id))
    {
        const NodeGraphDocNode* owner = m_Doc->FindNode(id);
        if (!owner || !owner->meta || !owner->meta->subgraphCategory)
        {
            error = "node " + std::to_string(id) + " does not exist or has no inside to show";
            return false;
        }
        path.insert(path.begin(), id);
    }
    NavigateToDepth(0);
    for (uint32_t id : path)
    {
        EnterSubgraph(id);
    }
    if (node != 0)
    {
        const NodeGraphDocGraph& graph = OpenGraph();
        const bool here = std::any_of(graph.nodes.begin(), graph.nodes.end(),
                                      [node](const NodeGraphDocNode& n) { return n.id == node; });
        if (!here)
        {
            error = "node " + std::to_string(node) + " is not on that canvas";
            return false;
        }
        m_SelectedNode = node;
    }
    return true;
}

void NodeGraphEditorWindow::DestroyPreview()
{
    // Destroyed through the same ops that created it: the instance belongs to
    // the domain's DLL, and after a hot reload the registry may return a
    // different domain, or none, for this asset type.
    if (m_Preview && m_PreviewOps && m_PreviewOps->destroy)
    {
        m_PreviewOps->destroy(m_Preview);
    }
    m_Preview = nullptr;
    m_PreviewOps = nullptr;
}

// ============================================================================
// Live preview
// ============================================================================

namespace
{
// The domain's drawing primitives, passed on to EditorUI. A preview provider
// lives in another package DLL and must never touch ImGui itself.
void PreviewCircleFilled(void* ctx, float cx, float cy, float r, uint32_t rgba)
{
    static_cast<EditorUI*>(ctx)->DrawCircleFilled(cx, cy, r, rgba);
}
void PreviewRectFilled(void* ctx, float x0, float y0, float x1, float y1, uint32_t rgba)
{
    static_cast<EditorUI*>(ctx)->DrawRectFilled(x0, y0, x1, y1, rgba);
}
void PreviewLine(void* ctx, float x0, float y0, float x1, float y1, uint32_t rgba, float thickness)
{
    static_cast<EditorUI*>(ctx)->DrawLine(x0, y0, x1, y1, rgba, thickness);
}
}  // namespace

void NodeGraphEditorWindow::DrawPreviewOverlay(float canvasX, float canvasY, float canvasW, float canvasH)
{
    if (!m_Doc || !m_Doc->domain)
    {
        return;
    }

    const NodeGraphPreviewOps& ops = m_Doc->domain->preview;
    if (!ops.create || !ops.destroy || !ops.tick)
    {
        return;  // this domain has no preview
    }

    auto& ui = EditorUI::Get();

    if (!m_Preview)
    {
        m_Preview = ops.create();
        m_PreviewOps = &ops;
        if (!m_Preview)
        {
            return;
        }
    }

    const float dpi = ui.GetDpiScale();
    const float pad = 8.0f * dpi;
    const float margin = 12.0f * dpi;
    const float rowH = 22.0f * dpi;

    float boxW = 260.0f * dpi;
    if (boxW > canvasW - margin * 2.0f)
    {
        boxW = canvasW - margin * 2.0f;
    }
    const float viewH = 150.0f * dpi;
    // Collapsed, only the header strip shows, so the overlay can be folded
    // away when it covers the part of the graph you are working on.
    const float boxH = m_PreviewCollapsed ? (rowH + pad * 2.0f) : (rowH + viewH + rowH + pad * 4.0f);

    if (boxW < 80.0f * dpi || boxH > canvasH)
    {
        return;  // canvas too small for it; the graph comes first
    }

    const float boxX = canvasX + margin;
    const float boxY = canvasY + canvasH - boxH - margin;

    ui.DrawRectFilled(boxX, boxY, boxX + boxW, boxY + boxH, EditorUI::Rgba(22, 22, 26, 235), 5.0f * dpi);
    ui.DrawRect(boxX, boxY, boxX + boxW, boxY + boxH, EditorUI::Rgba(255, 255, 255, 28), 1.0f, 5.0f * dpi);

    // ---- Header strip: fold, title, transport ----
    ui.SetCursorScreenPos(boxX + pad, boxY + pad);
    if (ui.SmallButton(m_PreviewCollapsed ? ICON_TI_CHEVRON_UP : ICON_TI_CHEVRON_DOWN))
    {
        m_PreviewCollapsed = !m_PreviewCollapsed;
    }
    ui.SameLine();
    ui.AlignTextToFramePadding();
    ui.TextDisabled("Preview");
    ui.SameLine();
    if (ui.SmallButton(m_PreviewPlaying ? ICON_TI_PLAYER_PAUSE : ICON_TI_PLAYER_PLAY))
    {
        m_PreviewPlaying = !m_PreviewPlaying;
    }
    ui.SameLine();
    if (ui.SmallButton(ICON_TI_REFRESH) && ops.reset)
    {
        ops.reset(m_Preview);
    }

    if (m_PreviewCollapsed)
    {
        return;  // folded: only the header
    }

    // The root graph is the one that runs. Entering a subgraph changes what
    // the canvas shows, not what the preview simulates.
    std::vector<NodeGraphPreviewNode> nodes;
    std::vector<NodeGraphPreviewLink> links;
    nodes.reserve(m_Doc->root.nodes.size());
    links.reserve(m_Doc->root.links.size());
    for (const NodeGraphDocNode& n : m_Doc->root.nodes)
    {
        if (!n.meta || !n.instance)
        {
            continue;
        }
        NodeGraphPreviewNode pn;
        pn.id = n.id;
        pn.typeId = n.meta->typeId;
        pn.instance = n.instance;
        nodes.push_back(pn);
    }
    for (const NodeGraphDocLink& l : m_Doc->root.links)
    {
        NodeGraphPreviewLink pl;
        pl.fromNode = l.fromNode;
        pl.fromPin = l.fromPin;
        pl.toNode = l.toNode;
        pl.toPin = l.toPin;
        links.push_back(pl);
    }

    NodeGraphPreviewGraph view;
    view.nodes = nodes.empty() ? nullptr : nodes.data();
    view.nodeCount = static_cast<int>(nodes.size());
    view.links = links.empty() ? nullptr : links.data();
    view.linkCount = static_cast<int>(links.size());

    // ---- Viewport ----
    const float viewX = boxX + pad;
    const float viewY = boxY + pad + rowH + pad;
    const float viewW = boxW - pad * 2.0f;

    ui.DrawRectFilled(viewX, viewY, viewX + viewW, viewY + viewH, EditorUI::Rgba(12, 12, 14, 255), 3.0f * dpi);

    NodeGraphPreviewCanvas canvas;
    canvas.ctx = &ui;
    canvas.circleFilled = &PreviewCircleFilled;
    canvas.rectFilled = &PreviewRectFilled;
    canvas.line = &PreviewLine;

    // Paused still ticks, with dt = 0, so the current state keeps drawing.
    const float dt = m_PreviewPlaying ? ui.GetDeltaTime() : 0.0f;

    ui.PushClipRect(viewX, viewY, viewX + viewW, viewY + viewH, true);
    ops.tick(m_Preview, view, dt, viewX, viewY, viewW, viewH, m_PreviewZoom * dpi, canvas);
    ui.PopClipRect();

    // ---- Zoom ----
    ui.SetCursorScreenPos(viewX, viewY + viewH + pad);
    ui.SetNextItemWidth(viewW);
    ui.SliderFloat("##ng_preview_zoom", &m_PreviewZoom, 4.0f, 256.0f, "%.0f px/m");
}

void NodeGraphEditorWindow::DrawNodeGizmo(const NodeGraphDocNode& node)
{
    if (!m_Doc || !m_Doc->domain || !node.meta || !node.instance)
    {
        return;
    }

    const NodeGraphNodeGizmoOps& ops = m_Doc->domain->gizmos;
    if (!ops.height || !ops.draw)
    {
        return;  // this domain has no gizmos
    }

    auto& ui = EditorUI::Get();
    const float dpi = ui.GetDpiScale();

    // Asked every frame: a gizmo can appear, grow or go away as values are
    // edited (a shape switched to Point has nothing to show).
    const float cssH = ops.height(node.meta->typeId, node.instance);
    if (cssH <= 0.0f)
    {
        return;
    }

    const float h = cssH * dpi;
    ui.Spacing();

    // The right edge lines up with the fields, not the window, since the band
    // is part of the form below it.
    float availW = 0.0f, availH = 0.0f;
    ui.GetContentRegionAvail(&availW, &availH);
    availW -= Metrics::kInspectorRightPad * dpi;
    if (availW < 32.0f * dpi)
    {
        return;
    }

    float x = 0.0f, y = 0.0f;
    ui.GetCursorScreenPos(&x, &y);

    // A recessed band, darker than the panel like the preview's viewport, so
    // a gizmo reads as a picture of the node, not another row of the form.
    ui.DrawRectFilled(x, y, x + availW, y + h, EditorUI::Rgba(12, 12, 14, 255), 4.0f * dpi);
    ui.DrawRect(x, y, x + availW, y + h, EditorUI::Rgba(255, 255, 255, 18), 1.0f, 4.0f * dpi);

    NodeGraphPreviewCanvas canvas;
    canvas.ctx = &ui;
    canvas.circleFilled = &PreviewCircleFilled;
    canvas.rectFilled = &PreviewRectFilled;
    canvas.line = &PreviewLine;

    // Clipped here, not by the provider, so a gizmo cannot draw over the rows
    // above and below.
    ui.PushClipRect(x, y, x + availW, y + h, true);
    ops.draw(node.meta->typeId, node.instance, x, y, availW, h, dpi, canvas);
    ui.PopClipRect();

    // The band is drawn, not laid out, so reserve its space for the fields to
    // follow.
    ui.Dummy(availW, h);
    ui.Spacing();
}

// ============================================================================
// Toolbar
// ============================================================================

void NodeGraphEditorWindow::DrawToolbar()
{
    auto& ui = EditorUI::Get();

    ui.BeginToolbar();
    if (ui.ToolbarIconButton("##ng_save", ICON_TI_DEVICE_FLOPPY, "Save graph", m_Doc->dirty))
    {
        SaveDocument();
    }

    // Navigation sits next to Save: with nested graphs, getting back out is
    // needed all the time.
    if (ui.ToolbarIconButton("##ng_up", ICON_TI_ARROW_BACK_UP, "Up one level", !m_GraphPath.empty()))
    {
        if (!m_GraphPath.empty())
        {
            NavigateToDepth(m_GraphPath.size() - 1);
        }
    }
    if (ui.ToolbarIconButton("##ng_frame", ICON_TI_FOCUS_CENTERED, "Frame all nodes"))
    {
        m_Canvas.FocusContent();
    }

    DrawBreadcrumb();

    // The strip's rect comes from the last cell (still the previous item), so
    // the status text can sit on the row's centre line like the cells' labels,
    // instead of at the top edge where the layout cursor would put it.
    float rowTop = 0.0f, rowBottom = 0.0f;
    ui.GetItemRect(nullptr, &rowTop, nullptr, &rowBottom);

    ui.SameLine();
    const NodeGraphDocGraph& graph = OpenGraph();
    char info[512];
    std::snprintf(info, sizeof(info), "%s  |  %s  |  %d nodes, %d links", m_Doc->domain->displayName,
                  std::filesystem::path(m_Doc->assetPath).filename().string().c_str(),
                  static_cast<int>(graph.nodes.size()), static_cast<int>(graph.links.size()));

    float textX = 0.0f, textY = 0.0f;
    ui.GetCursorScreenPos(&textX, &textY);
    ui.SetCursorScreenPos(textX + ui.GetDpiScale() * 6.0f, rowTop + (rowBottom - rowTop - ui.GetFontSize()) * 0.5f);
    ui.TextDisabled(info);
    ui.EndToolbar();
}

// The path back out of nested graphs, as toolbar cells: "Graph / Patrol /
// Attack", each ancestor clickable and the current level marked active. Drawn
// in the toolbar, only while inside a subgraph.
void NodeGraphEditorWindow::DrawBreadcrumb()
{
    if (m_GraphPath.empty())
    {
        return;
    }

    auto& ui = EditorUI::Get();

    if (ui.ToolbarButton("Graph"))
    {
        NavigateToDepth(0);
        return;  // m_GraphPath just changed; the rest of the trail is gone
    }

    for (size_t i = 0; i < m_GraphPath.size(); ++i)
    {
        const NodeGraphDocNode* node = m_Doc->FindNode(m_GraphPath[i]);
        if (!node)
        {
            break;
        }

        const bool isCurrent = (i + 1 == m_GraphPath.size());
        ui.PushID(static_cast<int>(i));
        const bool clicked = ui.ToolbarButton(NodeTitle(*node).c_str(), true, isCurrent);
        ui.PopID();
        // Clicking the current level does nothing.
        if (clicked && !isCurrent)
        {
            NavigateToDepth(i + 1);
            break;
        }
    }
}

// ============================================================================
// Canvas
// ============================================================================

void NodeGraphEditorWindow::DrawCanvas(float width, float height)
{
    auto& doc = *m_Doc;
    const NodeGraphDocGraph& graph = OpenGraph();
    const size_t nodeCount = graph.nodes.size();

    // Keep the centre of the view still when the canvas resizes, as the Scene
    // view does. The pan is an offset from the canvas's top-left, so it moves
    // by half the size change. Skipped on the first draw, when there is no
    // previous size; a pending frame-all sets the pan outright anyway.
    if (width > 0.0f && height > 0.0f && m_CanvasPannedForW > 0.0f && m_CanvasPannedForH > 0.0f &&
        (width != m_CanvasPannedForW || height != m_CanvasPannedForH))
    {
        m_Canvas.SetView(m_Canvas.GetPanX() + (width - m_CanvasPannedForW) * 0.5f,
                         m_Canvas.GetPanY() + (height - m_CanvasPannedForH) * 0.5f, m_Canvas.GetZoom());
    }
    if (width > 0.0f && height > 0.0f)
    {
        m_CanvasPannedForW = width;
        m_CanvasPannedForH = height;
    }

    m_Canvas.SetPalette(ThemeCanvasPalette());

    // Strings the canvas borrows during Draw, kept for this frame: titles and
    // dynamic output pin labels ("1", "2", ...).
    std::vector<std::string> titles(nodeCount);
    std::vector<std::vector<std::string>> dynLabelText(nodeCount);
    std::vector<std::vector<const char*>> dynLabelPtrs(nodeCount);

    std::vector<NodeCanvasNode> cnodes;
    cnodes.reserve(nodeCount);
    for (size_t i = 0; i < nodeCount; ++i)
    {
        const NodeGraphDocNode& n = graph.nodes[i];
        titles[i] = NodeTitle(n);
        NodeCanvasNode cn;
        cn.id = n.id;
        cn.x = n.x;
        cn.y = n.y;
        cn.title = titles[i].c_str();
        cn.inputLabels = n.meta->inputPins;
        cn.inputCount = n.meta->inputPinCount;
        cn.selected = n.id == m_SelectedNode;
        cn.hasSubgraph = n.meta->subgraphCategory != nullptr;

        const int outCount = doc.OutputPinCount(n);
        if (n.meta->dynamicOutputsProperty)
        {
            // A string-array of dynamic outputs labels pins with its values (FSM
            // transition event names); other kinds use 1-based indices.
            const Deki::PropertyInfo* dynProp = FindProperty(*n.meta, n.meta->dynamicOutputsProperty);
            const std::vector<std::string>* names = nullptr;
            if (dynProp && dynProp->type == Deki::PropertyType::Array &&
                dynProp->elementType == Deki::PropertyType::String)
            {
                names = reinterpret_cast<const std::vector<std::string>*>(static_cast<const char*>(n.instance) +
                                                                          dynProp->offset);
            }

            auto& text = dynLabelText[i];
            auto& ptrs = dynLabelPtrs[i];
            text.reserve(outCount);
            ptrs.reserve(outCount);
            for (int k = 0; k < outCount; ++k)
            {
                if (names && k < static_cast<int>(names->size()) && !(*names)[k].empty())
                {
                    text.push_back((*names)[k]);
                }
                else
                {
                    text.push_back(std::to_string(k + 1));
                }
                ptrs.push_back(text.back().c_str());
            }
            cn.outputLabels = ptrs.data();
            cn.outputCount = outCount;
        }
        else
        {
            cn.outputLabels = n.meta->outputPins;
            cn.outputCount = n.meta->outputPinCount;
        }
        cnodes.push_back(cn);
    }

    std::vector<NodeCanvasLink> clinks;
    clinks.reserve(graph.links.size());
    for (size_t i = 0; i < graph.links.size(); ++i)
    {
        const NodeGraphDocLink& l = graph.links[i];
        NodeCanvasLink cl;
        cl.fromNode = l.fromNode;
        cl.fromPin = l.fromPin;
        cl.toNode = l.toNode;
        cl.toPin = l.toPin;
        cl.selected = static_cast<int>(i) == m_SelectedLink;
        clinks.push_back(cl);
    }

    NodeCanvasEvents events;
    m_Canvas.Draw("##nodecanvas", width, height, cnodes.data(), static_cast<int>(cnodes.size()), clinks.data(),
                  static_cast<int>(clinks.size()), events);

    HandleCanvasEvents(events);
}

void NodeGraphEditorWindow::HandleCanvasEvents(const NodeCanvasEvents& events)
{
    auto& doc = *m_Doc;

    if (events.nodeClicked)
    {
        m_SelectedNode = events.node;
        m_SelectedLink = -1;
    }
    if (events.linkClicked)
    {
        m_SelectedLink = events.linkIndex;
        m_SelectedNode = 0;
    }
    if (events.backgroundClicked)
    {
        m_SelectedNode = 0;
        m_SelectedLink = -1;
    }
    if (events.nodeActivated)
    {
        // Double-click enters a subgraph node; for any other node
        // EnterSubgraph does nothing.
        EnterSubgraph(events.activatedNode);
        return;  // the open graph changed: this frame's other events are stale
    }

    if (events.nodeMoving)
    {
        // During the drag positions are written directly; the undo command is
        // pushed once on release.
        if (NodeGraphDocNode* node = doc.FindNode(events.movedNode))
        {
            node->x = events.newX;
            node->y = events.newY;
        }
    }

    if (events.nodeMoveEnded)
    {
        if (const NodeGraphDocNode* node = doc.FindNode(events.moveEndedNode))
        {
            if (node->x != events.startX || node->y != events.startY)
            {
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<MoveNodeCommand>(m_Doc, node->id, events.startX, events.startY, node->x, node->y));
            }
        }
    }

    if (events.linkCreated)
    {
        const NodeGraphDocNode* from = doc.FindNode(events.fromNode);
        const NodeGraphDocNode* to = doc.FindNode(events.toNode);
        const bool valid = from && to && events.fromPin >= 0 && events.fromPin < doc.OutputPinCount(*from) &&
                           events.toPin >= 0 && events.toPin < to->meta->inputPinCount;
        if (valid)
        {
            NodeGraphDocLink link{ events.fromNode, events.fromPin, events.toNode, events.toPin };
            const NodeGraphDocLink* existing = doc.FindLinkFromPin(link.fromNode, link.fromPin);
            if (!existing || !(*existing == link))
            {
                CommandHistory::Instance().ExecuteNoMerge(std::make_unique<AddLinkCommand>(m_Doc, link));
            }
        }
    }

    if (events.contextMenu)
    {
        // OpenPopup must run in the same ID scope as BeginPopup. This handler
        // runs inside the canvas child and DrawAddNodeMenu in the parent
        // window, so a flag passes it on.
        m_AddMenuGraphX = events.graphX;
        m_AddMenuGraphY = events.graphY;
        m_AddMenuPending = true;
    }

    if (events.deleteRequested)
    {
        DeleteSelection();
    }
}

void NodeGraphEditorWindow::DeleteSelection()
{
    auto& doc = *m_Doc;
    if (m_SelectedNode != 0)
    {
        // Permanent lifecycle nodes (Awake/Start/Update and such) are part of
        // every graph: the Delete key and Delete button both end up here, and
        // do nothing to them.
        if (const NodeGraphDocNode* node = doc.FindNode(m_SelectedNode))
        {
            if (node->meta->permanent)
            {
                return;
            }
        }
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<DeleteNodeCommand>(m_Doc, m_SelectedNode));
        m_SelectedNode = 0;
    }
    else if (m_SelectedLink >= 0 && m_SelectedLink < static_cast<int>(OpenGraph().links.size()))
    {
        CommandHistory::Instance().ExecuteNoMerge(
            std::make_unique<RemoveLinkCommand>(m_Doc, OpenGraph().links[m_SelectedLink]));
        m_SelectedLink = -1;
    }
}

// ============================================================================
// Add-node context menu
// ============================================================================

void NodeGraphEditorWindow::DrawAddNodeMenu()
{
    auto& ui = EditorUI::Get();

    if (m_AddMenuPending)
    {
        m_AddMenuPending = false;
        ui.OpenPopup("##ng_addnode");
    }

    PushContextMenuPopupStyle();
    if (ui.BeginPopup("##ng_addnode"))
    {
        PushContextMenuItemStyle();

        // The node types this graph level accepts: inside a subgraph, the
        // owner's declared content category; at the root, the domain's types
        // except those that exist only inside a parent.
        const std::vector<std::string> innerOnly = InnerOnlyCategories();
        const char* subgraphCategory = nullptr;
        if (const uint32_t owner = OpenGraphOwner())
        {
            if (const NodeGraphDocNode* ownerNode = m_Doc->FindNode(owner))
            {
                subgraphCategory = ownerNode->meta->subgraphCategory;
            }
        }

        // Node types of this domain, grouped by category remainder, in
        // registration order.
        std::vector<std::pair<std::string, std::vector<const DekiNodeMeta*>>> groups;
        for (const DekiNodeMeta* meta : NodeTypeRegistry::Instance().GetAllNodes())
        {
            std::string metaDomain, group;
            if (!SplitCategory(meta->category, metaDomain, group))
            {
                continue;
            }
            if (metaDomain != m_Doc->domain->domainKey)
            {
                continue;
            }
            if (subgraphCategory)
            {
                if (std::strcmp(meta->category, subgraphCategory) != 0)
                {
                    continue;  // not allowed in this subgraph
                }
            }
            else if (Contains(innerOnly, meta->category))
            {
                continue;  // exists only inside a parent
            }
            if (meta->permanent)
            {
                continue;  // fixed lifecycle node: always present, never added
            }

            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == group; });
            if (it == groups.end())
            {
                groups.push_back({ group, {} });
                it = groups.end() - 1;
            }
            it->second.push_back(meta);
        }

        if (groups.empty())
        {
            ui.TextDisabled("No node types registered for this graph");
        }

        for (const auto& [group, metas] : groups)
        {
            // The popup and item style come from the EditorTheme pushes above;
            // the submenu is a plain nested menu.
            const bool useSubmenu = !group.empty();
            if (!useSubmenu || ui.BeginMenu(group.c_str()))
            {
                for (const DekiNodeMeta* meta : metas)
                {
                    if (ui.MenuItem(NodeDisplayName(meta).c_str(), true))
                    {
                        auto cmd = std::make_unique<AddNodeCommand>(m_Doc, meta->typeId, meta->name, m_AddMenuGraphX,
                                                                    m_AddMenuGraphY, OpenGraphOwner());
                        AddNodeCommand* raw = cmd.get();
                        CommandHistory::Instance().ExecuteNoMerge(std::move(cmd));
                        m_SelectedNode = raw->GetNodeId();
                        m_SelectedLink = -1;
                    }
                }
                if (useSubmenu)
                {
                    ui.EndMenu();
                }
            }
        }

        ui.PopStyleVar();  // ItemSpacing (PushContextMenuItemStyle)
        ui.EndPopup();
    }
    ui.PopStyleVar();  // WindowPadding (PushContextMenuPopupStyle)
}

// ============================================================================
// Properties panel
// ============================================================================

void NodeGraphEditorWindow::DrawPropertiesPanel(float /*width*/, float /*height*/)
{
    auto& ui = EditorUI::Get();
    auto& doc = *m_Doc;

    // A rename ends when the selection moves: deselecting, selecting another
    // node, entering a subgraph or deleting the node. Checked here, not in the
    // header, because most of those paths never draw the header again, and
    // the rename would still be open when the node came back.
    if (m_RenamingNode != m_SelectedNode)
    {
        m_RenamingNode = 0;
    }

    // Rows use the shared property context (BeginPropertyContext /
    // EndPropertyContext), the Inspector's gutter, row gap and field padding,
    // so they look like Inspector rows. Toolbars and section bands still span
    // the full width; only their labels follow the indent.

    if (m_SelectedNode != 0)
    {
        NodeGraphDocNode* node = doc.FindNode(m_SelectedNode);
        if (!node)
        {
            m_SelectedNode = 0;
            return;
        }

        // Actions first, in the toolbar strip other panels use, fixed at the
        // top so they do not move as the property list grows.
        if (DrawNodeActionsToolbar(*node))
        {
            CloseSetCursor(ui);  // the panel ends here, on the toolbar's cursor
            return;              // entered its subgraph or deleted it
        }
        BeginPropertyContext();

        // The title block shows the node's name (click it to rename) with the
        // type under it, so a node is renamed where its name is shown, not in
        // a "Name" row among its settings.
        const Deki::PropertyInfo* titleProp = TitleProperty(*node);
        DrawNodeHeader(*node, titleProp);

        // The node's picture goes above its fields, so the drawing stays on
        // screen while a radius or a ramp row is dragged.
        DrawNodeGizmo(*node);

        for (int i = 0; i < node->meta->propertyCount; ++i)
        {
            const Deki::PropertyInfo& p = node->meta->properties[i];
            if (&p == titleProp)
            {
                continue;  // shown in the title block
            }
            const bool isDynamicOutputs = node->meta->dynamicOutputsProperty &&
                                          std::strcmp(p.name, node->meta->dynamicOutputsProperty) == 0 &&
                                          p.type == Deki::PropertyType::Array;
            ui.PushID(p.name);
            if (isDynamicOutputs && p.elementType == Deki::PropertyType::Float)
            {
                DrawWeightsWidget(*node, p);
            }
            else if (isDynamicOutputs && p.elementType == Deki::PropertyType::String)
            {
                DrawTransitionsWidget(*node, p);
            }
            else
            {
                DrawPropertyWidget(*node, p);
            }
            ui.PopID();
        }

        if (node->meta->childCategory && node->meta->childCategory[0] != '\0')
        {
            DrawChildStack(*node);
        }

        // A node with only a name (a Group Exit, say) draws no rows, so the
        // header's cursor restore would be the panel's last act.
        CloseSetCursor(ui);
        EndPropertyContext();
    }
    else if (m_SelectedLink >= 0 && m_SelectedLink < static_cast<int>(OpenGraph().links.size()))
    {
        // The same strip a node gets, so the actions are in one place
        // whatever is selected.
        bool deleteLink = false;
        ui.BeginToolbar();
        deleteLink = ui.ToolbarButton(ICON_TI_TRASH "  Delete Link");
        ui.EndToolbar();
        if (deleteLink)
        {
            DeleteSelection();
            CloseSetCursor(ui);  // the panel ends here, on the toolbar's cursor
            return;
        }
        BeginPropertyContext();

        const NodeGraphDocLink& link = OpenGraph().links[m_SelectedLink];
        if (SchematicSectionBegin("Link"))
        {
            // The ends by name, as they read on the canvas, rather than node
            // and pin numbers.
            const NodeGraphDocNode* from = doc.FindNode(link.fromNode);
            const NodeGraphDocNode* to = doc.FindNode(link.toNode);
            const std::string fromName = from ? NodeTitle(*from) : "(missing)";
            const std::string toName = to ? NodeTitle(*to) : "(missing)";
            const std::string pinName = from ? OutputPinLabel(*from, link.fromPin) : std::string();

            char line[320];
            std::snprintf(line, sizeof(line), "%s  " ICON_TI_CHEVRON_RIGHT "  %s", fromName.c_str(), toName.c_str());
            ui.Text(line);
            if (!pinName.empty())
            {
                std::snprintf(line, sizeof(line), "on %s", pinName.c_str());
                ui.TextDisabled(line);
            }
            SchematicSectionEnd();
        }

        EndPropertyContext();
    }
    else
    {
        BeginPropertyContext();
        ui.Spacing();
        ui.TextDisabled("Nothing selected.");
        ui.Spacing();
        ui.TextDisabled(ICON_TI_PLUS "   Right-click the canvas to add nodes");
        ui.TextDisabled(ICON_TI_LINK "   Drag an output pin onto an input pin");
        if (const uint32_t owner = OpenGraphOwner())
        {
            if (const NodeGraphDocNode* ownerNode = m_Doc->FindNode(owner))
            {
                ui.Spacing();
                char hint[192];
                std::snprintf(hint, sizeof(hint), ICON_TI_ARROW_BACK_UP "   Inside %s", NodeTitle(*ownerNode).c_str());
                ui.TextDisabled(hint);
            }
        }
        EndPropertyContext();
    }
}

bool NodeGraphEditorWindow::DrawNodeActionsToolbar(NodeGraphDocNode& node)
{
    auto& ui = EditorUI::Get();

    // Clicks are acted on after EndToolbar, since both actions invalidate the
    // node the strip was built from.
    bool open = false;
    bool del = false;

    ui.BeginToolbar();
    if (node.meta->subgraphCategory)
    {
        // A subgraph node's contents are edited on the canvas, not here. The
        // tooltip gives the node count, so an empty one shows without opening it.
        const int inner = node.inner ? static_cast<int>(node.inner->nodes.size()) : 0;
        char tip[96];
        std::snprintf(tip, sizeof(tip), "Open this node's graph (%d node%s inside)", inner, inner == 1 ? "" : "s");
        open = ui.ToolbarIconButton("##ng_opennode", ICON_TI_BOX_MULTIPLE, tip);
    }
    if (node.meta->permanent)
    {
        ui.ToolbarButton(ICON_TI_LOCK "  Permanent", false);
    }
    else
    {
        del =
            ui.ToolbarIconButton("##ng_delnode", ICON_TI_TRASH, "Delete this node", true, false, /*destructive*/ true);
    }
    ui.EndToolbar();

    if (open)
    {
        EnterSubgraph(node.id);
        return true;
    }
    if (del)
    {
        DeleteSelection();
        return true;
    }
    return false;
}

void NodeGraphEditorWindow::DrawNodeHeader(NodeGraphDocNode& node, const Deki::PropertyInfo* titleProp)
{
    auto& ui = EditorUI::Get();
    const float dpi = ui.GetDpiScale();
    const std::string typeName = NodeDisplayName(node.meta);

    // The same band the sections below use, without the fold: a panel header
    // is not collapsed.
    //
    // The name is the band's label until it is clicked, and only then a text
    // field, so the header does not read as a form.

    // (A rename belongs to the selected node; DrawPropertiesPanel ends it when
    // the selection moves.)
    const bool renaming = titleProp && m_RenamingNode == node.id;

    // Where the band's label starts and how much room it has, captured before
    // the band because the rename field takes that slot. The band reports where
    // it puts its label: one step left of the content, so the content reads as
    // nested.
    float contentX = 0.0f;
    ui.GetCursorScreenPos(&contentX, nullptr);
    float bandAvailW = 0.0f;
    ui.GetContentRegionAvail(&bandAvailW, nullptr);
    const float contentRight = contentX + bandAvailW - Metrics::kInspectorRightPad * dpi;

    // No spacing after the band: ImGui adds ItemSpacing.y after each item,
    // which would hold the header off the separator below. Nothing above needs
    // it: the toolbar leaves the cursor on its own line, and Indent only moves
    // x.
    float spacingX = 0.0f;
    ui.GetItemSpacing(&spacingX, nullptr);
    ui.PushStyleVar(EditorUI::StyleVar::ItemSpacing, spacingX, 0.0f);

    // An id-only label draws the band empty, leaving the row for the field.
    const std::string bandLabel =
        renaming ? std::string("###ng_nodeband") : (titleProp ? NodeTitle(node) : typeName) + "###ng_nodeband";
    float labelX = contentX;
    const bool bandClicked = SchematicSectionBand(bandLabel.c_str(), &labelX);

    if (renaming)
    {
        // The field sits in the band, on the label's line and at its left
        // edge, so renaming does not move the text. This moves the layout
        // cursor, which is put back below.
        float bandMinY = 0.0f, bandMaxY = 0.0f;
        ui.GetItemRect(nullptr, &bandMinY, nullptr, &bandMaxY);
        float afterBandX = 0.0f, afterBandY = 0.0f;
        ui.GetCursorScreenPos(&afterBandX, &afterBandY);

        auto* field = reinterpret_cast<std::string*>(static_cast<char*>(node.instance) + titleProp->offset);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", field->c_str());

        // Scoped by node id, so selecting another node mid-edit drops this
        // control instead of carrying the half-typed text (and its commit) to
        // the other node.
        ui.PushID(static_cast<int>(node.id));

        // Focused the frame it appears, so typing works without a second click.
        if (m_RenameFocusPending)
        {
            ui.SetKeyboardFocusHere();
            m_RenameFocusPending = false;
        }

        // The hint is the type name, which the canvas shows while the name is
        // empty. As with the other controls, the edit commits when the field
        // is left, as one undo step.
        const std::string editKey = "title:" + std::to_string(node.id);
        // Drawn as the band's title, not a framed field: same font, size,
        // colour and line. Only the caret shows that it is editable.
        const bool changed = SchematicBandTitleField("##ng_nodename", buf, static_cast<int>(sizeof(buf)), labelX,
                                                     bandMinY, bandMaxY, contentRight - labelX, typeName.c_str());
        if (ui.IsItemActive() && m_EditingProperty != editKey)
        {
            m_EditingProperty = editKey;
            m_EditingOldValue = *field;
        }
        if (changed)
        {
            *field = buf;  // live preview; the canvas title follows
        }
        if (ui.IsItemDeactivatedAfterEdit() && m_EditingProperty == editKey)
        {
            CommandHistory::Instance().ExecuteNoMerge(std::make_unique<SetNodePropertyCommand>(
                m_Doc, node.id, titleProp->name, m_EditingOldValue, nlohmann::json(*field)));
            m_EditingProperty.clear();
        }
        // Closes the field when focus goes elsewhere, edited or not (hence not
        // the ...AfterEdit variant).
        if (ui.IsItemDeactivated())
        {
            m_RenamingNode = 0;
        }

        ui.PopID();
        ui.SetCursorScreenPos(afterBandX, afterBandY);
    }
    else if (titleProp)
    {
        if (ui.IsItemHovered())
        {
            ui.SetTooltip("Click to rename");
        }
        if (bandClicked)
        {
            m_RenamingNode = node.id;
            m_RenameFocusPending = true;
        }
    }

    ui.PopStyleVar();  // ItemSpacing
}

void NodeGraphEditorWindow::DrawPropertyControl(void* instance, const DekiNodeMeta& meta, const Deki::PropertyInfo& p,
                                                const std::string& editKey, uint32_t selfNodeId, const CommitFn& commit)
{
    auto& ui = EditorUI::Get();
    void* field = static_cast<char*>(instance) + p.offset;
    // The label the component inspector would use (displayName if set, else
    // camelCase/snake_case to "Start Hour", "Min Sec", ...).
    const std::string label = EditorNaming::GetDisplayName(p);

    // The value before the edit is captured when the control activates, and
    // `commit` runs once when the edited control is left, so one drag is one
    // undo step. Changes during the edit write the instance directly. `editKey`
    // keeps same-named properties of a node and its children apart.
    const nlohmann::json preValue = PropertyValueJsonOf(instance, meta, p);
    auto captureIfActivated = [&]()
    {
        if (ui.IsItemActive() && m_EditingProperty != editKey)
        {
            m_EditingProperty = editKey;
            m_EditingOldValue = preValue;
        }
    };
    auto commitOnDeactivate = [&]()
    {
        if (ui.IsItemDeactivatedAfterEdit() && m_EditingProperty == editKey)
        {
            commit(m_EditingOldValue, PropertyValueJsonOf(instance, meta, p));
            m_EditingProperty.clear();
        }
    };

    // A PropertyRef is three rows, so it draws its own labels; every other
    // type fills the value column of one labelled row.
    if (p.type == Deki::PropertyType::PropertyRef)
    {
        DrawPropertyRefControl(p, instance, label, editKey, commit);
        return;
    }

    // A value typed by a PropertyRef also draws its own row, since its control
    // depends on what the reference points at.
    if (p.type == Deki::PropertyType::String && p.valueOfProperty)
    {
        DrawTypedLiteralControl(p, instance, meta, editKey, commit);
        return;
    }

    ui.PropertyRow(label.c_str());

    switch (p.type)
    {
        case Deki::PropertyType::Float:
        {
            float v;
            std::memcpy(&v, field, sizeof v);
            bool changed;
            if (p.hasRange && p.useSlider)
            {
                changed = ui.SliderFloat("##v", &v, p.minValue, p.maxValue, "%.3f");
            }
            else
            {
                changed =
                    ui.DragFloat("##v", &v, 0.05f, p.hasRange ? p.minValue : 0.0f, p.hasRange ? p.maxValue : 0.0f);
            }
            captureIfActivated();
            if (changed)
            {
                std::memcpy(field, &v, sizeof v);  // live preview
            }
            commitOnDeactivate();
            break;
        }
        case Deki::PropertyType::Int32:
        {
            int32_t pre;
            std::memcpy(&pre, field, sizeof pre);
            int v = pre;
            const bool changed = ui.DragInt("##v", &v, 0.2f, p.hasRange ? static_cast<int>(p.minValue) : 0,
                                            p.hasRange ? static_cast<int>(p.maxValue) : 0);
            captureIfActivated();
            if (changed)
            {
                const int32_t nv = v;
                std::memcpy(field, &nv, sizeof nv);
            }
            commitOnDeactivate();
            break;
        }
        case Deki::PropertyType::Bool:
        {
            bool v;
            std::memcpy(&v, field, sizeof v);
            if (ui.Checkbox("##v", &v))
            {
                commit(nlohmann::json(!v), nlohmann::json(v));
            }
            break;
        }
        case Deki::PropertyType::Enum:
        {
            const int current = ReadEnumIndex(field, p.enumSize);
            const char* preview = (current >= 0 && current < p.enumCount) ? p.enumValues[current] : "?";
            if (ui.BeginCombo("##v", preview))
            {
                for (int e = 0; e < p.enumCount; ++e)
                {
                    if (ui.Selectable(p.enumValues[e], e == current) && e != current)
                    {
                        commit(nlohmann::json(current), nlohmann::json(e));
                    }
                }
                ui.EndCombo();
            }
            break;
        }
        case Deki::PropertyType::String:
        {
            const std::string& current = *static_cast<std::string*>(field);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", current.c_str());

            // DEKI_OBJECT_NAME fields keep a free-text input (a graph can name
            // an object in a scene that is not open) and add a picker button
            // that fills it from the open scene's hierarchy.
            const bool isObjectName = p.componentRefType != nullptr;
            if (isObjectName)
            {
                ui.SetNextItemWidth(ui.CalcItemWidth() - ui.GetFrameHeight());
            }

            const bool changed = ui.InputText("##v", buf, sizeof(buf));
            captureIfActivated();
            if (changed)
            {
                *static_cast<std::string*>(field) = buf;  // live preview
            }
            commitOnDeactivate();

            if (isObjectName)
            {
                DrawObjectNamePicker(p.componentRefType, current, [&](const std::string& name)
                                     { commit(nlohmann::json(current), nlohmann::json(name)); });
            }
            break;
        }
        case Deki::PropertyType::AssetRef:
        {
            // The asset GUID is saved and the runtime loads it, but this window
            // cannot open the asset picker: that needs the AssetPipeline, which
            // tool windows cannot reach (the EditorApplication facade would
            // have to expose it). So the GUID is shown and edited as text.
            auto* ref = static_cast<Deki::AssetRefBase*>(field);
            char buf[80];
            std::snprintf(buf, sizeof(buf), "%s", ref->guid.c_str());
            const bool changed = ui.InputTextWithHint("##v", "(no asset)", buf, static_cast<int>(sizeof(buf)));
            captureIfActivated();
            if (changed)
            {
                ref->guid = buf;
                ref->ptr = nullptr;
                ref->loadAttempted = false;
            }
            commitOnDeactivate();
            if (ui.IsItemHovered())
            {
                ui.SetTooltip("Asset GUID. The picker is not wired into the graph window yet");
            }
            break;
        }
        case Deki::PropertyType::NodeRef:
        {
            // Picks another node in this graph by name: the jump target of a
            // Go To node, drawn without a wire. Commits at once, like a combo.
            // The owning node is left out.
            auto* ref = static_cast<Deki::NodeRef*>(field);
            const uint32_t currentId = ref->targetId;

            char previewBuf[160];
            if (currentId == 0)
            {
                std::snprintf(previewBuf, sizeof(previewBuf), "(none)");
            }
            else if (const NodeGraphDocNode* target = selfNodeId == currentId ? nullptr : m_Doc->FindNode(currentId))
            {
                std::snprintf(previewBuf, sizeof(previewBuf), "%s (#%u)", NodeTitle(*target).c_str(), currentId);
            }
            else
            {
                std::snprintf(previewBuf, sizeof(previewBuf), "(missing #%u)", currentId);
            }

            if (ui.BeginCombo("##v", previewBuf))
            {
                if (ui.Selectable("(none)", currentId == 0) && currentId != 0)
                {
                    commit(nlohmann::json(currentId), nlohmann::json(0u));
                }
                // Only the open graph: a node reference names a sibling, never
                // a node inside another state's action flow.
                for (const NodeGraphDocNode& other : OpenGraph().nodes)
                {
                    if (other.id == selfNodeId)
                    {
                        continue;  // no self-jump
                    }
                    char itemBuf[160];
                    std::snprintf(itemBuf, sizeof(itemBuf), "%s (#%u)", NodeTitle(other).c_str(), other.id);
                    const bool selected = other.id == currentId;
                    if (ui.Selectable(itemBuf, selected) && !selected)
                    {
                        commit(nlohmann::json(currentId), nlohmann::json(other.id));
                    }
                }
                ui.EndCombo();
            }
            break;
        }
        default: ui.TextDisabled("(unsupported property type)"); break;
    }

    if (p.tooltip && ui.IsItemHovered())
    {
        ui.SetTooltip(p.tooltip);
    }
}

void NodeGraphEditorWindow::DrawObjectNamePicker(const char* componentFilter, const std::string& current,
                                                 const std::function<void(const std::string&)>& onPick)
{
    auto& ui = EditorUI::Get();

    // An empty filter lists every object; otherwise only objects with that
    // component type or a subclass.
    const char* filter = (componentFilter && componentFilter[0] != '\0') ? componentFilter : nullptr;

    ui.SameLine(0.0f, 0.0f);
    if (ui.Button(ICON_TI_CHEVRON_DOWN "##objnamepicker", ui.GetFrameHeight()))
    {
        m_ObjectPickerSearch[0] = '\0';
        ui.OpenPopup("##ng_objname");
    }

    const float dpi = ui.GetDpiScale();
    ui.SetNextWindowSizeConstraints(300.0f * dpi, 0.0f, FLT_MAX, 420.0f * dpi);

    PushContextMenuPopupStyle();
    if (ui.BeginPopup("##ng_objname"))
    {
        PushContextMenuItemStyle();

        // Picking writes the object's name; the empty name means "the object
        // the graph runs on", as an empty field does.
        auto pick = [&](const std::string& name)
        {
            if (name != current)
            {
                onPick(name);
            }
            ui.CloseCurrentPopup();
        };

        ui.SetNextItemWidth(280.0f * dpi);
        ui.InputTextWithHint("##search", "Search objects", m_ObjectPickerSearch,
                             static_cast<int>(sizeof(m_ObjectPickerSearch)));
        ui.Separator();

        ::Deki::Scene* scene = EditorApplication::Get().GetActiveScene();
        if (!scene)
        {
            ui.TextDisabled("Open a scene to pick an object");
        }
        else
        {
            if (MatchesSearch("Owner", m_ObjectPickerSearch) && ui.Selectable("(Owner)", current.empty()))
            {
                pick(std::string());
            }

            // Depth-first, so the list reads like the Hierarchy panel; each row
            // shows its parent path.
            bool anyListed = false;
            std::function<void(Deki::Object*)> listObject = [&](Deki::Object* obj)
            {
                const std::string name = obj->GetName();
                const std::string path = HierarchyPath(obj);
                if ((!filter || ObjectHasComponent(obj, filter)) && MatchesSearch(path + name, m_ObjectPickerSearch))
                {
                    anyListed = true;
                    ui.PushID(obj);
                    if (ui.Selectable((path + name).c_str(), !current.empty() && name == current))
                    {
                        pick(name);
                    }
                    ui.PopID();
                }
                for (Deki::Object* child : obj->GetChildren())
                {
                    listObject(child);
                }
            };
            for (Deki::Object* root : scene->GetObjects())
            {
                if (!root->GetParent())
                {
                    listObject(root);
                }
            }

            if (!anyListed)
            {
                if (filter)
                {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "No object with a %s", filter);
                    ui.TextDisabled(msg);
                }
                else
                {
                    ui.TextDisabled("No matching object");
                }
            }
        }

        ui.PopStyleVar();  // ItemSpacing (PushContextMenuItemStyle)
        ui.EndPopup();
    }
    ui.PopStyleVar();  // WindowPadding (PushContextMenuPopupStyle)
}

void NodeGraphEditorWindow::DrawPropertyRefControl(const Deki::PropertyInfo& p, void* instance,
                                                   const std::string& label, const std::string& editKey,
                                                   const CommitFn& commit)
{
    auto& ui = EditorUI::Get();
    auto* ref = reinterpret_cast<Deki::PropertyRef*>(static_cast<char*>(instance) + p.offset);

    // Every edit commits the whole reference as one undo step: copy it, change
    // one part, commit old and new.
    auto commitWith = [&](const nlohmann::json& before, Deki::PropertyRef next)
    { commit(before, PropertyRefToJson(next)); };

    const Deki::ComponentMeta* meta = MetaOfRef(*ref);

    // ---- Object -----------------------------------------------------------
    // Like a DEKI_OBJECT_NAME field: pick from the open scene, or type the name
    // of an object in a scene that is not open.
    ui.PushID("object");
    ui.PropertyRow((label + " Object").c_str());
    {
        const std::string objectKey = editKey + ":object";
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", ref->object.c_str());
        ui.SetNextItemWidth(ui.CalcItemWidth() - ui.GetFrameHeight());
        const bool changed = ui.InputTextWithHint("##name", "(Owner)", buf, static_cast<int>(sizeof(buf)));
        if (ui.IsItemActive() && m_EditingProperty != objectKey)
        {
            m_EditingProperty = objectKey;
            m_EditingOldValue = PropertyRefToJson(*ref);
        }
        if (changed)
        {
            ref->object = buf;  // live preview
        }
        if (ui.IsItemDeactivatedAfterEdit() && m_EditingProperty == objectKey)
        {
            commit(m_EditingOldValue, PropertyRefToJson(*ref));
            m_EditingProperty.clear();
        }

        const nlohmann::json before = PropertyRefToJson(*ref);
        DrawObjectNamePicker(nullptr, ref->object,
                             [&](const std::string& name)
                             {
                                 Deki::PropertyRef next = *ref;
                                 next.object = name;
                                 commitWith(before, next);
                             });
    }
    ui.PopID();

    // ---- Component --------------------------------------------------------
    ui.PushID("component");
    ui.PropertyRow((label + " Component").c_str());
    {
        // Only what the picked object has. With no object picked the target is
        // the graph's own object, unknown at edit time, so every registered
        // component is offered.
        Deki::Object* obj = FindSceneObjectByName(ref->object);

        const bool isTransform = (ref->component == Deki::kTransformRefComponent);
        const bool isVariable = (ref->component == Deki::kVariableRefComponent);
        const char* preview = "(none)";
        if (isTransform)
        {
            preview = "Transform";
        }
        else if (isVariable)
        {
            preview = "Variable";
        }
        else if (meta)
        {
            preview = meta->GetDisplayName();
        }
        else if (!ref->component.empty())
        {
            preview = "(missing component type)";
        }

        const nlohmann::json before = PropertyRefToJson(*ref);
        auto pickComponent = [&](const Deki::ComponentMeta* picked)
        {
            Deki::PropertyRef next = *ref;
            next.component = picked ? picked->serializedName : "";
            // The field belonged to the old component; keep it only if the new
            // one has a field with that name.
            if (!FindComponentField(picked, next.field))
            {
                next.field.clear();
            }
            commitWith(before, next);
        };

        if (ui.BeginCombo("##comp", preview))
        {
            if (ui.Selectable("(none)", ref->component.empty()))
            {
                pickComponent(nullptr);
            }

            // The object's transform first: most actions target position,
            // rotation or scale, and every object has them.
            if (ui.Selectable("Transform", isTransform) && !isTransform)
            {
                Deki::PropertyRef next = *ref;
                next.component = Deki::kTransformRefComponent;
                next.field.clear();
                commitWith(before, next);
            }

            // This graph's own variables, if it declares any. They belong to the
            // machine, not an object, so the object row above does not apply.
            if (!CollectGraphVariables().empty() && ui.Selectable("Variable", isVariable) && !isVariable)
            {
                Deki::PropertyRef next = *ref;
                next.component = Deki::kVariableRefComponent;
                next.field.clear();
                commitWith(before, next);
            }

            if (obj)
            {
                auto& registry = Deki::ComponentRegistry::Instance();
                for (Deki::Component* c : obj->GetComponents())
                {
                    const Deki::ComponentMeta* m = registry.GetMeta(c->GetType());
                    if (!m)
                    {
                        continue;
                    }
                    const bool selected = ref->component == m->serializedName;
                    if (ui.Selectable(m->GetDisplayName(), selected) && !selected)
                    {
                        pickComponent(m);
                    }
                }
            }
            else
            {
                for (const Deki::ComponentMeta* m : Deki::ComponentRegistry::Instance().GetAllComponents())
                {
                    if (!m || m->isAbstract)
                    {
                        continue;
                    }
                    const bool selected = ref->component == m->serializedName;
                    if (ui.Selectable(m->GetDisplayName(), selected) && !selected)
                    {
                        pickComponent(m);
                    }
                }
            }
            ui.EndCombo();
        }
        if (!obj && !ref->object.empty() && ui.IsItemHovered())
        {
            ui.SetTooltip("That object is not in the open scene, so every component type is listed");
        }
    }
    ui.PopID();

    // ---- Field ------------------------------------------------------------
    ui.PushID("field");
    ui.PropertyRow((label + " Field").c_str());
    {
        const bool isTransform = (ref->component == Deki::kTransformRefComponent);
        const bool isVariable = (ref->component == Deki::kVariableRefComponent);
        const nlohmann::json before = PropertyRefToJson(*ref);

        auto pickField = [&](const char* fieldName)
        {
            Deki::PropertyRef next = *ref;
            next.field = fieldName;
            commitWith(before, next);
        };

        if (isVariable)
        {
            const std::vector<GraphVariable> vars = CollectGraphVariables();
            const char* preview = ref->field.empty() ? "(none)" : ref->field.c_str();
            if (ui.BeginCombo("##field", preview))
            {
                for (const GraphVariable& var : vars)
                {
                    const bool selected = ref->field == var.name;
                    if (ui.Selectable(var.name.c_str(), selected) && !selected)
                    {
                        pickField(var.name.c_str());
                    }
                }
                if (vars.empty())
                {
                    ui.TextDisabled("This graph declares no variables");
                }
                ui.EndCombo();
            }
        }
        else if (isTransform)
        {
            // The transform's fields come from their own table, not a
            // component's (see Deki::TransformFields).
            std::string previewOwned =
                ref->field.empty() ? std::string("(none)") : EditorNaming::NicifyName(ref->field.c_str());
            if (ui.BeginCombo("##field", previewOwned.c_str()))
            {
                int count = 0;
                const char* const* names = Deki::TransformFieldNames(count);
                for (int i = 0; i < count; ++i)
                {
                    const bool selected = ref->field == names[i];
                    const std::string display = EditorNaming::NicifyName(names[i]);
                    if (ui.Selectable(display.c_str(), selected) && !selected)
                    {
                        pickField(names[i]);
                    }
                }
                ui.EndCombo();
            }
        }
        else if (!meta)
        {
            ui.TextDisabled("Pick a component first");
        }
        else
        {
            const Deki::PropertyInfo* current = FindComponentField(meta, ref->field);
            const char* preview = "(none)";
            std::string previewOwned;
            if (current)
            {
                previewOwned = EditorNaming::GetDisplayName(*current);
                preview = previewOwned.c_str();
            }
            else if (!ref->field.empty())
            {
                preview = "(missing field)";
            }

            if (ui.BeginCombo("##field", preview))
            {
                bool any = false;
                ForEachComponentField(meta,
                                      [&](const Deki::PropertyInfo& f)
                                      {
                                          // Only fields a text value can set; asset and object refs
                                          // and arrays have no text form.
                                          if (!IsLiteralWritable(f) || !f.name)
                                          {
                                              return;
                                          }
                                          any = true;
                                          const bool selected = ref->field == f.name;
                                          const std::string name = EditorNaming::GetDisplayName(f);
                                          if (ui.Selectable(name.c_str(), selected) && !selected)
                                          {
                                              pickField(f.name);
                                          }
                                      });
                if (!any)
                {
                    ui.TextDisabled("This component has no settable fields");
                }
                ui.EndCombo();
            }
        }
    }
    ui.PopID();
}

void NodeGraphEditorWindow::DrawTypedLiteralControl(const Deki::PropertyInfo& p, void* instance,
                                                    const DekiNodeMeta& meta, const std::string& editKey,
                                                    const CommitFn& commit)
{
    auto& ui = EditorUI::Get();
    auto* text = reinterpret_cast<std::string*>(static_cast<char*>(instance) + p.offset);
    const std::string label = EditorNaming::GetDisplayName(p);

    // The reference this value is for, then the type it points at. Either may
    // be missing (nothing picked yet, or a component or package no longer
    // loaded); then this is a plain text field.
    const Deki::PropertyInfo* refProp = FindProperty(meta, p.valueOfProperty);
    const Deki::PropertyInfo* target = nullptr;  // component field (has enum names, ranges)
    Deki::PropertyType targetType = Deki::PropertyType::String;
    bool haveTarget = false;
    if (refProp && refProp->type == Deki::PropertyType::PropertyRef)
    {
        const auto* ref =
            reinterpret_cast<const Deki::PropertyRef*>(static_cast<const char*>(instance) + refProp->offset);

        if (ref->component == Deki::kTransformRefComponent)
        {
            // The transform has only a type per field, no other metadata.
            int count = 0;
            const Deki::FieldRef* fields = Deki::TransformFields(count);
            const char* const* names = Deki::TransformFieldNames(count);
            for (int i = 0; i < count; ++i)
            {
                if (ref->field == names[i])
                {
                    targetType = static_cast<Deki::PropertyType>(fields[i].type);
                    haveTarget = true;
                    break;
                }
            }
        }
        else if (ref->component == Deki::kVariableRefComponent)
        {
            for (const GraphVariable& var : CollectGraphVariables())
            {
                if (var.name == ref->field)
                {
                    targetType = var.type;
                    haveTarget = true;
                    break;
                }
            }
        }
        else if ((target = FindComponentField(MetaOfRef(*ref), ref->field)) != nullptr)
        {
            targetType = target->type;
            haveTarget = true;
        }
    }

    ui.PropertyRow(label.c_str());

    // Committed at once (combo, checkbox) or when the control is left (drag,
    // text), as in the rest of the panel. Always stored as text, so the
    // runtime parses it once and the device needs no type table.
    const nlohmann::json before = nlohmann::json(*text);
    auto commitText = [&](const std::string& next)
    {
        if (next != *text)
        {
            commit(before, nlohmann::json(next));
        }
    };
    auto captureIfActivated = [&]()
    {
        if (ui.IsItemActive() && m_EditingProperty != editKey)
        {
            m_EditingProperty = editKey;
            m_EditingOldValue = before;
        }
    };
    auto commitOnDeactivate = [&]()
    {
        if (ui.IsItemDeactivatedAfterEdit() && m_EditingProperty == editKey)
        {
            commit(m_EditingOldValue, nlohmann::json(*text));
            m_EditingProperty.clear();
        }
    };

    char buf[256];
    switch (targetType)
    {
        case Deki::PropertyType::Vector2:
        {
            // "x, y" in one control, so a two-axis move is one action.
            float v[2] = { 0.0f, 0.0f };
            {
                char* end = nullptr;
                v[0] = std::strtof(text->c_str(), &end);
                while (end && (*end == ' ' || *end == ','))
                {
                    ++end;
                }
                if (end)
                {
                    v[1] = std::strtof(end, nullptr);
                }
            }
            float innerSpacingX = 0.0f;
            ui.GetItemInnerSpacing(&innerSpacingX, nullptr);
            const float half = (ui.CalcItemWidth() - innerSpacingX) * 0.5f;
            bool changed = false;
            ui.SetNextItemWidth(half);
            changed |= ui.DragFloat("##vx", &v[0], 0.05f, 0.0f, 0.0f);
            captureIfActivated();
            ui.SameLine(0.0f, innerSpacingX);
            ui.SetNextItemWidth(half);
            changed |= ui.DragFloat("##vy", &v[1], 0.05f, 0.0f, 0.0f);
            captureIfActivated();
            if (changed)
            {
                std::snprintf(buf, sizeof(buf), "%g, %g", static_cast<double>(v[0]), static_cast<double>(v[1]));
                *text = buf;  // live preview
            }
            commitOnDeactivate();
            break;
        }
        case Deki::PropertyType::Bool:
        {
            bool v = (*text == "true" || *text == "1");
            if (ui.Checkbox("##v", &v))
            {
                commitText(v ? "true" : "false");
            }
            break;
        }
        case Deki::PropertyType::Enum:
        {
            // Picked by name, stored as the index, so the device needs no enum
            // name table. Only a component field has the names (the transform
            // has no enum fields), so this needs `target`.
            const int currentIndex = std::atoi(text->c_str());
            const int enumCount = target ? target->enumCount : 0;
            const char* preview =
                (target && currentIndex >= 0 && currentIndex < enumCount) ? target->enumValues[currentIndex] : "(none)";
            if (ui.BeginCombo("##v", preview))
            {
                for (int e = 0; e < enumCount; ++e)
                {
                    const bool selected = e == currentIndex;
                    if (ui.Selectable(target->enumValues[e], selected) && !selected)
                    {
                        commitText(std::to_string(e));
                    }
                }
                ui.EndCombo();
            }
            break;
        }
        case Deki::PropertyType::Float:
        case Deki::PropertyType::Double:
        {
            // Only component fields have ranges; transform fields do not.
            const bool ranged = target && target->hasRange;
            float v = static_cast<float>(std::atof(text->c_str()));
            const bool changed =
                ui.DragFloat("##v", &v, 0.05f, ranged ? target->minValue : 0.0f, ranged ? target->maxValue : 0.0f);
            captureIfActivated();
            if (changed)
            {
                std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                *text = buf;  // live preview
            }
            commitOnDeactivate();
            break;
        }
        case Deki::PropertyType::Int8:
        case Deki::PropertyType::Int16:
        case Deki::PropertyType::Int32:
        case Deki::PropertyType::Int64:
        case Deki::PropertyType::UInt8:
        case Deki::PropertyType::UInt16:
        case Deki::PropertyType::UInt32:
        case Deki::PropertyType::UInt64:
        {
            const bool ranged = target && target->hasRange;
            int v = std::atoi(text->c_str());
            const bool changed = ui.DragInt("##v", &v, 0.2f, ranged ? static_cast<int>(target->minValue) : 0,
                                            ranged ? static_cast<int>(target->maxValue) : 0);
            captureIfActivated();
            if (changed)
            {
                *text = std::to_string(v);  // live preview
            }
            commitOnDeactivate();
            break;
        }
        default:
        {
            std::snprintf(buf, sizeof(buf), "%s", text->c_str());
            const bool changed = ui.InputText("##v", buf, sizeof(buf));
            captureIfActivated();
            if (changed)
            {
                *text = buf;  // live preview
            }
            commitOnDeactivate();
            break;
        }
    }

    if (!haveTarget && ui.IsItemHovered())
    {
        ui.SetTooltip("Pick a component and field above to get a typed editor here");
    }
}

void NodeGraphEditorWindow::DrawPropertyWidget(NodeGraphDocNode& node, const Deki::PropertyInfo& p)
{
    const uint32_t nodeId = node.id;
    const std::string propName = p.name;
    auto doc = m_Doc;
    DrawPropertyControl(node.instance, *node.meta, p, propName, node.id,
                        [doc, nodeId, propName](const nlohmann::json& oldV, const nlohmann::json& newV)
                        {
                            CommandHistory::Instance().ExecuteNoMerge(
                                std::make_unique<SetNodePropertyCommand>(doc, nodeId, propName, oldV, newV));
                        });
}

void NodeGraphEditorWindow::DrawWeightsWidget(NodeGraphDocNode& node, const Deki::PropertyInfo& p)
{
    auto& ui = EditorUI::Get();
    auto* weights = reinterpret_cast<std::vector<float>*>(static_cast<char*>(node.instance) + p.offset);

    // Grouped like the transitions list: its own band with the pin count, and
    // separators above and below its rows.
    const std::string sectionLabel = EditorNaming::GetDisplayName(p);
    char band[128];
    std::snprintf(band, sizeof(band), "%s  (%d)###ng_dynout", sectionLabel.c_str(), static_cast<int>(weights->size()));

    // Separator, band, rows, as a component section in the Inspector. No
    // Spacing() under the band: it already adds a row gap after itself.
    ui.FullBleedSeparator();
    if (!SchematicSectionBegin(band))
    {
        return;
    }

    // Deferred, as in the transitions list: the remove command rewrites the
    // vector being iterated here.
    std::function<void()> pendingRemove;
    const float rowH = ui.GetFrameHeight();

    for (size_t i = 0; i < weights->size(); ++i)
    {
        ui.PushID(static_cast<int>(i));
        const int index = static_cast<int>(i);
        char label[32];
        std::snprintf(label, sizeof(label), "Weight %d", index + 1);
        ui.PropertyRow(label);

        // Value, gap, remove button: the same row as the transitions list.
        const float rmGap = 6.0f * ui.GetDpiScale();
        ui.SetNextItemWidth(ui.CalcItemWidth() - rowH - rmGap);
        float v = (*weights)[i];
        const bool changed = ui.DragFloat("##w", &v, 0.05f, 0.0f, 0.0f);
        if (ui.IsItemActive() && m_EditingProperty != p.name)
        {
            m_EditingProperty = p.name;
            m_EditingOldValue = *weights;
        }
        if (changed)
        {
            (*weights)[i] = v;  // live preview
        }
        if (ui.IsItemDeactivatedAfterEdit() && m_EditingProperty == p.name)
        {
            CommandHistory::Instance().ExecuteNoMerge(std::make_unique<SetNodePropertyCommand>(
                m_Doc, node.id, p.name, m_EditingOldValue, nlohmann::json(*weights)));
            m_EditingProperty.clear();
        }

        // Per-row removal as in the transitions list: the pin goes, and the
        // links on the pins after it move down instead of being removed.
        // Red, because it removes the pin and the wire leaving it. The fill
        // shows only under the cursor, so a list of them does not look like a
        // column of warnings.
        ui.SameLine(0.0f, rmGap);
        ui.PushStyleColor(EditorUI::Col::Text, PackPalette(Palette::Red, 0.80f));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered, PackPalette(Palette::Red, 0.18f));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, PackPalette(Palette::Red, 0.30f));
        const bool remove = ui.Button(ICON_TI_X "##rm", rowH);
        ui.PopStyleColor(3);
        if (remove)
        {
            pendingRemove = [this, id = node.id, prop = std::string(p.name), index]()
            {
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<RemoveDynamicOutputCommand>(m_Doc, id, prop, index, "output"));
            };
        }
        if (ui.IsItemHovered())
        {
            ui.SetTooltip("Remove this output");
        }
        ui.PopID();
    }

    // The same add button as the transitions list.
    ui.Spacing();
    if (SchematicAddButton(ICON_TI_PLUS "  Add Output", 160.0f * ui.GetDpiScale()))
    {
        std::vector<float> next = *weights;
        next.push_back(1.0f);
        CommandHistory::Instance().ExecuteNoMerge(
            std::make_unique<SetWeightCountCommand>(m_Doc, node.id, p.name, *weights, std::move(next)));
    }

    ui.FullBleedSeparator();
    SchematicSectionEnd();

    if (pendingRemove)
    {
        pendingRemove();
    }
}

void NodeGraphEditorWindow::DrawTransitionsWidget(NodeGraphDocNode& node, const Deki::PropertyInfo& p)
{
    auto& ui = EditorUI::Get();
    auto* events = reinterpret_cast<std::vector<std::string>*>(static_cast<char*>(node.instance) + p.offset);

    // Both labels come from the property, so a state's `transitions` heads a
    // "Transitions" section of "Transition 1..N" rows and a group's `exits`
    // heads "Exits" of "Exit 1..N", without this widget knowing either type.
    const std::string sectionLabel = EditorNaming::GetDisplayName(p);
    std::string rowLabel = sectionLabel;
    if (rowLabel.size() > 1 && rowLabel.back() == 's')
    {
        rowLabel.pop_back();
    }

    // The list is its own inspector section, not loose rows among the node's
    // settings: its own band and count, with separators above and below. The
    // "###" id keeps it open while the count in the label changes.
    char band[128];
    std::snprintf(band, sizeof(band), "%s  (%d)###ng_dynout", sectionLabel.c_str(), static_cast<int>(events->size()));

    // Separator, band, rows, as a component section in the Inspector. No
    // Spacing() under the band: it already adds a row gap after itself.
    ui.FullBleedSeparator();
    if (!SchematicSectionBegin(band))
    {
        return;
    }

    // Each entry is one output pin, labelled by its value. Renames commit in
    // place (same pin count); each row has a remove button, so a pin can be
    // dropped from the middle and the links on later pins move down with them
    // (RemoveDynamicOutputCommand).
    //
    // Removal waits until after the loop: the command rewrites `events` at
    // once, which would invalidate the vector being iterated.
    std::function<void()> pendingRemove;

    const float rowH = ui.GetFrameHeight();

    for (size_t i = 0; i < events->size(); ++i)
    {
        const int index = static_cast<int>(i);
        ui.PushID(index);
        char label[64];
        std::snprintf(label, sizeof(label), "%s %d", rowLabel.c_str(), index + 1);
        ui.PropertyRow(label);

        // The field, then a square remove button at the row's right end, as
        // DrawPropertyControl places a DEKI_OBJECT_NAME field's picker, with a
        // gap so it reads as its own control.
        const float rmGap = 6.0f * ui.GetDpiScale();
        ui.SetNextItemWidth(ui.CalcItemWidth() - rowH - rmGap);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", (*events)[i].c_str());
        const std::string editKey = std::string(p.name) + "#" + std::to_string(i);
        const bool changed = ui.InputText("##e", buf, sizeof(buf));
        if (ui.IsItemActive() && m_EditingProperty != editKey)
        {
            m_EditingProperty = editKey;
            m_EditingOldValue = *events;
        }
        if (changed)
        {
            (*events)[i] = buf;  // live preview; the pin label follows
        }
        if (ui.IsItemDeactivatedAfterEdit() && m_EditingProperty == editKey)
        {
            CommandHistory::Instance().ExecuteNoMerge(std::make_unique<SetNodePropertyCommand>(
                m_Doc, node.id, p.name, m_EditingOldValue, nlohmann::json(*events)));
            m_EditingProperty.clear();
        }

        // Red, because it removes the pin and the wire leaving it. The fill
        // shows only under the cursor, so a list of them does not look like a
        // column of warnings.
        ui.SameLine(0.0f, rmGap);
        ui.PushStyleColor(EditorUI::Col::Text, PackPalette(Palette::Red, 0.80f));
        ui.PushStyleColor(EditorUI::Col::ButtonHovered, PackPalette(Palette::Red, 0.18f));
        ui.PushStyleColor(EditorUI::Col::ButtonActive, PackPalette(Palette::Red, 0.30f));
        const bool remove = ui.Button(ICON_TI_X "##rm", rowH);
        ui.PopStyleColor(3);
        if (remove)
        {
            pendingRemove = [this, id = node.id, prop = std::string(p.name), index, rowName = rowLabel]()
            {
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<RemoveDynamicOutputCommand>(m_Doc, id, prop, index, rowName));
            };
        }
        if (ui.IsItemHovered())
        {
            ui.SetTooltip("Remove this output pin, and the wire leaving it");
        }
        ui.PopID();
    }

    if (events->empty())
    {
        ui.TextDisabled("No output pins.");
    }

    // The editor's shared add button: centred, outlined, quiet until hovered,
    // like the one under the Inspector's component list.
    ui.Spacing();
    const std::string addLabel = std::string(ICON_TI_PLUS "  Add ") + rowLabel;
    if (SchematicAddButton(addLabel.c_str(), 160.0f * ui.GetDpiScale()))
    {
        nlohmann::json next = *events;
        next.push_back("EVENT");
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ResizeDynamicOutputsCommand>(
            m_Doc, node.id, p.name, nlohmann::json(*events), std::move(next)));
    }

    // Close the section as it opened, so the list reads as one block.
    ui.FullBleedSeparator();
    SchematicSectionEnd();

    if (pendingRemove)
    {
        pendingRemove();
    }
}

void NodeGraphEditorWindow::DrawChildStack(NodeGraphDocNode& node)
{
    auto& ui = EditorUI::Get();

    // The section label is the child category after the slash ("Fsm/Actions"
    // gives "Actions"), or the whole category without one.
    const char* cat = node.meta->childCategory;
    const char* slash = std::strchr(cat, '/');
    const std::string sectionLabel = slash ? std::string(slash + 1) : std::string(cat);

    // The section starts like every other: a full-width line with the band
    // right below it.
    ui.FullBleedSeparator();
    if (!SchematicSectionBegin(sectionLabel.c_str()))
    {
        return;
    }

    // Adds, removes and moves wait until after the loop: the commands change
    // node.children at once, which would invalidate the iteration.
    std::function<void()> pendingOp;

    for (size_t i = 0; i < node.children.size(); ++i)
    {
        NodeGraphDocChild& child = node.children[i];
        const int index = static_cast<int>(i);
        const int lastIndex = static_cast<int>(node.children.size()) - 1;
        ui.PushID(index);

        // One collapsible band per entry, like a component in the Inspector:
        // its position (run order) and the type's display name, with the enable
        // toggle and a menu at the band's right end. The "###e" id ties the
        // open state to the slot, so folded entries stay put when the stack is
        // reordered.
        char title[192];
        std::snprintf(title, sizeof(title), "%d. %s###e", index + 1, NodeDisplayName(child.meta).c_str());
        const bool open =
            SchematicSectionBegin(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        ui.OpenPopupOnItemClick("##ng_entrymenu");

        // Band controls, laid out right to left (as SchematicActionLink does:
        // the header is the previous item, so its rect is the band). They move
        // the layout cursor, which is restored before the rows below.
        {
            float bandMinY = 0.0f, bandMaxY = 0.0f;
            ui.GetItemRect(nullptr, &bandMinY, nullptr, &bandMaxY);
            float afterBandX = 0.0f, afterBandY = 0.0f;
            ui.GetCursorScreenPos(&afterBandX, &afterBandY);
            float framePaddingX = 0.0f, innerSpacingX = 0.0f;
            ui.GetFramePadding(&framePaddingX, nullptr);
            ui.GetItemInnerSpacing(&innerSpacingX, nullptr);
            float availW = 0.0f;
            ui.GetContentRegionAvail(&availW, nullptr);
            const float slotH = ui.GetFrameHeight();
            const float top = bandMinY + (bandMaxY - bandMinY - slotH) * 0.5f;
            // The right content edge (before the scrollbar), one frame padding
            // in from the seam, as SchematicActionLink uses.
            float right = afterBandX + availW - framePaddingX;

            right -= slotH;
            ui.SetCursorScreenPos(right, top);
            if (ui.Button(ICON_TI_DOTS_VERTICAL "##menu", slotH))
            {
                ui.OpenPopup("##ng_entrymenu");
            }

            // The bare checkbox is only its box wide (font * 1.2) but takes a
            // full frame height, like the Inspector's.
            const float boxW = std::floor(ui.GetFontSize() * 1.2f);
            right -= boxW + innerSpacingX;
            ui.SetCursorScreenPos(right, top);
            bool enabled = child.enabled;
            if (ui.Checkbox("##en", &enabled))
            {
                pendingOp = [this, id = node.id, index, enabled]()
                {
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<SetChildEnabledCommand>(m_Doc, id, index, enabled));
                };
            }
            if (ui.IsItemHovered())
            {
                ui.SetTooltip(child.enabled ? "Enabled (skipped when off)" : "Disabled");
            }

            ui.SetCursorScreenPos(afterBandX, afterBandY);
        }

        // The entry's menu, also opened by right-clicking the band.
        PushContextMenuPopupStyle();
        if (ui.BeginPopup("##ng_entrymenu"))
        {
            PushContextMenuItemStyle();
            if (ui.MenuItem("Move Up", index > 0))
            {
                pendingOp = [this, id = node.id, index]()
                {
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<MoveChildCommand>(m_Doc, id, index, index - 1));
                };
            }
            if (ui.MenuItem("Move Down", index < lastIndex))
            {
                pendingOp = [this, id = node.id, index]()
                {
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<MoveChildCommand>(m_Doc, id, index, index + 1));
                };
            }
            if (ui.MenuItem(child.enabled ? "Disable" : "Enable"))
            {
                pendingOp = [this, id = node.id, index, on = !child.enabled]()
                {
                    CommandHistory::Instance().ExecuteNoMerge(
                        std::make_unique<SetChildEnabledCommand>(m_Doc, id, index, on));
                };
            }
            ui.Separator();
            if (ui.MenuItem("Remove"))
            {
                pendingOp = [this, id = node.id, index]()
                { CommandHistory::Instance().ExecuteNoMerge(std::make_unique<RemoveChildCommand>(m_Doc, id, index)); };
            }
            ui.PopStyleVar();  // ItemSpacing (PushContextMenuItemStyle)
            ui.EndPopup();
        }
        ui.PopStyleVar();  // WindowPadding (PushContextMenuPopupStyle)

        // The child's reflected properties, committed against (node, index).
        if (open)
        {
            ui.Spacing();
            for (int pi = 0; pi < child.meta->propertyCount; ++pi)
            {
                const Deki::PropertyInfo& p = child.meta->properties[pi];
                ui.PushID(p.name);
                const std::string editKey = "child#" + std::to_string(i) + ":" + p.name;
                const uint32_t nodeId = node.id;
                auto doc = m_Doc;
                const std::string propName = p.name;
                DrawPropertyControl(
                    child.instance, *child.meta, p, editKey, node.id,
                    [doc, nodeId, index, propName](const nlohmann::json& oldV, const nlohmann::json& newV)
                    {
                        CommandHistory::Instance().ExecuteNoMerge(
                            std::make_unique<SetChildPropertyCommand>(doc, nodeId, index, propName, oldV, newV));
                    });
                ui.PopID();
            }
            if (child.meta->propertyCount == 0)
            {
                ui.TextDisabled("No settings");
            }
            ui.Spacing();
            SchematicSectionEnd();
        }

        ui.FullBleedSeparator();
        ui.PopID();
    }

    // Add menu: every registered node type in this child category, behind the
    // same outlined add button the other lists end with.
    const std::string addLabel = std::string(ICON_TI_PLUS "  Add ") + sectionLabel;
    if (SchematicAddButton(addLabel.c_str(), 160.0f * ui.GetDpiScale()))
    {
        ui.OpenPopup("##ng_addchild");
    }

    // The editor's shared picker, as Add Component opens: a title, a search
    // ranked best first, and full keyboard control. A stack's types share one
    // category, so they are listed flat, not in a single group.
    const float pickerW = 300.0f * ui.GetDpiScale();
    ui.SetNextWindowSizeConstraints(pickerW, 0.0f, pickerW, 480.0f * ui.GetDpiScale());
    PushContextMenuPopupStyle();
    if (ui.BeginPopup("##ng_addchild"))
    {
        // Pointers into the metas' static strings; nothing here outlives the
        // frame except what the registry owns.
        std::vector<const DekiNodeMeta*> metas;
        std::vector<PickerItem> picks;
        for (const DekiNodeMeta* meta : NodeTypeRegistry::Instance().GetAllNodes())
        {
            if (!meta->category || std::strcmp(meta->category, cat) != 0)
            {
                continue;
            }
            metas.push_back(meta);
            PickerItem item;
            item.label = meta->displayName && meta->displayName[0] ? meta->displayName : meta->name;
            item.altName = meta->name;
            // What it does, on the row's second line, since names like "Set
            // Bool" and "Set Bool Value" are not enough on their own.
            item.description = meta->description;
            picks.push_back(item);
        }

        const std::string title = "Add " + sectionLabel;
        const std::string hint = "Search " + sectionLabel;
        const int chosen = SchematicPicker(
            "##ng_addchild_picker", title.c_str(), picks.data(), static_cast<int>(picks.size()), m_AddChildJustOpened,
            320.0f * ui.GetDpiScale(), ICON_TI_BOLT, hint.c_str(), "No types registered for this stack");
        m_AddChildJustOpened = false;

        if (chosen >= 0 && chosen < static_cast<int>(metas.size()))
        {
            pendingOp =
                [this, id = node.id, typeId = metas[chosen]->typeId, typeName = std::string(metas[chosen]->name)]()
            {
                CommandHistory::Instance().ExecuteNoMerge(
                    std::make_unique<AddChildCommand>(m_Doc, id, typeId, typeName));
            };
            ui.CloseCurrentPopup();
        }
        ui.EndPopup();
    }
    else
    {
        m_AddChildJustOpened = true;  // focus the search on the next open
    }
    ui.PopStyleVar();  // WindowPadding (PushContextMenuPopupStyle)

    SchematicSectionEnd();

    if (pendingOp)
    {
        pendingOp();
    }
}

std::vector<NodeGraphEditorWindow::GraphVariable> NodeGraphEditorWindow::CollectGraphVariables() const
{
    std::vector<GraphVariable> out;
    if (!m_Doc)
    {
        return out;
    }

    // Any node type marked DEKI_NODE_VARIABLES declares variables through its
    // child stack. Each child's title property is the name, and its first
    // other exported property gives the type. Only the root is searched:
    // variables belong to the whole document and are visible in every
    // subgraph.
    for (const NodeGraphDocNode& node : m_Doc->root.nodes)
    {
        if (!node.meta || !node.meta->declaresVariables)
        {
            continue;
        }

        for (const NodeGraphDocChild& child : node.children)
        {
            if (!child.meta || !child.instance || !child.meta->titleProperty)
            {
                continue;
            }

            GraphVariable var;
            bool haveType = false;
            for (int i = 0; i < child.meta->propertyCount; ++i)
            {
                const Deki::PropertyInfo& p = child.meta->properties[i];
                if (std::strcmp(p.name, child.meta->titleProperty) == 0)
                {
                    var.name =
                        *reinterpret_cast<const std::string*>(static_cast<const char*>(child.instance) + p.offset);
                }
                else if (!haveType)
                {
                    var.type = p.type;
                    haveType = true;
                }
            }
            if (!var.name.empty() && haveType)
            {
                out.push_back(std::move(var));
            }
        }
    }
    return out;
}

std::string NodeGraphEditorWindow::NodeTitle(const NodeGraphDocNode& node) const
{
    if (node.meta->titleProperty && node.instance)
    {
        if (const Deki::PropertyInfo* p = FindProperty(*node.meta, node.meta->titleProperty))
        {
            if (p->type == Deki::PropertyType::String)
            {
                const auto* s =
                    reinterpret_cast<const std::string*>(static_cast<const char*>(node.instance) + p->offset);
                if (!s->empty())
                {
                    return *s;
                }
            }
        }
    }
    return NodeDisplayName(node.meta);
}

// ============================================================================
// Modals
// ============================================================================

void NodeGraphEditorWindow::DrawModals()
{
    auto& ui = EditorUI::Get();

    if (m_ConfirmPopupPending)
    {
        m_ConfirmPopupPending = false;
        ui.OpenPopup("Unsaved Graph Changes");
    }
    if (m_ErrorPopupPending)
    {
        m_ErrorPopupPending = false;
        ui.OpenPopup("Node Graph Error");
    }

    if (BeginSchematicModal("Unsaved Graph Changes", 420.0f))
    {
        const std::string name = m_Doc ? std::filesystem::path(m_Doc->assetPath).filename().string() : "";
        char msg[512];
        std::snprintf(msg, sizeof(msg), "'%s' has unsaved changes.", name.c_str());
        ui.Text(msg);
        ui.Spacing();

        auto proceed = [this]()
        {
            if (m_ConfirmAction == ConfirmAction::OpenPending)
            {
                std::string error;
                if (!LoadDocument(m_PendingOpenPath, m_PendingOpenCache, error))
                {
                    m_ErrorText = error;
                    m_ErrorPopupPending = true;
                }
            }
            else if (m_ConfirmAction == ConfirmAction::CloseWindow)
            {
                CloseDocument();
                SetOpen(false);
                m_CloseAfterConfirm = true;  // so the open flag does not reopen it
            }
            m_ConfirmAction = ConfirmAction::None;
            m_PendingOpenPath.clear();
            m_PendingOpenCache.clear();
        };

        // The editor's dialog row: Cancel, the destructive Discard, then Save
        // as the main action.
        const int choice = DekiEditor::SchematicDialogButtons({ { "Cancel" },
                                                                { "Discard", DekiEditor::DialogButton::Danger },
                                                                { "Save", DekiEditor::DialogButton::Primary } });
        if (choice == 2)
        {
            SaveDocument();
            ui.CloseCurrentPopup();
            proceed();
        }
        else if (choice == 1)
        {
            ui.CloseCurrentPopup();
            proceed();
        }
        else if (choice == 0)
        {
            m_ConfirmAction = ConfirmAction::None;
            m_PendingOpenPath.clear();
            m_PendingOpenCache.clear();
            ui.CloseCurrentPopup();
        }
        EndSchematicModal();
    }

    if (BeginSchematicModal("Node Graph Error", 460.0f))
    {
        ui.TextWrapped(m_ErrorText.c_str());
        ui.Spacing();
        if (DekiEditor::SchematicDialogButtons({ { "OK", DekiEditor::DialogButton::Primary } }) == 0)
        {
            m_ErrorText.clear();
            ui.CloseCurrentPopup();
        }
        EndSchematicModal();
    }
}

// ============================================================================
// Session (hot reload survival)
// ============================================================================

bool NodeGraphEditorWindow::SaveSession(std::string& outJson)
{
    // The last call before the package DLLs unload, and the preview instance
    // belongs to one of them. It holds only runtime state (live particles), so
    // dropping it loses nothing; RestoreSession makes a new one.
    DestroyPreview();

    nlohmann::json s;
    if (m_Doc)
    {
        s["assetPath"] = m_Doc->assetPath;
        s["cachePath"] = m_Doc->cachePath;
        s["dirty"] = m_Doc->dirty;
        s["doc"] = m_Doc->ToJson();
        s["panX"] = m_Canvas.GetPanX();
        s["panY"] = m_Canvas.GetPanY();
        s["zoom"] = m_Canvas.GetZoom();
        s["selectedNode"] = m_SelectedNode;
        s["graphPath"] = m_GraphPath;  // stay inside the state you were editing
    }
    outJson = s.dump();
    return true;
}

void NodeGraphEditorWindow::RestoreSession(const std::string& json)
{
    nlohmann::json s;
    try
    {
        s = nlohmann::json::parse(json);
    }
    catch (const nlohmann::json::exception&)
    {
        return;
    }

    if (!s.contains("assetPath"))
    {
        return;  // the window had no document
    }

    const nlohmann::json docJson = s.value("doc", nlohmann::json::object());
    const std::string assetType = docJson.value("type", "");
    const DekiNodeGraphDomain* domain = NodeGraphDomainRegistry::Instance().Get(assetType);

    auto doc = std::make_shared<NodeGraphDocument>();
    std::string error;
    if (!domain)
    {
        error = "'" + assetType + "' is no longer a registered node graph type";
    }
    else
    {
        doc->domain = domain;
        doc->FromJson(docJson, error);
    }

    if (!error.empty())
    {
        // Unsaved work must not vanish: log the whole session so it can be
        // recovered from the console, then open empty.
        DEKI_LOG_ERROR("NodeGraphEditorWindow: session restore failed after reload: %s", error.c_str());
        DEKI_LOG_ERROR("NodeGraphEditorWindow: unsaved session content follows:\n%s", json.c_str());
        m_ErrorText = "Could not restore the open graph after reload: " + error +
                      "\nThe unsaved content was written to the Console log.";
        m_ErrorPopupPending = true;
        return;
    }

    doc->assetPath = s.value("assetPath", "");
    doc->cachePath = s.value("cachePath", "");
    doc->dirty = s.value("dirty", false);
    m_Doc = std::move(doc);
    m_SelectedNode = s.value("selectedNode", 0u);
    m_SelectedLink = -1;
    m_GraphPath = s.value("graphPath", std::vector<uint32_t>{});
    ValidateGraphPath();  // node types can vanish across a reload
    EnsurePermanentNodes();
    m_Canvas.SetView(s.value("panX", 0.0f), s.value("panY", 0.0f), s.value("zoom", 1.0f));
}

// ============================================================================
// Registration
// ============================================================================

// Registers at DLL load, like every tool window a package provides. The
// editor clears the window registry only when it also unloads the package
// DLLs, so this registrar always runs again when needed.
REGISTER_EDITOR_WINDOW(NodeGraphEditorWindow, "Node Graph", "Tools/Node Graph")

}  // namespace DekiEditor
