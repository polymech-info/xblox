#include "Context.h"
#include "ParameterManager.h"
#include "ParameterIn.h"
#include "ParameterOut.h"
#include "string/uuid_generator.h"
#include "logging/logger.h"

NH_Context::NH_Context() : m_ParameterManager(nullptr) {}

NH_Context::~NH_Context()
{
    delete m_ParameterManager;
    m_ParameterManager = nullptr;
}

NH_ERROR NH_Context::init()
{
    m_ParameterManager = new NH_ParameterManager(this);
    const NH_ERROR err = m_ParameterManager->OnInit();
    if (err == E_OK)
        // debug, not info: a fresh context is built per run pass (and a looping
        // document rebuilds one each pass), so info-level here floods the log.
        logger::debug("[NH_Context] initialized (" +
                      std::to_string(m_ParameterManager->GetTypeCount()) +
                      " types registered)");
    else
        logger::warn("[NH_Context] init failed (error " + std::to_string(err) + ")");
    return err;
}

const NH_ParameterTypeDesc* NH_Context::FindType(const Uuid64& guid) const
{
    if (!m_ParameterManager) return nullptr;
    return m_ParameterManager->FindType(guid);
}

NH_ParameterIn* NH_Context::CreateParameterIn(NH_CSTRING name, const Uuid64& typeGuid)
{
    auto* param = new NH_ParameterIn(0, name);
    if (m_ParameterManager) {
        const NH_ParameterTypeDesc* td = m_ParameterManager->FindType(typeGuid);
        if (td) param->SetType(td->Index);
    }
    return param;
}

NH_ParameterOut* NH_Context::CreateParameterOut(NH_CSTRING name, const Uuid64& typeGuid)
{
    auto* param = new NH_ParameterOut(0, name);
    if (m_ParameterManager) {
        const NH_ParameterTypeDesc* td = m_ParameterManager->FindType(typeGuid);
        if (td) param->SetType(td->Index);
    }
    return param;
}
