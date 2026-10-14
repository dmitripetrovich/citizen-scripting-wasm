/*
 * Copyright (c) 2023 Take-Two Interactive Software, Inc.
 *
 * This file is licensed under the GNU Library General Public License, version 2.0 (LGPL-2.0).
 *
 * The full license text is available at: LICENSES/LGPL-2.0.txt
 *
 * Original source: https://github.com/citizenfx/fivem/blob/master/code/components/citizen-scripting-{core,lua,mono-v2,mono,node,v8}/src/Component.cpp
 */

#include "WasmComponentHost.h"

class EXPORTED_TYPE ComponentInstance : public OMComponentBase<Component>
{
public:
        virtual bool Initialize();

        virtual bool DoGameLoad(void* module);

        virtual bool Shutdown();
};

bool ComponentInstance::Initialize()
{
        InitFunctionBase::RunAll();

        return true;
}

bool ComponentInstance::DoGameLoad(void* module)
{
        HookFunction::RunAll();

        return true;
}

bool ComponentInstance::Shutdown()
{
        return true;
}

extern "C" DLL_EXPORT Component* CreateComponent()
{
        return new ComponentInstance();
}

OMComponentBaseImpl* OMComponentBaseImpl::ms_instance;
