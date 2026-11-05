#pragma once
#include "../commons.h"

class NH_Context;

// Minimal base for all XBlox managers.
// Holds a back-pointer to the owning context and exposes an OnInit lifecycle hook.
class NH_BaseManager {
public:
    NH_BaseManager() : m_Context(nullptr) {}
    virtual ~NH_BaseManager() = default;

    virtual NH_ERROR OnInit() { return E_OK; }

    void         SetContext(NH_Context* ctx) { m_Context = ctx; }
    NH_Context*  GetContext() const          { return m_Context; }

protected:
    NH_Context* m_Context;
};
