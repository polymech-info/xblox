#ifndef NH_PARAMETER_OUT_H
#define NH_PARAMETER_OUT_H

#include "../commons.h"
#include "./Parameter.h"

/**************************************************************************
Name: CKParameterOut

Summary: Output parameter providing a value

Remarks:
        {Image:ParameterOut}

        + The type of the parameter defines the type of the data provided. These
types are maintained by the parameter manager. It defines the size of the buffer
to use, and also decides what can be plugged onto the output parameter. To have
the list and definition of predefined parameter types see CKParameterManager.

        + An output parameter may have destinations to which it pushes the data
each time it is changed. These destinations are other output parameters, which
for example provide their value out of the enclosing behavior, or local
parameters which provide values to other parts of the graph of sub-behaviors.
These destinations are managed using the AddDestination and related methods.
When the data of the output parameter changes, it pushes the new value down to
its destinations.

        + An output parameter will probably also be plugged into input
parameters. These input parameters will pull the value from the output parameter
when needed.

        + An output parameter usually knows how to write its data from and to a
string. This is useful for display and debugging purposes. When you define a new
type of parameter, you can specify the function that converts to and from
strings.

        + An output parameter can also have an edition window. When you define a
new type of parameter, you can specify the function that will create the edition
window when needed by the interface.

        + A NH_ParameterOut is created with CKBehavior::CreateOutputParameter or
CKContext::CreateCKParameterOut.

        + The class id of NH_ParameterOut is NHCID_PARAMETEROUT.


See also: NH_ParameterIn, NH_ParameterOperation
**********************************************************************************/

class NH_ParameterOut : public NH_Parameter {
public:
  NH_ParameterOut(int id, NH_CSTRING name) : NH_Parameter(id, name) {}
  virtual ~NH_ParameterOut() = default;

  virtual NH_CLASS_ID GetClassID() override { return NHCID_PARAMETEROUT; }

  //--------------------------------------------
  // Value

  virtual NH_ERROR GetValue(void *buf, NH_BOOL update = true);
  virtual NH_ERROR SetValue(const void *buf, int size = 0);
  virtual NH_ERROR CopyValue(NH_Parameter *param, NH_BOOL UpdateParam = true);
  virtual void *GetReadDataPtr(NH_BOOL update = true);
  virtual int GetStringValue(NH_STRING Value, NH_BOOL update = true);

  // void				CheckClass(CKParameterTypeDesc* iType);

  //--------------------------------------------
  // Destinations

  void DataChanged();
  NH_ERROR AddDestination(NH_Parameter *param, NH_BOOL CheckType = true);
  void RemoveDestination(NH_Parameter *param);
  int GetDestinationCount();
  NH_Parameter *GetDestination(int pos);
  void RemoveAllDestinations();
};

#endif