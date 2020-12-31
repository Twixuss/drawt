#define TL_IMPL
#include "platform.cpp"
#include "os_windows.h"
#include "resource.h"

#include "../dep/tl/include/tl/console.h"
#include "../dep/tl/include/tl/file.h"
#include "../dep/tl/include/tl/debug.h"
#include "../dep/tl/include/tl/d3d11.h"

#define NOMINMAX
#include <Windows.h>
#include <Windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#pragma push_macro("OS_WINDOWS")
#undef OS_WINDOWS
#include <Shlwapi.h>
#pragma pop_macro("OS_WINDOWS")
#include <mmsystem.h>

#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "winmm")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shlwapi")
#pragma comment(lib, "ws2_32")

HWND consoleWindow;
std::wstring executableDirectory;
FILE *logFile;

HWND mainWindow;
bool running = true;
bool needRepaint = true;
bool windowResized;
bool lostFocus;
bool exitSizeMove;
bool windowSizeDirty = true;
bool updateWindowText = true;
v2u clientSize;
u32 minWindowDim;
u32 maxWindowDim;
v2s windowPos;
v2s mousePosTL, mousePosBL, prevMousePos;
v2f mouseScenePos;
v2s mouseDelta;
s32 mouseWheel;
bool keysHeld[256];
bool keysDown[256];
bool keysDownRep[256];
bool keysUp[256];
bool mouseButtons[8];
bool prevMouseButtons[8];
bool resetKeys;
bool consoleIsVisible = true;

std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t> utf16ToUtf8Converter;
Mutex utf16ToUtf8ConverterMutex;

HINSTANCE instance;

struct DropTargetImpl : IDropTarget {
	ULONG STDMETHODCALLTYPE AddRef() override { return 0; }
	ULONG STDMETHODCALLTYPE Release() override { return 0; }
	STDMETHOD(QueryInterface(REFIID riid, void** ppvObject)) override { return E_ABORT; }
	STDMETHOD(DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)) override { 
		*pdwEffect = DROPEFFECT_COPY;
		return S_OK; 
	}
	STDMETHOD(DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)) override { return 0; }
	STDMETHOD(DragLeave(void)) override { return 0; }
	STDMETHOD(Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect)) override {
		HRESULT result = S_OK;
		FORMATETC fmt{};
		fmt.cfFormat = CF_HDROP;
		fmt.dwAspect = DVASPECT_CONTENT;
		fmt.lindex = -1;
		fmt.tymed = TYMED_HGLOBAL;
		STGMEDIUM medium{};
		if (SUCCEEDED(pDataObj->GetData(&fmt, &medium))) {
			auto fileCount = DragQueryFileW((HDROP)medium.hGlobal, ~0, 0, 0);
			DEFER { 
				DragFinish((HDROP)medium.hGlobal);
				ReleaseStgMedium(&medium);
			};
			Span<Span<wchar>> paths = { ALLOCATE_T(TL_DEFAULT_ALLOCATOR, Span<wchar>, fileCount, 0), fileCount };
			DEFER { DEALLOCATE(TL_DEFAULT_ALLOCATOR, paths.data()); };
			for (u32 i = 0; i < fileCount; ++i) {
				auto pathLength = DragQueryFileW((HDROP)medium.hGlobal, i, 0, 0) + 1;
				paths[i] = { ALLOCATE_T(TL_DEFAULT_ALLOCATOR, wchar, pathLength, 0), pathLength - 1 };
				DragQueryFileW((HDROP)medium.hGlobal, i, paths[i].data(), pathLength);
			}
			app_onDragAndDrop(paths);
		}
		return result;
	}
};

DropTargetImpl dropTarget;

void platform_init() {
	instance = GetModuleHandleA(0);
	executableDirectory.resize(512);
	GetModuleFileNameW(instance, executableDirectory.data(), executableDirectory.size() + 1);
	executableDirectory.resize(executableDirectory.rfind(L'\\') + 1);
	
	logFile = _wfopen((executableDirectory + L"drawt_log.txt").data(), L"w");

	{TIMED_BLOCK("AllocConsole");
		AllocConsole();
		consoleWindow = GetConsoleWindow();
#if !BUILD_DEBUG
		hideConsoleWindow();
#endif
		
		SetConsoleCtrlHandler([](DWORD CtrlType) -> BOOL {
			switch (CtrlType) {
				case CTRL_C_EVENT:
					hideConsoleWindow();
					return true;
				case CTRL_CLOSE_EVENT:
					__try {
						app_exitAbnormal();
					} __finally {
					}
					return true;
				default:
					return false;
			}
		}, true);

		freopen("CONOUT$", "w", stdout);
	}

	wchar localeBuf[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(localeBuf, countof(localeBuf))) {
		LOGW(L"Locale: %", localeBuf);
		wchar *dash = wcsstr(localeBuf, L"-");
		if (dash) {
			*dash = L'\0';
			     if (wcscmp(localeBuf, L"en") == 0) { language = Language_english; }
			else if (wcscmp(localeBuf, L"ru") == 0) { language = Language_russian; }
			else                                    { language = Language_english; }
		}
	} else {
		auto error = GetLastError();
		LOG("GetUserDefaultLocaleName failed. GetLastError = 0x%", error);
		language = Language_english;
	}
	timeBeginPeriod(1);
}
void platform_deinit() {
	timeEndPeriod(1);
}

void createWindow() {
	WNDCLASSEXA wc{};
	wc.cbSize = sizeof wc;
	wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
		switch (msg) {
			case WM_GETMINMAXINFO: {
				MINMAXINFO *mmi = (MINMAXINFO *)lp;
				mmi->ptMinTrackSize.x = 256;
				mmi->ptMinTrackSize.y = 128;
			} return 0;
			case WM_DESTROY:
			case WM_CLOSE: {
				app_onWindowClose();
			} return 0;
			case WM_SIZE: {
				v2u newSize;
				newSize.x = GET_X_LPARAM(lp);
				newSize.y = GET_Y_LPARAM(lp);
				if (newSize.x == 0 || newSize.y == 0)
					return 0;
				clientSize = newSize;
				minWindowDim = min(clientSize.x, clientSize.y);
				maxWindowDim = max(clientSize.x, clientSize.y);
				windowResized = true;
				app_onWindowResize();
			} return 0;
			case WM_MOVE: {
				windowPos.x = GET_X_LPARAM(lp);
				windowPos.y = GET_Y_LPARAM(lp);
				app_onWindowMove();
			} return 0;
			case WM_KILLFOCUS: {
				lostFocus = true;
			} return 0;
			case WM_EXITSIZEMOVE: {
				exitSizeMove = true;
			} return 0;
		}
		return DefWindowProc(hwnd, msg, wp, lp);
	};
	wc.hIcon = LoadIconA(instance, MAKEINTRESOURCE(IDI_DRAWT));
	wc.hInstance = instance;
	wc.hCursor = LoadCursorA(0, IDC_ARROW);
	wc.lpszClassName = "drawt_main";
	wc.hbrBackground = CreateSolidBrush(RGB(32,32,32));
	if (!RegisterClassExA(&wc)) {
		LOG("RegisterClassExA failed");
		exit(-1);
	}

	DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_MAXIMIZE | WS_VISIBLE;
	mainWindow = CreateWindowExA(0, wc.lpszClassName, "Drawt", windowStyle, 0, 0, 1600, 900, NULL, NULL, instance, NULL);
	if (!mainWindow) {
		LOG("CreateWindowExA failed");
		exit(-1);
	}

	DHR(OleInitialize(0));
	DHR(RegisterDragDrop(mainWindow, &dropTarget));
}

void platform_log(char const *str) {
	OutputDebugStringA(str);
	fprintf(logFile, "%s", str);
	printf("%s", str);
}
void platform_log(wchar const *str) {
	OutputDebugStringW(str);
	lock(utf16ToUtf8ConverterMutex);
	auto utf8 = utf16ToUtf8Converter.to_bytes(str);
	unlock(utf16ToUtf8ConverterMutex);
	fprintf(logFile, "%s", utf8.data());
	wprintf(L"%s", str);
}

void hideConsoleWindow() { ShowWindow(consoleWindow, SW_HIDE); consoleIsVisible = false; }
void showConsoleWindow() { ShowWindow(consoleWindow, SW_SHOW); consoleIsVisible = true; }
void toggleConsoleWindow() {
	ShowWindow(consoleWindow, consoleIsVisible ? SW_HIDE : SW_SHOW);
	consoleIsVisible = !consoleIsVisible;
}

void showCursor() { while (ShowCursor(1) < 0); }
void hideCursor() { while (ShowCursor(0) >= 0); }

void platform_saveScene(Scene *scene, wchar const *path) {
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (file == INVALID_HANDLE_VALUE) {
		showConsoleWindow();
		LOGW(L"failed to open file for writing: %", path);
	} else {
		scene->path = path;
		scene->filename = getFilename(scene->path);
		app_writeScene(scene).stream([&] (char *data, umm size) {
			DWORD bytesWritten;
			WriteFile(file, data, size, &bytesWritten, 0);
		});
		CloseHandle(file);
		LOGW(L"saved scene: '%'", path);
	}
}
bool openSceneDialog(Scene *scene) {
	showCursor();
	
	wchar buf[1024];
	buf[0] = 0;

	OPENFILENAMEW f{};
	f.lStructSize = sizeof(f);
	f.hwndOwner = mainWindow;
	f.lpstrFilter = localizations[language].fileFilter;
	f.lpstrFile = buf;
	f.nMaxFile = 1024;
	f.lpstrTitle = localizations[language].openFileTitle;
	f.lpstrDefExt = L".drawt";
	f.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetOpenFileNameW(&f)) {
		return app_loadScene(scene, f.lpstrFile);
	} else {
		auto err = CommDlgExtendedError();
		if (err) {
			LOG("GetOpenFileNameW failed; CommDlgExtendedError: %", err);
		}
	}
	return false;
}

bool platform_messageBox(wchar const *text, wchar const *caption, MessageBoxType type) {
	switch (type) {
		case MessageBoxType::warning: return MessageBoxW(mainWindow, text, caption, MB_OKCANCEL | MB_ICONWARNING) == IDOK;
		default:
			INVALID_CODE_PATH();
	}
	return false;
}

void saveSceneDialog(Scene *scene) {
	resetKeys = true;
	showCursor();
	wchar buf[1024];
	buf[0] = 0;
	OPENFILENAMEW f{};
	f.lStructSize = sizeof(f);
	f.hwndOwner = mainWindow;
	f.lpstrFilter = localizations[language].fileFilter;
	f.lpstrFile = buf;
	f.nMaxFile = 1024;
	f.lpstrTitle = localizations[language].saveFileTitle;
	f.lpstrDefExt = L".drawt";
	f.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	if (GetSaveFileNameW(&f)) {
		bool save = true;
		if (PathFileExistsW(f.lpstrFile)) {
			save = MessageBoxW(mainWindow, localizations[language].sceneAlreadyExists, localizations[language].warning, MB_OKCANCEL | MB_ICONWARNING) == IDOK;
		}
		if (save) {
			platform_saveScene(scene, f.lpstrFile);
			updateWindowText = true;
		}
	} else {
		auto err = CommDlgExtendedError();
		if (err) {
			LOG("GetSaveFileNameW failed; CommDlgExtendedError: {}", err);
		}
	}
	hideCursor();
}

void platform_emergencySave(bool const *unsavedScenes) {
	SYSTEMTIME time = {};
	GetLocalTime(&time);
	std::wstring year   = std::to_wstring(time.wYear);
	std::wstring day    = std::to_wstring(time.wDay);    if (   day.size() == 1)    day.insert(   day.begin(), '0');
	std::wstring hour   = std::to_wstring(time.wHour);   if (  hour.size() == 1)   hour.insert(  hour.begin(), '0');
	std::wstring minute = std::to_wstring(time.wMinute); if (minute.size() == 1) minute.insert(minute.begin(), '0');
	std::wstring second = std::to_wstring(time.wSecond); if (second.size() == 1) second.insert(second.begin(), '0');

	wchar const *month;
	switch (time.wMonth) {
		case  0: month = L"jan"; break;
		case  1: month = L"feb"; break;
		case  2: month = L"mar"; break;
		case  3: month = L"apr"; break;
		case  4: month = L"may"; break;
		case  5: month = L"jun"; break;
		case  6: month = L"jul"; break;
		case  7: month = L"aug"; break;
		case  8: month = L"sep"; break;
		case  9: month = L"oct"; break;
		case 10: month = L"nov"; break;
		case 11: month = L"dec"; break;
		default: month = L"unk"; break;
	}

	std::wstring savePath = executableDirectory;
	savePath.reserve(256);
	savePath += L"saves\\";
	CreateDirectoryW(savePath.data(), 0);
	savePath += year;   savePath.push_back(L'_');
	savePath += month;  savePath.push_back(L'_');
	savePath += day;    savePath.push_back(L'_');
	savePath += hour;   savePath.push_back(L'_');
	savePath += minute; savePath.push_back(L'_');
	savePath += second; savePath.push_back(L'\\');
	CreateDirectoryW(savePath.data(), 0);
	u32 savePathSize = savePath.size();
	for (u32 i = 0; i < countof(scenes); ++i) {
		Scene *scene = scenes + i;
		if (unsavedScenes[i]) {
			savePath.resize(savePathSize);
			savePath += std::to_wstring(i);
			savePath += L".drawt";
			platform_saveScene(scene, savePath.data());
		}
	}
}

void setCursorPos(v2s p) {
	SetCursorPos(p.x, p.y);
}

void platform_beginFrame() {
	MSG message;
	while (PeekMessageA(&message, 0, 0, 0, PM_REMOVE)) {
		switch (message.message) {
			case WM_KEYUP:
			case WM_KEYDOWN:
			case WM_SYSKEYUP:
			case WM_SYSKEYDOWN: {
				auto key = (u8)message.wParam;
				[[maybe_unused]] bool extended = message.lParam & u32(1 << 24);
				bool alt      = message.lParam & u32(1 << 29);
				bool repeated = message.lParam & u32(1 << 30);
				bool wentUp   = message.lParam & u32(1 << 31);
				bool wentDown = !wentUp;
				if (key == VK_F4 && alt) {
					if (app_tryExit())
						break;
					else
						continue;
				}
				if (wentDown) {
					if (!repeated) {
						app_onKeyDown(key);
					}
					app_onKeyDownRepeated(key);
				} else {
					app_onKeyUp(key);
				}
			} continue;
			case WM_LBUTTONDOWN: app_onMouseDown(mouseButton_left); continue;
			case WM_RBUTTONDOWN: app_onMouseDown(mouseButton_right); continue;
			case WM_MBUTTONDOWN: app_onMouseDown(mouseButton_middle); continue;
			case WM_LBUTTONUP: app_onMouseUp(mouseButton_left); continue;
			case WM_RBUTTONUP: app_onMouseUp(mouseButton_right); continue;
			case WM_MBUTTONUP: app_onMouseUp(mouseButton_middle); continue;
			case WM_MOUSEWHEEL: mouseWheel = GET_WHEEL_DELTA_WPARAM(message.wParam) / WHEEL_DELTA; continue;
		}
		if (!running)
			break;
		TranslateMessage(&message);
		DispatchMessageA(&message);
	}
}

v2s getMousePos() {
	POINT cursorPos;
	GetCursorPos(&cursorPos);
	return {
		(s32)cursorPos.x - windowPos.x,
		(s32)cursorPos.y - windowPos.y
	};
}

void setWindowTitle(wchar const *title) {
	SetWindowTextW(mainWindow, title);
}
