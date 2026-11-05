#ifndef NH_PARAMETER_IN_H
#define NH_PARAMETER_IN_H

#include "./Parameter.h"

// Forward declaration
class NH_ParameterIn;

class NH_ParameterIn : public NH_Parameter {
public:
  NH_ParameterIn(int id, NH_CSTRING name);
  virtual ~NH_ParameterIn() = default;

  // Source management
  NH_Parameter *GetRealSource();

  NH_Parameter *GetDirectSource();
  NH_ERROR SetDirectSource(NH_Parameter *param);

  NH_ParameterIn *GetSharedSource();
  NH_ERROR ShareSourceWith(NH_ParameterIn *param);

  // Overridden value accessors
  virtual NH_ERROR GetValue(void *buf, NH_BOOL update = true) override;
  virtual void *GetReadDataPtr(NH_BOOL update = true) override;

  // Disable writing
  virtual NH_ERROR SetValue(const void *buf, int size = 0) override;
  virtual NH_ERROR CopyValue(NH_Parameter *param, NH_BOOL UpdateParam = true) override;
  virtual void *GetWriteDataPtr() override;

  // String conversion
  virtual NH_ERROR SetStringValue(NH_STRING Value) override;
  virtual int GetStringValue(NH_STRING Value, NH_BOOL update = true) override;

  virtual NH_CLASS_ID GetClassID() override { return NHCID_PARAMETERIN; }

protected:
  union {
    NH_Parameter *m_DirectSource;
    NH_ParameterIn *m_SharedSource;
  };
};

#endif // NH_PARAMETER_IN_H