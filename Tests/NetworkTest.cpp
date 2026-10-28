#include "NetworkTestPCH.h"

#include <TestFramework/Utilities/TestSetup.h>

PL_TESTFRAMEWORK_ENTRY_POINT_BEGIN("NetworkTest", "Networking Tests")
{
  plTestFramework::GetInstance()->SetTestTimeout(1000 * 60 * 5);
  plTestFramework::s_bCallstackOnAssert = true;
}
PL_TESTFRAMEWORK_ENTRY_POINT_END()
