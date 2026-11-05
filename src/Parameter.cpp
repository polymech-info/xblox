#include "Parameter.h"
#include "Context.h"
#include "ParameterManager.h"
#include <cstring>

NH_Parameter::NH_Parameter(int id, NH_CSTRING name) : NH_Object(id, name) {
    m_Owner = nullptr;
    m_AllocatedSize = 0;
    m_Buffer = nullptr;
    m_InlineScalar = false;
    m_ParamType = nullptr;
}

void NH_Parameter::SetScalar(double v) {
    // Switching to inline scalar storage: release any heap buffer first so the
    // union no longer aliases a live pointer.
    if (!m_InlineScalar && m_Buffer) {
        delete[] m_Buffer;
        m_Buffer = nullptr;
    }
    m_InlineScalar = true;
    m_Scalar = v;
    m_DataSize = static_cast<int>(sizeof(double));
}

NH_PARAMETER_TYPE NH_Parameter::GetType() {
    return m_ParamType ? m_ParamType->Index : 0;
}

void NH_Parameter::SetType(NH_PARAMETER_TYPE typeIndex) {
    // Caller is responsible for keeping the ParameterTypeDesc alive.
    // To look up by index the owner context must be consulted;
    // for now we just clear the cached pointer so it is safe.
    m_ParamType = nullptr;
    (void)typeIndex; // resolved lazily by the caller via Context::FindType
}

NH_CLASS_ID NH_Parameter::GetParameterClassID() {
    return NHCID_PARAMETER;
}

int NH_Parameter::GetDataSize() {
    return m_DataSize;
}

NH_BOOL NH_Parameter::IsCompatibleWith(NH_Parameter* param) {
    if (!param) return false;
    if (!m_ParamType || !param->m_ParamType) return false;
    return m_ParamType->Guid == param->m_ParamType->Guid;
}

NH_Object* NH_Parameter::GetValueObject(NH_BOOL /*update*/) {
    return nullptr; // typed object retrieval not yet implemented
}

NH_Parameter::~NH_Parameter() {
    // Only free when the union actually holds a heap buffer; an inline scalar
    // reuses the same storage and must not be deleted.
    if (!m_InlineScalar && m_Buffer) {
        delete[] m_Buffer;
        m_Buffer = nullptr;
    }
}

NH_ERROR NH_Parameter::GetValue(void *buf, NH_BOOL update) {
    if (!buf || !m_Buffer) return E_FAIL;
    memcpy(buf, m_Buffer, m_DataSize);
    return E_OK;
}

NH_ERROR NH_Parameter::SetValue(const void *buf, int size) {
    if (!buf) return E_FAIL;
    
    int dataSize = size;
    if (dataSize == 0 && m_ParamType) {
        dataSize = m_ParamType->DefaultSize;
    }

    if (dataSize == 0) return E_FAIL;

    // Leaving inline-scalar mode: the union currently holds a double, not a
    // pointer, so reset before treating it as a heap buffer.
    if (m_InlineScalar) {
        m_InlineScalar = false;
        m_Buffer = nullptr;
        m_AllocatedSize = 0;
    }

    if (m_AllocatedSize < dataSize) {
        if (m_Buffer) {
            delete[] m_Buffer;
        }
        m_Buffer = new NH_BYTE[dataSize];
        m_AllocatedSize = dataSize;
    }

    memcpy(m_Buffer, buf, dataSize);
    m_DataSize = dataSize;

    return E_OK;
}

NH_ERROR NH_Parameter::CopyValue(NH_Parameter *param, NH_BOOL UpdateParam) {
  return E_NOTIMPL;
}

void *NH_Parameter::GetReadDataPtr(NH_BOOL update) {
    return m_Buffer;
}

void *NH_Parameter::GetWriteDataPtr() {
  return nullptr;
}

NH_ERROR NH_Parameter::SetStringValue(NH_STRING Value) {
    return E_NOTIMPL;
}

int NH_Parameter::GetStringValue(NH_STRING Value, NH_BOOL update) {
    return E_NOTIMPL;
}
