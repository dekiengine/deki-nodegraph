#pragma once

#ifdef DEKI_EDITOR
#include <cstdint>
#endif

namespace DekiNodeGraph
{

#ifdef DEKI_EDITOR

// Optional live preview for a node graph domain (editor only).
//
// The Node Graph window does not know what any domain's nodes mean, so it
// cannot preview them itself. A domain that can show its graph running
// supplies these hooks; the window then offers a Preview panel, ticks it with
// the editor's frame delta, and gives it a rectangle to draw in.
//
// The graph passed to Tick is the live document being edited, not the saved
// asset, so the preview shows an edit at once. It is a flat view of one graph
// level (the root), in the same {id, typeId, instance} and link shape the
// runtime loader produces, so a domain's interpreter can walk either with the
// same code.
//
// Drawing goes through the canvas, not the editor's UI: package DLLs share the
// editor's ImGui context only by pointer, and a preview has no reason to touch
// it. Coordinates are absolute screen pixels, already clipped to the preview
// rect.

struct NodeGraphPreviewNode
{
    uint32_t id = 0;
    uint32_t typeId = 0;
    void* instance = nullptr;  // live node struct, owned by the document
};

struct NodeGraphPreviewLink
{
    uint32_t fromNode = 0;
    int32_t fromPin = 0;
    uint32_t toNode = 0;
    int32_t toPin = 0;
};

// One graph level, borrowed for the duration of the Tick call. Never stored.
struct NodeGraphPreviewGraph
{
    const NodeGraphPreviewNode* nodes = nullptr;
    int nodeCount = 0;
    const NodeGraphPreviewLink* links = nullptr;
    int linkCount = 0;

    /// The first node of `typeId`, or nullptr. As in NodeGraphData::Graph.
    const NodeGraphPreviewNode* FindFirstOfType(uint32_t typeId) const
    {
        for (int i = 0; i < nodeCount; ++i)
        {
            if (nodes[i].typeId == typeId)
            {
                return &nodes[i];
            }
        }
        return nullptr;
    }

    /// Follows the link leaving (nodeId, fromPin). As in NodeGraphData::Graph.
    const NodeGraphPreviewNode* Next(uint32_t nodeId, int32_t fromPin) const
    {
        for (int i = 0; i < linkCount; ++i)
        {
            if (links[i].fromNode != nodeId || links[i].fromPin != fromPin)
            {
                continue;
            }
            for (int n = 0; n < nodeCount; ++n)
            {
                if (nodes[n].id == links[i].toNode)
                {
                    return &nodes[n];
                }
            }
        }
        return nullptr;
    }
};

/// Packs a colour for the primitives below. The byte order is EditorUI::Rgba's
/// (and ImGui's IM_COL32), not the 0xRRGGBBAA a reader would guess, so use this
/// rather than shifting by hand.
inline uint32_t NodeGraphPreviewRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(r);
}

// The drawing primitives a preview may use, bound to the window's draw list.
// Colors come from NodeGraphPreviewRgba.
struct NodeGraphPreviewCanvas
{
    void* ctx = nullptr;
    void (*circleFilled)(void* ctx, float cx, float cy, float radius, uint32_t rgba) = nullptr;
    void (*rectFilled)(void* ctx, float x0, float y0, float x1, float y1, uint32_t rgba) = nullptr;
    void (*line)(void* ctx, float x0, float y0, float x1, float y1, uint32_t rgba, float thickness) = nullptr;
};

/// Optional per-node illustration, drawn in the properties panel under the
/// selected node's title (editor only).
///
/// A domain that can picture one of its nodes (an emitter's shape, a ramp, a
/// gradient, a force vector) supplies these, and the panel gives that node a
/// band to draw in, redrawn as its fields are edited.
///
/// The same rules as the preview above: the instance is the live node struct,
/// and drawing goes through NodeGraphPreviewCanvas, since the provider lives in
/// another DLL. Coordinates are absolute screen pixels, clipped to the band.
struct NodeGraphNodeGizmoOps
{
    // The height in CSS px this node wants, or 0 for nothing to show (most
    // node types). Asked every frame with the live instance, so a node can
    // size itself by its values.
    float (*height)(uint32_t typeId, const void* instance) = nullptr;

    // Draws into (x, y, w, h), in screen pixels. `dpi` is the editor's scale,
    // for line thickness and anything else measured in pixels rather than in
    // proportion to the band.
    void (*draw)(uint32_t typeId, const void* instance, float x, float y, float w, float h, float dpi,
                 const NodeGraphPreviewCanvas& canvas) = nullptr;
};

/// A domain's preview. The window offers a Preview panel only when all four
/// hooks are set.
struct NodeGraphPreviewOps
{
    // One preview instance per open document.
    void* (*create)() = nullptr;
    void (*destroy)(void* preview) = nullptr;

    // Returns to the starting state (the transport's Restart).
    void (*reset)(void* preview) = nullptr;

    // Advances by dt seconds and draws. (x, y) is the preview rect's top-left
    // in screen pixels, (w, h) its size; pixelsPerMeter converts the domain's
    // world units to it. dt is 0 while paused, and the current state must
    // still be drawn.
    void (*tick)(void* preview, const NodeGraphPreviewGraph& graph, float dt, float x, float y, float w, float h,
                 float pixelsPerMeter, const NodeGraphPreviewCanvas& canvas) = nullptr;
};

#endif  // DEKI_EDITOR

}  // namespace DekiNodeGraph
