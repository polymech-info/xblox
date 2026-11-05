#include "ParameterManager.h"
#include <string>

NH_ParameterManager::NH_ParameterManager(NH_Context *context) : NH_BaseManager() {
  SetContext(context);
}

NH_ERROR NH_ParameterManager::OnInit() {
  auto reg = [&](Uuid64 guid, const char* name, int defaultSize,
                 NH_DWORD flags = 0) {
    NH_ParameterTypeDesc d;
    d.Guid        = guid;
    d.TypeName    = name;
    d.DefaultSize = defaultSize;
    d.dwFlags     = flags;
    RegisterParameterType(&d);
  };

  // ── Primitives ────────────────────────────────────────────────────────────
  reg(NHP_GUID_FLOAT,  "Float",  sizeof(float));
  reg(NHP_GUID_INT,    "Int",    sizeof(int));
  reg(NHP_GUID_BOOL,   "Bool",   sizeof(bool));
  reg(NHP_GUID_STRING, "String", sizeof(char*),
      NHPARAMETERTYPE_VARIABLESIZE);
  reg(NHP_GUID_VECTOR, "Vector", sizeof(float) * 3);

  // ── XBlox domain types ────────────────────────────────────────────────────
  // All string-backed -> VARIABLESIZE
  constexpr NH_DWORD VS = NHPARAMETERTYPE_VARIABLESIZE;
  reg(NHP_GUID_FILE_PATH,   "FilePath",   sizeof(char*), VS);
  reg(NHP_GUID_DIR_PATH,    "DirPath",    sizeof(char*), VS);
  reg(NHP_GUID_EXPRESSION,  "Expression", sizeof(char*), VS);
  reg(NHP_GUID_JSON,        "Json",       sizeof(char*), VS);
  reg(NHP_GUID_PROMPT,      "Prompt",     sizeof(char*), VS);
  reg(NHP_GUID_DEVICE_NAME, "DeviceName", sizeof(char*), VS);
  reg(NHP_GUID_IMAGE_REF,   "ImageRef",   sizeof(char*), VS);
  reg(NHP_GUID_AUDIO_REF,   "AudioRef",   sizeof(char*), VS);
  reg(NHP_GUID_VIDEO_REF,   "VideoRef",   sizeof(char*), VS);
  // Integer-backed
  reg(NHP_GUID_DURATION_MS, "DurationMs", sizeof(int));

  return E_OK;
}

NH_ERROR
NH_ParameterManager::RegisterParameterType(NH_ParameterTypeDesc *parameterType) {
  if (!parameterType)
    return E_FAIL;

  // Check if GUID is valid
  if (!parameterType->Guid.IsValid())
    return E_FAIL;

  // Check if GUID already exists
  if (m_ParameterGuids.find(parameterType->Guid) != m_ParameterGuids.end()) {
    return E_OK; // Or an error for duplicate
  }

  m_ParameterTypes.push_back(*parameterType);
  int newIndex = m_ParameterTypes.size() - 1;
  m_ParameterTypes.back().Index = newIndex;
  m_ParameterGuids[parameterType->Guid] = newIndex;

  return E_OK;
}