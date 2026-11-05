#pragma once

#include "../commons.h"
#include "string/uuid_generator.h"
#include <nlohmann/json.hpp>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// NH_Object  -  base for all XBlox named/typed objects (parameters, behaviors, ...)
// ─────────────────────────────────────────────────────────────────────────────

class NH_Object {
public:
    NH_Object()
        : m_ID(0)
        , m_BehaviorFlags(NHBEHAVIOR_NONE)
        , m_ObjectFlags(NH_OBJECT_DYNAMIC)
        , m_Parent(nullptr)
    {}

    NH_Object(int id, NH_CSTRING name, NH_CSTRING typeName = nullptr)
        : NH_Object()
    {
        m_ID = id;
        if (name)     m_Name     = name;
        if (typeName) m_TypeName = typeName;
    }

    virtual ~NH_Object() = default;

    // ── Identity ─────────────────────────────────────────────────────────────
    int         GetID()   const { return m_ID; }
    void        SetID(int id)   { m_ID = id; }

    NH_CSTRING        GetName()       const { return m_Name.c_str(); }
    const std::string& GetNameString() const { return m_Name; }
    void SetName(NH_CSTRING name) { m_Name = name ? name : ""; }

    NH_CSTRING GetTypeName() const { return m_TypeName.c_str(); }
    void SetTypeName(NH_CSTRING tn) { m_TypeName = tn ? tn : ""; }

    Uuid64 GetUUID() const { return m_UUID; }
    void   SetUUID(Uuid64 uuid) { m_UUID = uuid; }

    // ── Hierarchy ────────────────────────────────────────────────────────────
    NH_Object* GetParent() const           { return m_Parent; }
    void       SetParent(NH_Object* parent) { m_Parent = parent; }

    // ── Flags ────────────────────────────────────────────────────────────────
    NH_BEHAVIOR_FLAGS GetBehaviorFlags() const { return m_BehaviorFlags; }
    void SetBehaviorFlags(NH_BEHAVIOR_FLAGS f)  { m_BehaviorFlags = f; }
    void AddBehaviorFlags(NH_BEHAVIOR_FLAGS f) {
        m_BehaviorFlags = static_cast<NH_BEHAVIOR_FLAGS>(
            static_cast<int>(m_BehaviorFlags) | static_cast<int>(f));
    }
    void RemoveBehaviorFlags(NH_BEHAVIOR_FLAGS f) {
        m_BehaviorFlags = static_cast<NH_BEHAVIOR_FLAGS>(
            static_cast<int>(m_BehaviorFlags) & ~static_cast<int>(f));
    }

    NH_OBJECT_FLAGS GetObjectFlags() const {
        return static_cast<NH_OBJECT_FLAGS>(m_ObjectFlags);
    }
    void SetObjectFlags(NH_OBJECT_FLAGS f)   { m_ObjectFlags = static_cast<NH_DWORD>(f); }
    void AddObjectFlags(NH_OBJECT_FLAGS f)   { m_ObjectFlags |=  static_cast<NH_DWORD>(f); }
    void ClearObjectFlags(NH_OBJECT_FLAGS f) { m_ObjectFlags &= ~static_cast<NH_DWORD>(f); }

    // ── Serialisation ────────────────────────────────────────────────────────
    // Blocks / parameters override these to persist their state to/from JSON.
    virtual void SaveState(nlohmann::json& out)       {}
    virtual void LoadState(const nlohmann::json& in)  {}

    // ── Class identification ──────────────────────────────────────────────────
    virtual NH_CLASS_ID GetClassID() { return NHCID_OBJECT; }
    static  NH_STRING   m_ClassName;

protected:
    int               m_ID;
    std::string       m_Name;
    std::string       m_TypeName;
    Uuid64            m_UUID;
    NH_Object*        m_Parent;
    NH_BEHAVIOR_FLAGS m_BehaviorFlags;
    NH_DWORD          m_ObjectFlags;
    NH_CLASS_ID       m_ClassID = NHCID_OBJECT;
};
