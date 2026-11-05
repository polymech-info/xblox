#include "./ParameterIn.h"
#include "./Context.h"
#include "ParameterManager.h"

NH_ParameterIn::NH_ParameterIn(int id, NH_CSTRING name) : NH_Parameter(id, name) {
    m_ObjectFlags &= ~NH_PARAMETERIN_SHARED;
    m_DirectSource = nullptr;
}

NH_Parameter* NH_ParameterIn::GetRealSource() {
    if (m_ObjectFlags & NH_PARAMETERIN_SHARED) {
        if (m_SharedSource) return m_SharedSource->GetRealSource();
    } else {
        return m_DirectSource;
    }
    return nullptr;
}

NH_Parameter* NH_ParameterIn::GetDirectSource() {
    if (m_ObjectFlags & NH_PARAMETERIN_SHARED) return nullptr;
    return m_DirectSource;
}

NH_ERROR NH_ParameterIn::SetDirectSource(NH_Parameter* param) {
    m_ObjectFlags &= ~NH_PARAMETERIN_SHARED;
    m_DirectSource = param;
    return E_OK;
}

NH_ParameterIn* NH_ParameterIn::GetSharedSource() {
    if (!(m_ObjectFlags & NH_PARAMETERIN_SHARED)) return nullptr;
    return m_SharedSource;
}

NH_ERROR NH_ParameterIn::ShareSourceWith(NH_ParameterIn* param) {
    m_ObjectFlags |= NH_PARAMETERIN_SHARED;
    m_SharedSource = param;
    return E_OK;
}

NH_ERROR NH_ParameterIn::GetValue(void* buf, NH_BOOL update) {
    // Stub implementation
    NH_Parameter* src = GetRealSource();
    if (!src) return E_FAIL;
    return src->GetValue(buf, update);
}

void* NH_ParameterIn::GetReadDataPtr(NH_BOOL update) {
    // Stub implementation
    NH_Parameter* src = GetRealSource();
    if (!src) return nullptr;
    return src->GetReadDataPtr(update);
}

NH_ERROR NH_ParameterIn::SetValue(const void* buf, int size) {
    // Input parameters are read-only
    return E_ACCESSDENIED;
}

void* NH_ParameterIn::GetWriteDataPtr() {
    // Input parameters are read-only
    return nullptr;
}

NH_ERROR NH_ParameterIn::CopyValue(NH_Parameter* param, NH_BOOL UpdateParam) {
    NH_Parameter* src = GetRealSource();
    if (!src || !param) return E_FAIL;
    return src->CopyValue(param, UpdateParam);
}

NH_ERROR NH_ParameterIn::SetStringValue(NH_STRING Value) {
    NH_Parameter* src = GetRealSource();
    if (!src) return E_FAIL;
    return src->SetStringValue(Value);
}

int NH_ParameterIn::GetStringValue(NH_STRING Value, NH_BOOL update) {
    NH_Parameter* src = GetRealSource();
    if (!src) return E_FAIL;
    return src->GetStringValue(Value, update);
}
