#ifndef NH_PARAMETER_H
#define NH_PARAMETER_H

#include "../commons.h"
#include "../types.h"
#include "./Object.h"

class NH_Parameter : public NH_Object {
public:
  NH_Parameter(int id, NH_CSTRING name);
  virtual ~NH_Parameter();

  NH_PARAMETER_TYPE GetType();
  void SetType(NH_PARAMETER_TYPE type);

  NH_CLASS_ID GetParameterClassID();
  virtual NH_CLASS_ID GetClassID() { return NHCID_PARAMETER; }

  void SetOwner(NH_Object *owner) { m_Owner = owner; }
  NH_Object *GetOwner() { return m_Owner; }

  NH_ParameterTypeDesc *GetParameterType() { return m_ParamType; }

  // Convertion from / to string
  virtual NH_ERROR SetStringValue(NH_STRING Value);
  virtual int GetStringValue(NH_STRING Value, NH_BOOL update = true);

  //--------------------------------------------
  // Value

  NH_Object *GetValueObject(NH_BOOL update = true);

  virtual NH_ERROR GetValue(void *buf, NH_BOOL update = true);
  virtual NH_ERROR SetValue(const void *buf, int size = 0);
  virtual NH_ERROR CopyValue(NH_Parameter *param, NH_BOOL UpdateParam = true);

  NH_BOOL IsCompatibleWith(NH_Parameter *param);
  //--------------------------------------------
  // Data pointer
  int GetDataSize();
  virtual void *GetReadDataPtr(NH_BOOL update = true);
  virtual void *GetWriteDataPtr();

  //--------------------------------------------
  // Inline scalar fast-path.
  //
  // For numeric scalar types we store the value directly in the inline union
  // (no heap) and hand out a stable address.  This is what muParser binds to
  // via SetVarFactory: GetScalarPtr() stays valid for the lifetime of the
  // parameter, so a cached double* never dangles.
  double  GetScalar() const          { return m_Scalar; }
  void    SetScalar(double v);                 // frees any heap buffer, switches to inline
  double *GetScalarPtr()             { return &m_Scalar; }
  bool    IsInlineScalar() const     { return m_InlineScalar; }

protected:
  NH_Object *m_Owner;

  union {
    int m_DataSize;
    int m_AllocatedSize;
  };
  // Inline value union  -  pointer-sized on x64, so a 64-bit double costs nothing
  // extra over the existing buffer pointer.  m_Scalar is the numeric fast-path
  // muParser binds to; m_Buffer is the heap path for variable-size types.
  union {
    NH_BYTE *m_Buffer;
    NH_DWORD m_Value;   // legacy 32-bit inline
    double   m_Scalar;  // 64-bit inline numeric (muParser SetVarFactory target)
    void    *m_Ptr;
  };

  // Storage discriminant: when true the union holds an inline scalar (m_Scalar)
  // and must NOT be treated as a heap buffer (no delete[] in dtor).
  bool m_InlineScalar = false;

  NH_ParameterTypeDesc *m_ParamType;
};

#endif