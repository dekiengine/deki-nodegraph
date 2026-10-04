#pragma once

// Undoable edits to a NodeGraphDocument, for the Node Graph window.
//
// Every command holds the document by weak_ptr plus plain data (ids, JSON,
// positions), never node instance pointers, which die on hot reload. With the
// document gone (window or project closed), Execute and Undo log an error and
// do nothing.
//
// Most node property edits are not here: the window's properties panel uses
// EditorUI value-mode fields, whose recordEdit backend already pushes a
// ModifyValueCommand (merging a drag into one step).

#include <deki-editor/Command.h>
#include "deki-nodegraph/editor/NodeGraphDocument.h"
#include "deki-nodegraph/editor/NodePropertyJson.h"

#include "deki-nodegraph/DekiNode.h"
#include <deki/LogSystem.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Editor extensions live in DekiEditor; the package's own types are in DekiNodeGraph.
using namespace DekiNodeGraph;

namespace DekiEditor
{

namespace NodeGraphCommandDetail
{
inline std::shared_ptr<NodeGraphDocument> Lock(const std::weak_ptr<NodeGraphDocument>& doc, const char* what)
{
    auto locked = doc.lock();
    if (!locked)
    {
        DEKI_LOG_ERROR("NodeGraphCommands: %s skipped — node graph document no longer open", what);
    }
    return locked;
}
}  // namespace NodeGraphCommandDetail

/// Adds a node of a given type at a canvas position, in the graph owned by
/// `ownerNodeId` (0 = the document root), which is the graph the canvas has
/// open.
///
/// A subgraph node is more than one node: AddNode also creates its inner graph
/// and entry node, using more ids. So redo replays the captured subtree rather
/// than creating a bare node, and undo puts the id counter back to where it was.
class AddNodeCommand : public Command
{
public:
    AddNodeCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t typeId, std::string typeName, float x, float y,
                   uint32_t ownerNodeId = 0)
        : m_Doc(std::move(doc)),
          m_TypeId(typeId),
          m_TypeName(std::move(typeName)),
          m_X(x),
          m_Y(y),
          m_Owner(ownerNodeId)
    {
    }

    std::string GetDescription() const override { return "Add " + m_TypeName + " node"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "AddNode");
        if (!doc)
        {
            return;
        }

        if (m_NodeId == 0)
        {
            // First run: create it, then capture the whole subtree so redo
            // recreates the same node, inner graph included.
            m_NextIdBefore = doc->nextNodeId;
            m_NodeId = doc->AddNode(m_TypeId, m_X, m_Y, m_Owner);
            if (const NodeGraphDocNode* node = doc->FindNode(m_NodeId))
            {
                m_Node = doc->NodeToJson(*node);
            }
        }
        else
        {
            doc->NodeFromJson(m_Owner, m_Node);
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "AddNode undo");
        if (!doc || m_NodeId == 0)
        {
            return;
        }
        doc->RemoveNode(m_NodeId);
        // Undo runs in reverse order, so every id handed out after this command
        // is already given back, and rewinding the counter cannot collide.
        if (m_NextIdBefore != 0)
        {
            doc->nextNodeId = m_NextIdBefore;
        }
        doc->dirty = true;
    }

    uint32_t GetNodeId() const { return m_NodeId; }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_TypeId;
    std::string m_TypeName;
    float m_X, m_Y;
    uint32_t m_Owner;
    uint32_t m_NodeId = 0;
    uint32_t m_NextIdBefore = 0;
    nlohmann::json m_Node;  // full subtree (values + children + inner graph)
};

/// Deletes a node and every link attached to it in its graph. The node's whole
/// subtree (child stack and inner graph, recursively) is captured, so undoing
/// the deletion of a group brings back everything inside it.
class DeleteNodeCommand : public Command
{
public:
    DeleteNodeCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId)
    {
    }

    std::string GetDescription() const override { return "Delete node"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "DeleteNode");
        if (!doc)
        {
            return;
        }

        if (const NodeGraphDocNode* node = doc->FindNode(m_NodeId))
        {
            // Capture everything needed to restore it on the first run.
            m_Owner = doc->OwnerOf(m_NodeId);
            m_Node = doc->NodeToJson(*node);
            m_Links = doc->AttachedLinks(m_NodeId);
        }
        doc->RemoveNode(m_NodeId);
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "DeleteNode undo");
        if (!doc || m_Node.is_null())
        {
            return;
        }
        if (doc->NodeFromJson(m_Owner, m_Node))
        {
            for (const auto& link : m_Links)
            {
                doc->AddLink(link);
            }
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    uint32_t m_Owner = 0;
    nlohmann::json m_Node;  // full subtree (values + children + inner graph)
    std::vector<NodeGraphDocLink> m_Links;
};

/// One node drag. The window moves the node directly during the drag and
/// pushes this once on release; Execute applies the end position again (no
/// change the first time), Undo restores the start position.
class MoveNodeCommand : public Command
{
public:
    MoveNodeCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, float oldX, float oldY, float newX,
                    float newY)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_OldX(oldX),
          m_OldY(oldY),
          m_NewX(newX),
          m_NewY(newY)
    {
    }

    std::string GetDescription() const override { return "Move node"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "MoveNode");
        if (doc)
        {
            doc->SetNodePos(m_NodeId, m_NewX, m_NewY);
            // The drag already wrote the new position, so SetNodePos saw no
            // change and did not mark the graph dirty. Mark it here.
            if (m_OldX != m_NewX || m_OldY != m_NewY)
            {
                doc->dirty = true;
            }
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "MoveNode undo");
        if (doc)
        {
            doc->SetNodePos(m_NodeId, m_OldX, m_OldY);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    float m_OldX, m_OldY, m_NewX, m_NewY;
};

/// Creates a link. An output pin has at most one link: an existing link out of
/// the same (fromNode, fromPin) is replaced, and undo restores it.
class AddLinkCommand : public Command
{
public:
    AddLinkCommand(std::weak_ptr<NodeGraphDocument> doc, NodeGraphDocLink link)
        : m_Doc(std::move(doc)),
          m_Link(link)
    {
    }

    std::string GetDescription() const override { return "Connect nodes"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "AddLink");
        if (!doc)
        {
            return;
        }
        m_HadReplaced = doc->RemoveLinkFromPin(m_Link.fromNode, m_Link.fromPin, &m_Replaced);
        doc->AddLink(m_Link);
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "AddLink undo");
        if (!doc)
        {
            return;
        }
        doc->RemoveLink(m_Link);
        if (m_HadReplaced)
        {
            doc->AddLink(m_Replaced);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    NodeGraphDocLink m_Link;
    NodeGraphDocLink m_Replaced;
    bool m_HadReplaced = false;
};

/// Removes one link.
class RemoveLinkCommand : public Command
{
public:
    RemoveLinkCommand(std::weak_ptr<NodeGraphDocument> doc, NodeGraphDocLink link)
        : m_Doc(std::move(doc)),
          m_Link(link)
    {
    }

    std::string GetDescription() const override { return "Disconnect nodes"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "RemoveLink");
        if (doc)
        {
            doc->RemoveLink(m_Link);
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "RemoveLink undo");
        if (doc)
        {
            doc->AddLink(m_Link);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    NodeGraphDocLink m_Link;
};

/// Sets one reflected property of a node, with old and new values as JSON, so
/// no instance pointer is stored.
///
/// The properties panel pushes it when an edit is committed (drag released,
/// field left), so one drag is one command. During the drag the instance is
/// written directly; Execute applies the committed value again.
class SetNodePropertyCommand : public Command
{
public:
    SetNodePropertyCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, std::string propertyName,
                           nlohmann::json oldValue, nlohmann::json newValue)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Property(std::move(propertyName)),
          m_Old(std::move(oldValue)),
          m_New(std::move(newValue))
    {
    }

    std::string GetDescription() const override { return "Edit " + m_Property; }

    void Execute() override { Apply(m_New, "SetNodeProperty"); }
    void Undo() override { Apply(m_Old, "SetNodeProperty undo"); }

private:
    void Apply(const nlohmann::json& value, const char* what)
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, what);
        if (!doc)
        {
            return;
        }
        NodeGraphDocNode* node = doc->FindNode(m_NodeId);
        if (!node)
        {
            DEKI_LOG_ERROR("SetNodePropertyCommand: node %u no longer exists", m_NodeId);
            return;
        }
        nlohmann::json values;
        values[m_Property] = value;
        if (NodePropertiesFromJson(node->instance, *node->meta, values))
        {
            doc->dirty = true;
        }
    }

    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    std::string m_Property;
    nlohmann::json m_Old, m_New;
};

/// Resizes a dynamic-output weights vector (adds or removes an output pin of an
/// FsmRandom-style node). Shrinking removes the links from the dropped pins in
/// the same undo step.
class SetWeightCountCommand : public Command
{
public:
    // newWeights is the whole new vector (the window builds it).
    SetWeightCountCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, std::string propertyName,
                          std::vector<float> oldWeights, std::vector<float> newWeights)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Property(std::move(propertyName)),
          m_Old(std::move(oldWeights)),
          m_New(std::move(newWeights))
    {
    }

    std::string GetDescription() const override
    {
        return m_New.size() > m_Old.size() ? "Add output pin" : "Remove output pin";
    }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "SetWeightCount");
        if (!doc)
        {
            return;
        }
        if (!Apply(*doc, m_New))
        {
            return;
        }

        // Remove links leaving pins that no longer exist.
        m_PrunedLinks.clear();
        const int count = static_cast<int>(m_New.size());
        for (const auto& link : doc->AttachedLinks(m_NodeId))
        {
            if (link.fromNode == m_NodeId && link.fromPin >= count)
            {
                m_PrunedLinks.push_back(link);
                doc->RemoveLink(link);
            }
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "SetWeightCount undo");
        if (!doc)
        {
            return;
        }
        if (!Apply(*doc, m_Old))
        {
            return;
        }
        for (const auto& link : m_PrunedLinks)
        {
            doc->AddLink(link);
        }
    }

private:
    bool Apply(NodeGraphDocument& doc, const std::vector<float>& weights)
    {
        NodeGraphDocNode* node = doc.FindNode(m_NodeId);
        if (!node)
        {
            return false;
        }
        for (int i = 0; i < node->meta->propertyCount; ++i)
        {
            const Deki::PropertyInfo& p = node->meta->properties[i];
            if (m_Property == p.name && p.type == Deki::PropertyType::Array &&
                p.elementType == Deki::PropertyType::Float)
            {
                auto* vec = reinterpret_cast<std::vector<float>*>(static_cast<char*>(node->instance) + p.offset);
                *vec = weights;
                doc.dirty = true;
                return true;
            }
        }
        DEKI_LOG_ERROR("SetWeightCountCommand: property '%s' not found or not Array(Float)", m_Property.c_str());
        return false;
    }

    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    std::string m_Property;
    std::vector<float> m_Old, m_New;
    std::vector<NodeGraphDocLink> m_PrunedLinks;
};

/// Resizes a dynamic-outputs array property (any element type), removing the
/// links from dropped pins in the same undo step. Like SetWeightCountCommand,
/// for FSM-style transition lists whose pins are labelled by string values.
/// Old and new are whole JSON arrays.
class ResizeDynamicOutputsCommand : public Command
{
public:
    ResizeDynamicOutputsCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, std::string propertyName,
                                nlohmann::json oldValues, nlohmann::json newValues)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Property(std::move(propertyName)),
          m_Old(std::move(oldValues)),
          m_New(std::move(newValues))
    {
    }

    std::string GetDescription() const override
    {
        return m_New.size() > m_Old.size() ? "Add output pin" : "Remove output pin";
    }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "ResizeDynamicOutputs");
        if (!doc)
        {
            return;
        }
        if (!Apply(*doc, m_New))
        {
            return;
        }

        // Remove links leaving pins that no longer exist.
        m_PrunedLinks.clear();
        const int count = static_cast<int>(m_New.size());
        for (const auto& link : doc->AttachedLinks(m_NodeId))
        {
            if (link.fromNode == m_NodeId && link.fromPin >= count)
            {
                m_PrunedLinks.push_back(link);
                doc->RemoveLink(link);
            }
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "ResizeDynamicOutputs undo");
        if (!doc)
        {
            return;
        }
        if (!Apply(*doc, m_Old))
        {
            return;
        }
        for (const auto& link : m_PrunedLinks)
        {
            doc->AddLink(link);
        }
    }

private:
    bool Apply(NodeGraphDocument& doc, const nlohmann::json& values)
    {
        NodeGraphDocNode* node = doc.FindNode(m_NodeId);
        if (!node)
        {
            DEKI_LOG_ERROR("ResizeDynamicOutputsCommand: node %u no longer exists", m_NodeId);
            return false;
        }
        nlohmann::json wrapper;
        wrapper[m_Property] = values;
        if (!NodePropertiesFromJson(node->instance, *node->meta, wrapper))
        {
            return false;
        }
        doc.dirty = true;
        return true;
    }

    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    std::string m_Property;
    nlohmann::json m_Old, m_New;
    std::vector<NodeGraphDocLink> m_PrunedLinks;
};

/// Removes one entry from a dynamic-outputs array, by index.
///
/// ResizeDynamicOutputsCommand only adds or drops the last pin. Removing a pin
/// from the middle renumbers every pin after it, so the links leaving those
/// pins move down with them; otherwise they would point at the wrong
/// transition.
class RemoveDynamicOutputCommand : public Command
{
public:
    RemoveDynamicOutputCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, std::string propertyName,
                               int index, std::string label)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Property(std::move(propertyName)),
          m_Index(index),
          m_Label(std::move(label))
    {
    }

    std::string GetDescription() const override { return "Remove " + m_Label; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "RemoveDynamicOutput");
        if (!doc)
        {
            return;
        }
        NodeGraphDocNode* node = doc->FindNode(m_NodeId);
        if (!node)
        {
            return;
        }

        nlohmann::json values;
        if (!NodePropertiesToJson(node->instance, *node->meta, values) || !values.contains(m_Property) ||
            !values[m_Property].is_array())
        {
            return;
        }
        m_Old = values[m_Property];
        if (m_Index < 0 || m_Index >= static_cast<int>(m_Old.size()))
        {
            return;
        }

        nlohmann::json next = m_Old;
        next.erase(static_cast<size_t>(m_Index));
        if (!Apply(*doc, next))
        {
            return;
        }

        // Every link touching this node, captured so undo can put the
        // numbering back exactly.
        m_OldLinks = doc->AttachedLinks(m_NodeId);
        for (const auto& link : m_OldLinks)
        {
            if (link.fromNode != m_NodeId || link.fromPin < m_Index)
            {
                continue;  // inputs and pins before the removed one stay
            }
            doc->RemoveLink(link);
            if (link.fromPin > m_Index)
            {
                NodeGraphDocLink moved = link;
                moved.fromPin = link.fromPin - 1;  // one down, into the gap
                doc->AddLink(moved);
            }
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "RemoveDynamicOutput undo");
        if (!doc || m_Old.is_null())
        {
            return;
        }
        NodeGraphDocNode* node = doc->FindNode(m_NodeId);
        if (!node)
        {
            return;
        }

        // Drop the shifted links, then restore the originals: rebuilding from
        // the captured set is exact, shifting back one by one is not.
        for (const auto& link : doc->AttachedLinks(m_NodeId))
        {
            if (link.fromNode == m_NodeId)
            {
                doc->RemoveLink(link);
            }
        }
        Apply(*doc, m_Old);
        for (const auto& link : m_OldLinks)
        {
            doc->AddLink(link);
        }
    }

private:
    bool Apply(NodeGraphDocument& doc, const nlohmann::json& values)
    {
        NodeGraphDocNode* node = doc.FindNode(m_NodeId);
        if (!node)
        {
            return false;
        }
        nlohmann::json wrapper;
        wrapper[m_Property] = values;
        if (!NodePropertiesFromJson(node->instance, *node->meta, wrapper))
        {
            return false;
        }
        doc.dirty = true;
        return true;
    }

    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    std::string m_Property;
    int m_Index;
    std::string m_Label;
    nlohmann::json m_Old;
    std::vector<NodeGraphDocLink> m_OldLinks;
};

// ===========================================================================
// Child stack commands (DEKI_NODE_CHILDREN, such as FSM state action lists).
// Children are addressed by (nodeId, index); every command restores the index
// it acted on, so the order survives undo and redo.
// ===========================================================================

/// Appends one default child of a given type.
class AddChildCommand : public Command
{
public:
    AddChildCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, uint32_t childTypeId,
                    std::string childTypeName)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_ChildTypeId(childTypeId),
          m_ChildTypeName(std::move(childTypeName))
    {
    }

    std::string GetDescription() const override { return "Add " + m_ChildTypeName; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "AddChild");
        if (!doc)
        {
            return;
        }
        if (m_Index < 0)
        {
            // First run: append and remember the index.
            m_Index = doc->AddChild(m_NodeId, m_ChildTypeId, -1);
        }
        else
        {
            doc->AddChildFromJson(m_NodeId, m_Index, m_ChildTypeName, nlohmann::json::object(), true);
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "AddChild undo");
        if (doc && m_Index >= 0)
        {
            doc->RemoveChild(m_NodeId, m_Index);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    uint32_t m_ChildTypeId;
    std::string m_ChildTypeName;
    int m_Index = -1;
};

/// Removes one child; undo restores its type, values, enabled flag and index.
class RemoveChildCommand : public Command
{
public:
    RemoveChildCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, int index)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Index(index)
    {
    }

    std::string GetDescription() const override { return "Remove " + m_TypeName; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "RemoveChild");
        if (!doc)
        {
            return;
        }
        if (const NodeGraphDocNode* node = doc->FindNode(m_NodeId))
        {
            if (m_Index >= 0 && m_Index < static_cast<int>(node->children.size()))
            {
                const NodeGraphDocChild& c = node->children[m_Index];
                m_TypeName = c.meta->name;
                m_Enabled = c.enabled;
                NodePropertiesToJson(c.instance, *c.meta, m_Values);
            }
        }
        doc->RemoveChild(m_NodeId, m_Index);
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "RemoveChild undo");
        if (doc && !m_TypeName.empty())
        {
            doc->AddChildFromJson(m_NodeId, m_Index, m_TypeName, m_Values, m_Enabled);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    int m_Index;
    std::string m_TypeName;
    nlohmann::json m_Values;
    bool m_Enabled = true;
};

/// Moves one child within its stack.
class MoveChildCommand : public Command
{
public:
    MoveChildCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, int from, int to)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_From(from),
          m_To(to)
    {
    }

    std::string GetDescription() const override { return "Reorder"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "MoveChild");
        if (doc)
        {
            doc->MoveChild(m_NodeId, m_From, m_To);
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "MoveChild undo");
        if (doc)
        {
            doc->MoveChild(m_NodeId, m_To, m_From);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    int m_From, m_To;
};

/// Toggles one child's enabled flag.
class SetChildEnabledCommand : public Command
{
public:
    SetChildEnabledCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, int index, bool enabled)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Index(index),
          m_Enabled(enabled)
    {
    }

    std::string GetDescription() const override { return m_Enabled ? "Enable action" : "Disable action"; }

    void Execute() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "SetChildEnabled");
        if (doc)
        {
            doc->SetChildEnabled(m_NodeId, m_Index, m_Enabled);
        }
    }

    void Undo() override
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, "SetChildEnabled undo");
        if (doc)
        {
            doc->SetChildEnabled(m_NodeId, m_Index, !m_Enabled);
        }
    }

private:
    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    int m_Index;
    bool m_Enabled;
};

/// Sets one reflected property of a child, with old and new values as JSON.
class SetChildPropertyCommand : public Command
{
public:
    SetChildPropertyCommand(std::weak_ptr<NodeGraphDocument> doc, uint32_t nodeId, int index, std::string propertyName,
                            nlohmann::json oldValue, nlohmann::json newValue)
        : m_Doc(std::move(doc)),
          m_NodeId(nodeId),
          m_Index(index),
          m_Property(std::move(propertyName)),
          m_Old(std::move(oldValue)),
          m_New(std::move(newValue))
    {
    }

    std::string GetDescription() const override { return "Edit " + m_Property; }

    void Execute() override { Apply(m_New, "SetChildProperty"); }
    void Undo() override { Apply(m_Old, "SetChildProperty undo"); }

private:
    void Apply(const nlohmann::json& value, const char* what)
    {
        auto doc = NodeGraphCommandDetail::Lock(m_Doc, what);
        if (!doc)
        {
            return;
        }
        NodeGraphDocNode* node = doc->FindNode(m_NodeId);
        if (!node || m_Index < 0 || m_Index >= static_cast<int>(node->children.size()))
        {
            DEKI_LOG_ERROR("SetChildPropertyCommand: node %u child %d no longer exists", m_NodeId, m_Index);
            return;
        }
        NodeGraphDocChild& child = node->children[m_Index];
        nlohmann::json values;
        values[m_Property] = value;
        if (NodePropertiesFromJson(child.instance, *child.meta, values))
        {
            doc->dirty = true;
        }
    }

    std::weak_ptr<NodeGraphDocument> m_Doc;
    uint32_t m_NodeId;
    int m_Index;
    std::string m_Property;
    nlohmann::json m_Old, m_New;
};

}  // namespace DekiEditor
