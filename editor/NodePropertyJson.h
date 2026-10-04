#pragma once

// Converts a node instance's properties to and from JSON, driven by
// Deki::PropertyInfo.
//
// Node instances are plain reflected structs behind void* (made by
// NodeFactory, no common base), so PropertyEditorFactory, which takes a
// Deki::Component*, cannot be used. This follows the scene component JSON code
// for the types nodes use. The encodings are what the generated
// DeserializeMsgPack expects after json::to_msgpack: numbers for ints and
// enums, JSON arrays for arrays, plain strings and bools.
//
// A property type nodes do not support (Color, Vector2/3, ObjectRef, arrays of
// anything but float, int32 or string) is an error: it is logged with the node
// type and property, the function returns false, and the caller stops.

#include <nlohmann/json.hpp>

namespace DekiNodeGraph
{
struct DekiNodeMeta;
}
namespace DekiEditor
{

/// Writes every reflected property of `instance` into `outValues` (an object).
bool NodePropertiesToJson(const void* instance, const DekiNodeGraph::DekiNodeMeta& meta, nlohmann::json& outValues);

/// Applies `values` to `instance`. Properties missing from `values` keep their
/// defaults; keys with no matching property are logged and skipped, as the
/// msgpack reader does, so newer files still load.
bool NodePropertiesFromJson(void* instance, const DekiNodeGraph::DekiNodeMeta& meta, const nlohmann::json& values);

}  // namespace DekiEditor
