#ifndef NH_PARAMETER_MANAGER_H
#define NH_PARAMETER_MANAGER_H

#include "../commons.h"
#include "../enums.h"
#include "../types.h"
#include "./BaseManager.h"
#include "./Parameter.h"
#include <map>
#include <vector>

class NH_ParameterManager : public NH_BaseManager {
public:
  NH_ParameterManager(NH_Context *context);
  virtual ~NH_ParameterManager() = default;

  virtual NH_ERROR OnInit() override;

  NH_ERROR RegisterParameterType(NH_ParameterTypeDesc *parameterType);

  // Lookup by stable GUID  -  returns nullptr if not found
  const NH_ParameterTypeDesc* FindType(const Uuid64& guid) const {
    auto it = m_ParameterGuids.find(guid);
    if (it == m_ParameterGuids.end()) return nullptr;
    return &m_ParameterTypes[static_cast<size_t>(it->second)];
  }

  int GetTypeCount() const { return static_cast<int>(m_ParameterTypes.size()); }

protected:
  std::vector<NH_ParameterTypeDesc> m_ParameterTypes;
  std::map<Uuid64, int> m_ParameterGuids;
};

#endif
