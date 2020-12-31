#pragma once

#define LOG(fmt, ...)											\
	do {														\
		char buf[1024];											\
		sprintf_s(buf, countof(buf), fmt "\n", __VA_ARGS__);	\
		fprintf(logFile, buf);									\
		printf("%s", buf);										\
	} while(0)

#define LOGW(fmt, ...)													\
	do {																\
		wchar buf[1024];												\
		swprintf_s(buf, countof(buf), fmt L"\n", __VA_ARGS__);			\
		fprintf(logFile, utf16ToUtf8Converter.to_bytes(buf).data());	\
		wprintf(L"%s", buf);											\
	} while(0)