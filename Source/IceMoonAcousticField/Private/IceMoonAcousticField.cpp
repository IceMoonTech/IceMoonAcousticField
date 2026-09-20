// Copyright Epic Games, Inc. All Rights Reserved.

#include "IceMoonAcousticField.h"
#include "IMAcousticSpatialization.h"
#include "MetasoundFrontendModuleRegistrationMacros.h"

METASOUND_IMPLEMENT_MODULE_REGISTRATION_LIST

#define LOCTEXT_NAMESPACE "FIceMoonAcousticFieldModule"

void FIceMoonAcousticFieldModule::StartupModule()
{
	IM_RegisterAcousticSpatialization();
	METASOUND_REGISTER_ITEMS_IN_MODULE
}

void FIceMoonAcousticFieldModule::ShutdownModule()
{
	METASOUND_UNREGISTER_ITEMS_IN_MODULE
	IM_UnregisterAcousticSpatialization();
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FIceMoonAcousticFieldModule, IceMoonAcousticField)
