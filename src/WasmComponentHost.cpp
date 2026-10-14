#include "WasmComponentHost.h"

extern "C" intptr_t CoreFxFindFirstImpl(const guid_t& iid, guid_t* clsid);
extern "C" int32_t CoreFxFindNextImpl(intptr_t handle, guid_t* clsid);
extern "C" void CoreFxFindImplClose(intptr_t handle);
extern "C" result_t CoreFxCreateObjectInstance(const guid_t& guid, const guid_t& iid, void** objectRef);

extern "C" intptr_t fxFindFirstImpl(const guid_t& iid, guid_t* clsid)
{
        return CoreFxFindFirstImpl(iid, clsid);
}

extern "C" int32_t fxFindNextImpl(intptr_t handle, guid_t* clsid)
{
        return CoreFxFindNextImpl(handle, clsid);
}

extern "C" void fxFindImplClose(intptr_t handle)
{
        CoreFxFindImplClose(handle);
}

extern "C" result_t fxCreateObjectInstance(const guid_t& guid, const guid_t& iid, void** objectRef)
{
        return CoreFxCreateObjectInstance(guid, iid, objectRef);
}

OMFactoryDef* OMFactoryDef::s_factories = nullptr;
OMImplementsDef* OMImplementsDef::s_impls = nullptr;
