#include "ParameterOut.h"

NH_ERROR NH_ParameterOut::GetValue(void* buf, NH_BOOL update) {
    return NH_Parameter::GetValue(buf, update);
}

NH_ERROR NH_ParameterOut::SetValue(const void* buf, int size) {
    return NH_Parameter::SetValue(buf, size);
}

NH_ERROR NH_ParameterOut::CopyValue(NH_Parameter* param, NH_BOOL UpdateParam) {
    return NH_Parameter::CopyValue(param, UpdateParam);
}

void* NH_ParameterOut::GetReadDataPtr(NH_BOOL update) {
    return NH_Parameter::GetReadDataPtr(update);
}

int NH_ParameterOut::GetStringValue(NH_STRING Value, NH_BOOL update) {
    return NH_Parameter::GetStringValue(Value, update);
}

void NH_ParameterOut::DataChanged() {}

NH_ERROR NH_ParameterOut::AddDestination(NH_Parameter* param, NH_BOOL CheckType) {
    return E_NOTIMPL;
}

void NH_ParameterOut::RemoveDestination(NH_Parameter* param) {}

int NH_ParameterOut::GetDestinationCount() {
    return 0;
}

NH_Parameter* NH_ParameterOut::GetDestination(int pos) {
    return nullptr;
}

void NH_ParameterOut::RemoveAllDestinations() {}
