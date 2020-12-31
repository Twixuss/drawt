#pragma once
#include "base.h"
#include "../dep/tl/include/tl/string.h"
#include <codecvt>
#include <locale>
#include <string>

#include <fcntl.h>
#include <io.h>

#include <chrono>

using namespace TL;

enum class MessageBoxType {
	warning
};

using Key = u8;
enum : Key {
	Key_tab     = 0x09,
	Key_shift   = 0x10,
	Key_control = 0x11,
	Key_alt     = 0x12,
	Key_escape  = 0x1B,
	Key_f1      = 0x70,
	Key_f2      = 0x71,
	Key_f3      = 0x72,
	Key_f4      = 0x73,
	Key_f5      = 0x74,
	Key_f6      = 0x75,
	Key_f7      = 0x76,
	Key_f8      = 0x77,
	Key_f9      = 0x78,
	Key_f10     = 0x79,
	Key_f11     = 0x7A,
	Key_f12     = 0x7B,
};

constexpr u8 mouseButton_left   = 0;
constexpr u8 mouseButton_right  = 1;
constexpr u8 mouseButton_middle = 2;

extern std::wstring executableDirectory;
extern FILE *logFile;

extern bool running;
extern bool needRepaint;
extern bool windowResized;
extern bool lostFocus;
extern bool exitSizeMove;
extern bool windowSizeDirty;
extern bool updateWindowText;
extern v2u clientSize;
extern u32 toolPanelWidth;
extern u32 minWindowDim;
extern u32 maxWindowDim;
extern v2s windowPos;
extern v2s mousePosTL, mousePosBL, prevMousePos;
extern v2f mouseScenePos;
extern v2s mouseDelta;
extern s32 mouseWheel;
extern bool keysHeld[256];
extern bool keysDown[256];
extern bool keysDownRep[256];
extern bool keysUp[256];
extern bool mouseButtons[8];
extern bool prevMouseButtons[8];
extern bool resetKeys;
extern bool consoleIsVisible;

extern std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t> utf16ToUtf8Converter;
extern Mutex utf16ToUtf8ConverterMutex;

void platform_init();
void platform_deinit();
void createWindow();

void hideConsoleWindow();
void showConsoleWindow();
void toggleConsoleWindow();

void showCursor();
void hideCursor();
void setCursorPos(v2s p);

v2s getMousePos();

void setWindowTitle(wchar const *title);

void platform_saveScene(Scene* scene, wchar const* path);
void saveSceneDialog(Scene * scene);
bool openSceneDialog(Scene *scene);

void platform_log(char const *str);
void platform_log(wchar const *str);

bool platform_messageBox(wchar const *text, wchar const *caption, MessageBoxType type);
void platform_emergencySave(bool const *unsavedScenes);

void platform_beginFrame();

void app_exitAbnormal();
void app_onWindowClose();
void app_onWindowResize();
void app_onWindowMove();
void app_onDragAndDrop(Span<Span<wchar>> paths);

void app_onKeyDown(u8);
void app_onKeyDownRepeated(u8);
void app_onKeyUp(u8);
void app_onMouseDown(u8);
void app_onMouseUp(u8);

bool app_tryExit();

StringBuilder<> app_writeScene(Scene *scene);
bool app_loadScene(Scene *scene, wchar const *path);

#define LOG(fmt, ...)							\
	do {										\
		StringBuilder<char> builder;            \
		builder.appendFormat(fmt, __VA_ARGS__); \
		builder.append(Span("\n\0", 2));        \
		auto buf = builder.get();               \
		platform_log(buf.data());				\
	} while(0)

#define LOGW(fmt, ...)	                        \
	do {										\
		StringBuilder<wchar> builder;           \
		builder.appendFormat(fmt, __VA_ARGS__); \
		builder.append(Span(L"\n\0", 2));       \
		auto buf = builder.get();               \
		platform_log(buf.data());				\
	} while(0)

#define TIMED_BLOCK_(name, line)								\
	char const *CONCAT(__timer_name_, line) = name;				\
	auto CONCAT(__timer_begin_, line) = std::chrono::high_resolution_clock::now();					\
	DEFER {														\
		auto CONCAT(__timer_end_, line) = std::chrono::high_resolution_clock::now();				\
		LOG("%: % ms", CONCAT(__timer_name_, line), (f64)(CONCAT(__timer_end_, line) - CONCAT(__timer_begin_, line)).count() / 1000000);	\
	}

#define TIMED_BLOCK_FMT_(format, line, ...)								\
	StringBuilder<> CONCAT(__timer_name_, line);				\
	CONCAT(__timer_name_, line).appendFormat(format, __VA_ARGS__); \
	auto CONCAT(__timer_begin_, line) = std::chrono::high_resolution_clock::now();					\
	DEFER {														\
		auto CONCAT(__timer_end_, line) = std::chrono::high_resolution_clock::now();				\
		LOG("%: % ms", CONCAT(__timer_name_, line).get(), (f64)(CONCAT(__timer_end_, line) - CONCAT(__timer_begin_, line)).count() / 1000000);	\
	}

#define TIMED_BLOCK(name) TIMED_BLOCK_(name, __LINE__)
#define TIMED_BLOCK_FMT(format, ...) TIMED_BLOCK_FMT_(format, __LINE__, __VA_ARGS__)
#define TIMED_FUNCTION TIMED_BLOCK(__FUNCTION__)

#define DHR(hr) 											\
	do {													\
		HRESULT _hr = hr;									\
		if (FAILED(_hr)) {									\
			char hrValueStr[16];							\
			sprintf(hrValueStr, "0x%x", _hr);				\
			LOG("BAD HRESULT: %", #hr);					\
			LOG("VALUE: %", hrValueStr);					\
			DEBUG_BREAK;									\
			MessageBoxA(0, "BAD HRESULT", hrValueStr, 0);	\
			exit(-1);										\
		}													\
	} while (0)
