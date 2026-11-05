#pragma once

#include "../commons.h"
#include "../types.h"

class NH_ParameterManager;
class NH_ParameterIn;
class NH_ParameterOut;
struct Uuid64;

// ─────────────────────────────────────────────────────────────────────────────
// NH_Context  -  owns the ParameterManager and acts as factory for typed params.
// One context is created per XBlox execution session (or per plugin instance).
// ─────────────────────────────────────────────────────────────────────────────

class NH_Context {
public:
    NH_Context();
    virtual ~NH_Context();

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    NH_ERROR init();

    // ── Parameter manager ─────────────────────────────────────────────────────
    NH_ParameterManager* GetParameterManager()                   { return m_ParameterManager; }
    void                 SetParameterManager(NH_ParameterManager* pm) { m_ParameterManager = pm; }

    // Look up a registered type descriptor by GUID (nullptr if not found)
    const NH_ParameterTypeDesc* FindType(const Uuid64& guid) const;

    // ── Factory helpers ───────────────────────────────────────────────────────
    // Caller owns the returned object.
    NH_ParameterIn*  CreateParameterIn (NH_CSTRING name, const Uuid64& typeGuid);
    NH_ParameterOut* CreateParameterOut(NH_CSTRING name, const Uuid64& typeGuid);

protected:
    NH_ParameterManager* m_ParameterManager;
};
