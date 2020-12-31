#pragma once

void showConsoleWindow();
#define ASSERTION_FAILURE(causeString, expression, ...)	\
	do {												\
		print("%(%)\n\tIn function %\n\t%: %", __FILE__, __LINE__, __FUNCTION__, causeString, expression);			\
		showConsoleWindow();				            \
		DEBUG_BREAK;									\
		exit(-1);										\
	} while(0)

namespace TL {}
using namespace TL;

#include "../dep/tl/include/tl/console.h"
#include "base.h"

#if OS_WINDOWS
#include "os_windows.h"
#elif OS_LINUX
#include "os_linux.h"
#else
#error no os
#endif
