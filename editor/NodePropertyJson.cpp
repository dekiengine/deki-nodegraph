#include "deki-nodegraph/editor/NodePropertyJson.h"

#include "deki-nodegraph/DekiNode.h"
#include <deki/reflection/NodeRef.h>
#include <deki/reflection/PropertyRef.h>
#include <deki/assets/AssetRef.h>
#include <deki/LogSystem.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace DekiEditor
{

namespace
{
    using nlohmann::json;

    inline void* FieldPtr(void* base, const Deki::PropertyInfo& p)
    {
        return static_cast<char*>(base) + p.offset;
    }
    inline const void* FieldPtr(const void* base, const Deki::PropertyInfo& p)
    {
        return static_cast<const char*>(base) + p.offset;
    }

    // Enum values live in 1/2/4-byte storage (enumSize); move through int64.
    int64_t ReadEnumValue(const void* field, uint8_t enumSize)
    {
        switch (enumSize)
        {
            case 1: { uint8_t v;  std::memcpy(&v, field, 1); return v; }
            case 2: { uint16_t v; std::memcpy(&v, field, 2); return v; }
            default: { uint32_t v; std::memcpy(&v, field, 4); return v; }
        }
    }
    void WriteEnumValue(void* field, uint8_t enumSize, int64_t value)
    {
        switch (enumSize)
        {
            case 1: { uint8_t v = static_cast<uint8_t>(value);  std::memcpy(field, &v, 1); break; }
            case 2: { uint16_t v = static_cast<uint16_t>(value); std::memcpy(field, &v, 2); break; }
            default: { uint32_t v = static_cast<uint32_t>(value); std::memcpy(field, &v, 4); break; }
        }
    }

    bool IsSupportedType(const Deki::PropertyInfo& p)
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
            case Deki::PropertyType::NodeRef:
            case Deki::PropertyType::PropertyRef:
            case Deki::PropertyType::AssetRef:
                return true;
            case Deki::PropertyType::Array:
                return p.elementType == Deki::PropertyType::Float ||
                       p.elementType == Deki::PropertyType::Int32 ||
                       p.elementType == Deki::PropertyType::String;
            default:
                return false;
        }
    }

    void LogUnsupported(const DekiNodeMeta& meta, const Deki::PropertyInfo& p)
    {
        DEKI_LOG_ERROR("NodePropertyJson: node type '%s' property '%s' has unsupported "
                       "property type %d for node graphs",
                       meta.name ? meta.name : "?", p.name ? p.name : "?",
                       static_cast<int>(p.type));
    }
}

bool NodePropertiesToJson(const void* instance, const DekiNodeMeta& meta,
                          nlohmann::json& outValues)
{
    outValues = json::object();
    if (!instance)
        return false;

    for (int i = 0; i < meta.propertyCount; ++i)
    {
        const Deki::PropertyInfo& p = meta.properties[i];
        if (!IsSupportedType(p))
        {
            LogUnsupported(meta, p);
            return false;
        }

        const void* field = FieldPtr(instance, p);
        switch (p.type)
        {
            case Deki::PropertyType::Int8:   { int8_t v;   std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::Int16:  { int16_t v;  std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::Int32:  { int32_t v;  std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::Int64:  { int64_t v;  std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::UInt8:  { uint8_t v;  std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::UInt16: { uint16_t v; std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::UInt32: { uint32_t v; std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::UInt64: { uint64_t v; std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::Float:  { float v;    std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::Double: { double v;   std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::Bool:   { bool v;     std::memcpy(&v, field, sizeof v); outValues[p.name] = v; break; }
            case Deki::PropertyType::String:
                outValues[p.name] = *static_cast<const std::string*>(field);
                break;
            case Deki::PropertyType::Enum:
                outValues[p.name] = ReadEnumValue(field, p.enumSize);
                break;
            case Deki::PropertyType::NodeRef:
                outValues[p.name] = static_cast<const Deki::NodeRef*>(field)->targetId;
                break;
            case Deki::PropertyType::PropertyRef:
            {
                // Authored form only (object/component/field); the ids are
                // derived by Rehash on the way back in, never serialized.
                const auto* ref = static_cast<const Deki::PropertyRef*>(field);
                json obj = json::object();
                obj["object"] = ref->object;
                obj["component"] = ref->component;
                obj["field"] = ref->field;
                outValues[p.name] = std::move(obj);
                break;
            }
            case Deki::PropertyType::AssetRef:
            {
                // Same {guid, source} shape components use.
                const auto* ref = static_cast<const Deki::AssetRefBase*>(field);
                json obj = json::object();
                obj["guid"] = ref->guid;
                obj["source"] = ref->source;
                outValues[p.name] = std::move(obj);
                break;
            }
            case Deki::PropertyType::Array:
                switch (p.elementType)
                {
                    case Deki::PropertyType::Float:
                        outValues[p.name] = *static_cast<const std::vector<float>*>(field);
                        break;
                    case Deki::PropertyType::Int32:
                        outValues[p.name] = *static_cast<const std::vector<int32_t>*>(field);
                        break;
                    default:
                        outValues[p.name] = *static_cast<const std::vector<std::string>*>(field);
                        break;
                }
                break;
            default:
                break;   // unreachable (IsSupportedType)
        }
    }
    return true;
}

bool NodePropertiesFromJson(void* instance, const DekiNodeMeta& meta,
                            const nlohmann::json& values)
{
    if (!instance || !values.is_object())
        return false;

    for (int i = 0; i < meta.propertyCount; ++i)
    {
        const Deki::PropertyInfo& p = meta.properties[i];
        auto it = values.find(p.name);
        if (it == values.end())
            continue;   // absent -> keep default

        if (!IsSupportedType(p))
        {
            LogUnsupported(meta, p);
            return false;
        }

        void* field = FieldPtr(instance, p);
        try
        {
            switch (p.type)
            {
                case Deki::PropertyType::Int8:   { int8_t v = it->get<int8_t>();     std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::Int16:  { int16_t v = it->get<int16_t>();   std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::Int32:  { int32_t v = it->get<int32_t>();   std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::Int64:  { int64_t v = it->get<int64_t>();   std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::UInt8:  { uint8_t v = it->get<uint8_t>();   std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::UInt16: { uint16_t v = it->get<uint16_t>(); std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::UInt32: { uint32_t v = it->get<uint32_t>(); std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::UInt64: { uint64_t v = it->get<uint64_t>(); std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::Float:  { float v = it->get<float>();       std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::Double: { double v = it->get<double>();     std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::Bool:   { bool v = it->get<bool>();         std::memcpy(field, &v, sizeof v); break; }
                case Deki::PropertyType::String:
                    *static_cast<std::string*>(field) = it->get<std::string>();
                    break;
                case Deki::PropertyType::Enum:
                    WriteEnumValue(field, p.enumSize, it->get<int64_t>());
                    break;
                case Deki::PropertyType::NodeRef:
                    static_cast<Deki::NodeRef*>(field)->targetId = it->get<uint32_t>();
                    break;
                case Deki::PropertyType::PropertyRef:
                {
                    auto* ref = static_cast<Deki::PropertyRef*>(field);
                    ref->object    = it->value("object", std::string());
                    ref->component = it->value("component", std::string());
                    ref->field     = it->value("field", std::string());
                    ref->Rehash();
                    break;
                }
                case Deki::PropertyType::AssetRef:
                {
                    auto* ref = static_cast<Deki::AssetRefBase*>(field);
                    ref->guid   = it->value("guid", std::string());
                    ref->source = it->value("source", std::string());
                    ref->ptr = nullptr;            // re-resolved on next Get()
                    ref->loadAttempted = false;
                    break;
                }
                case Deki::PropertyType::Array:
                    switch (p.elementType)
                    {
                        case Deki::PropertyType::Float:
                            *static_cast<std::vector<float>*>(field) = it->get<std::vector<float>>();
                            break;
                        case Deki::PropertyType::Int32:
                            *static_cast<std::vector<int32_t>*>(field) = it->get<std::vector<int32_t>>();
                            break;
                        default:
                            *static_cast<std::vector<std::string>*>(field) = it->get<std::vector<std::string>>();
                            break;
                    }
                    break;
                default:
                    break;   // unreachable (IsSupportedType)
            }
        }
        catch (const json::exception& e)
        {
            DEKI_LOG_ERROR("NodePropertyJson: node type '%s' property '%s': bad JSON value (%s)",
                           meta.name ? meta.name : "?", p.name ? p.name : "?", e.what());
            return false;
        }
    }

    // Keys with no matching property: forward-compat skip, but say so.
    for (auto it = values.begin(); it != values.end(); ++it)
    {
        bool known = false;
        for (int i = 0; i < meta.propertyCount && !known; ++i)
            known = it.key() == meta.properties[i].name;
        if (!known)
            DEKI_LOG_WARNING("NodePropertyJson: node type '%s' has no property '%s' (skipped)",
                             meta.name ? meta.name : "?", it.key().c_str());
    }
    return true;
}

} // namespace DekiEditor
