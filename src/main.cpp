#include <variant>
#include <optional>
#include <codecvt>
#include <string>

#include <fcntl.h>
#include <stdio.h>
#include <io.h>

#define NOMINMAX
#include <Windows.h>
#include <Windowsx.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <d3dcompiler.h>

std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>,wchar_t> utf16ToUtf8Converter;
HWND consoleWindow;
FILE *logFile;

#define LOG(fmt, ...)											\
	do {														\
		char buf[1024];											\
		sprintf_s(buf, _countof(buf), fmt "\n", __VA_ARGS__);	\
		OutputDebugStringA(buf);								\
		fprintf(logFile, buf);									\
		printf("%s", buf);										\
	} while(0)

#define LOGW(fmt, ...)													\
	do {																\
		wchar buf[1024];												\
		swprintf_s(buf, _countof(buf), fmt L"\n", __VA_ARGS__);			\
		OutputDebugStringW(buf);										\
		fprintf(logFile, utf16ToUtf8Converter.to_bytes(buf).data());	\
		wprintf(L"%s", buf);											\
	} while(0)

#define ASSERTION_FAILURE(causeString, expression, ...)	\
	do {												\
		LOG("%s: %s", causeString, expression);			\
		ShowWindow(consoleWindow, SW_SHOW);				\
		DEBUG_BREAK;									\
		exit(-1);										\
	} while(0)

#define TL_IMPL
#include "../dep/tl/include/tl/common.h"
#include "../dep/tl/include/tl/list.h"
#include "../dep/tl/include/tl/math.h"
#include "../dep/tl/include/tl/d3d11.h"
#include "../dep/tl/include/tl/thread.h"

#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "comdlg32")
#pragma comment(lib, "winmm")

using namespace TL;

#define DHR(hr) 											\
	do {													\
		HRESULT _hr = hr;									\
		if (FAILED(_hr)) {									\
			char hrValueStr[16];							\
			sprintf(hrValueStr, "0x%x", _hr);				\
			LOG("BAD HRESULT: %s", #hr);					\
			LOG("VALUE: %s", hrValueStr);					\
			DEBUG_BREAK;									\
			MessageBoxA(0, "BAD HRESULT", hrValueStr, 0);	\
			exit(-1);										\
		}													\
	} while (0)

LARGE_INTEGER performanceFrequncy;

#define TIMED_BLOCK_(name, line)								\
	char const *CONCAT(__timer_name_, line) = name;				\
	LARGE_INTEGER CONCAT(__timer_begin_, line);					\
	QueryPerformanceCounter(&CONCAT(__timer_begin_, line));		\
	DEFER {														\
		LARGE_INTEGER CONCAT(__timer_end_, line);				\
		QueryPerformanceCounter(&CONCAT(__timer_end_, line));	\
		LOG("%s: %.1f ms", CONCAT(__timer_name_, line), (f64)(CONCAT(__timer_end_, line).QuadPart - CONCAT(__timer_begin_, line).QuadPart) / performanceFrequncy.QuadPart * 1000);	\
	}

#define TIMED_BLOCK(name) TIMED_BLOCK_(name, __LINE__)
#define TIMED_FUNCTION TIMED_BLOCK(__FUNCTION__)

#define memequ(a, b, s) (memcmp(a, b, s) == 0)

#include "thread_pool.h"

struct Shader {
	ID3D11VertexShader *vs = 0;
	ID3D11PixelShader *ps = 0;
};

struct alignas(16) SceneConstantBufferData {
	v2f scenePosition;
	v2f sceneScale;
	v3f drawColor;
	f32 drawThickness;
	v2f mouseWorldPos;
};

struct alignas(16) GlobalConstantBufferData {
	m4 matrixWindowToNDC;
	v2f windowSize;
	f32 windowAspect;
};
struct alignas(16) PieConstantBufferData {
	v2f piePos;
	f32 pieAngle;
	f32 pieSize;
	f32 pieAlpha;
};
struct alignas(16) ColorConstantBufferData {
	v2f position;
	f32 size;
	f32 alpha;
	v3f hueColor;
};
struct alignas(16) ActionConstantBufferData {
	v3f color;
};

struct TransformedPoint {
	m2 transform;
	v2f position;
};

struct TransformedLine {
	TransformedPoint a, b;
};

struct Point {
	f32 thickness;
	v2f position;
};

struct Line {
	Point a, b;
};

struct Quad {
	v2f position;
	v2f size;
	v2f uvMin;
	v2f uvMax;
	v4f color;
};

struct PencilAction {
	v3f color;
	List<Line> lines;

	Point newLineStartPoint;
	bool popNextTime;

	List<TransformedLine> transformedLines;
	D3D11::StructuredBuffer lineBuffer;
};

struct LineAction {
	v3f color;
	Line line;

	v2f initialPosition;
	f32 initialThickness;

	D3D11::StructuredBuffer buffer;
};

struct GridAction {
	v3f color;
	Point startPoint;
	v2f endPosition;
	u32 cellCount;

	D3D11::StructuredBuffer buffer;
};

enum ActionType {
	ActionType_none,
	ActionType_pencil,
	ActionType_line,
	ActionType_grid,
};

#define ACTION_T(PencilAction, pencil, ActionType_pencil)\
	Action(PencilAction const &that) : type(ActionType_pencil), pencil(that) {}\
	Action(PencilAction &&that) : type(ActionType_pencil), pencil(std::move(that)) {}\
	Action &operator=(PencilAction const &that) { this->~Action(); return *new (this) Action(that); }\
	Action &operator=(PencilAction &&that) { this->~Action(); return *new (this) Action(std::move(that)); }

struct Action {
	ActionType type = {};
	union {
		PencilAction pencil;
		LineAction line;
		GridAction grid;
	};
	Action() {}
	ACTION_T(PencilAction, pencil, ActionType_pencil);
	ACTION_T(LineAction, line, ActionType_line);
	ACTION_T(GridAction, grid, ActionType_grid);
	Action(Action const &that) {
		type = that.type;
		switch (that.type) {
			case ActionType_none  : break;
			case ActionType_pencil: new (this) Action(that.pencil); break;
			case ActionType_line  : new (this) Action(that.line); break;
			case ActionType_grid  : new (this) Action(that.grid); break;
		}
	}
	Action(Action &&that) {
		switch (that.type) {
			case ActionType_none  : break;
			case ActionType_pencil: new (this) Action(std::move(that.pencil)); break;
			case ActionType_line  : new (this) Action(std::move(that.line)); break;
			case ActionType_grid  : new (this) Action(std::move(that.grid)); break;
		}
		that.reset();
	}
	Action &operator=(Action const &that) { reset(); new (this) Action(that); }
	Action &operator=(Action &&that) { reset(); new (this) Action(std::move(that)); }
	~Action() {
		switch (type) {
			case ActionType_pencil: pencil.~PencilAction(); break;
			case ActionType_line: line.~LineAction(); break;
			case ActionType_grid: grid.~GridAction(); break;
		}
		type = ActionType_none;
	}
	void reset() { this->~Action(); }
};

enum Tool {
	Tool_pencil,
	Tool_line,
	Tool_grid,
	Tool_dropper,
	Tool_count,
};

struct Scene {
	List<Action> actions;
	u32 postLastVisibleActionIndex = 0;
	Tool currentTool = Tool_pencil;
	v2f cameraPosition = {};
	f32 cameraDistance = 1.0f;
	f32 drawThickness = 16.0f;
	v3f drawColor = {1,1,1};

	std::wstring path;
	wchar *filename = 0;
	u64 savedHash;

	D3D11::RenderTexture canvasRT, canvasRTMS;
	bool needResize = true;
	bool needRepaint = true;
	SceneConstantBufferData constantBufferData;
	ID3D11Buffer *constantBuffer;
	bool matrixSceneToNDCDirty = true;
	bool drawColorDirty = true;
	bool constantBufferDirty = false;

	bool initialized = false;
};

enum Language {
	Language_english,
	Language_russian,
	Language_count
};

struct Localization {
	wchar const *windowTitle_path_gridSize;
	wchar const *windowTitle_path;
	wchar const *windowTitle_gridSize;
	wchar const *windowTitle;
	wchar const *warning;
	wchar const *fileFilter;
	wchar const *saveFileTitle;
	wchar const *openFileTitle;
	wchar const *unsavedScene;
	wchar const *unsavedScenes;
};

struct PieMenuItem {
	v2u uv;
	void (*onSelect)();
};
struct PieMenu {
	List<PieMenuItem> items;
	f32 T = 0;
	f32 prevT = 0;
	bool TChanged = false;
	v2f position = {};
	bool opened = false;
	f32 alpha = 0;
	ID3D11Buffer *constantBuffer = 0;
};

enum ColorMenuTarget {
	ColorMenuTarget_draw,
	ColorMenuTarget_canvas,
};

std::wstring executableDirectory;

HWND mainWindow;
bool running;
bool needRepaint = true;
bool windowResized;
bool lostFocus;
bool exitSizeMove;
bool windowSizeDirty = true;
bool updateWindowText = true;
v2u windowSize;
v2s windowPos;
v2s mousePos, mousePosBL, prevMousePos;
v2f mouseWorldPos;
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
u32 ignoreCursorFrameCount;

ThreadPool threadPool;

Language language;
Localization localizations[Language_count];

D3D11::State d3d11State;
Shader lineShader, quadShader, blitShader, blitShaderMS, circleShader, pieSelShader, colorMenuShader;
constexpr u32 vertexPerLineCount = 48;
D3D11::StructuredBuffer uiSBuffer;
constexpr u32 toolImageSize = 32;
D3D11::Texture toolAtlas;
ID3D11BlendState *alphaBlend;
ID3D11Buffer *globalConstantBuffer;
ID3D11Buffer *colorConstantBuffer;
ID3D11Buffer *actionConstantBuffer;
GlobalConstantBufferData globalConstantBufferData;
ColorConstantBufferData colorConstantBufferData;
ID3D11RasterizerState *currentRasterizer;
ID3D11RasterizerState *defaultRasterizer;
ID3D11RasterizerState *wireframeRasterizer;
u32 msaaSampleCount;
v4f canvasColor = {0.25f, 0.25f, 0.25f, 1.0f};

constexpr u32 pencilLineBufferDefElemCount = 1024;

Scene scenes[10];
Scene *currentScene = scenes + 1;
Scene *previousScene;
u64 emptySceneHash;
Action currentAction;
v2f cameraVelocity;
f32 targetCameraDistance = 1.0f;
f32 oldCameraDistance = 1.0f;
f32 targetThickness = 16.0f;
f32 oldThickness = 16.0f;
constexpr float minPencilLineLength = 0.5f;
bool playingSceneShiftAnimation;
f32 sceneShiftT;
u32 displayGridSize;
PieMenu mainPieMenu, toolPieMenu, canvasPieMenu;
constexpr f32 pieMenuSize = 0.125f;

bool colorMenuOpened;
bool colorMenuSelecting;
f32 colorMenuAlpha;
v2f colorMenuPosition;
f32 colorMenuHue;
ColorMenuTarget colorMenuTarget;

bool loadScene(Scene *, wchar const *);

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
			for (u32 i = 0; i < fileCount; ++i) {
				auto pathLength = DragQueryFileW((HDROP)medium.hGlobal, i, 0, 0) + 1;
				auto path = (wchar *)malloc(pathLength * sizeof(wchar));
				DragQueryFileW((HDROP)medium.hGlobal, i, path, pathLength);
				if (loadScene(currentScene, path)) {
					targetCameraDistance = currentScene->cameraDistance;
				} else {
					result = S_FALSE;
				}
				break;
			}
			DragFinish((HDROP)medium.hGlobal);
			ReleaseStgMedium(&medium);
		}
		return result;
	}
};

DropTargetImpl dropTarget;

u64 getHash(Scene const *scene);
void exitAppAbnormal();

void hideConsoleWindow() { ShowWindow(consoleWindow, SW_HIDE); consoleIsVisible = false; }
void showConsoleWindow() { ShowWindow(consoleWindow, SW_SHOW); consoleIsVisible = true; }
void toggleConsoleWindow() {
	ShowWindow(consoleWindow, consoleIsVisible ? SW_HIDE : SW_SHOW);
	consoleIsVisible = !consoleIsVisible;
}
void initLocalization() {
	wchar localeBuf[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(localeBuf, _countof(localeBuf))) {
		LOGW(L"Locale: %s", localeBuf);
		wchar *dash = wcsstr(localeBuf, L"-");
		if (dash) {
			*dash = L'\0';
			     if (wcscmp(localeBuf, L"en") == 0) { language = Language_english; }
			else if (wcscmp(localeBuf, L"ru") == 0) { language = Language_russian; }
			else                                    { language = Language_english; }
		}
	} else {
		LOG("GetUserDefaultLocaleName failed. GetLastError = 0x%x", GetLastError());
		language = Language_english;
	}
	localizations[Language_english].windowTitle_path_gridSize = L"Drawt - %s - Scene %u - Grid size: %u";
	localizations[Language_english].windowTitle_path          = L"Drawt - %s - Scene %u";
	localizations[Language_english].windowTitle_gridSize      = L"Drawt - <untitled> - Scene %u - Grid size: %u";
	localizations[Language_english].windowTitle               = L"Drawt - <untitled> - Scene %u";
	localizations[Language_english].warning                   = L"Warning!";
	localizations[Language_english].fileFilter                = L"Drawt scene\0*.drawt\0";
	localizations[Language_english].saveFileTitle             = L"Save Drawt Scene";
	localizations[Language_english].openFileTitle             = L"Open Drawt Scene";
	localizations[Language_english].unsavedScene              = L"This scene has unsaved changes. Discard?";
	localizations[Language_english].unsavedScenes             = L"There are unsaved scenes. Exit?";

	localizations[Language_russian].windowTitle_path_gridSize = L"Drawt - %s - Сцена %u - Размер сетки: %u";
	localizations[Language_russian].windowTitle_path          = L"Drawt - %s - Сцена %u";
	localizations[Language_russian].windowTitle_gridSize      = L"Drawt - <untitled> - Сцена %u - Размер сетки: %u";
	localizations[Language_russian].windowTitle               = L"Drawt - <untitled> - Сцена %u";
	localizations[Language_russian].warning                   = L"Внимание!";
	localizations[Language_russian].fileFilter                = L"Drawt сцена\0*.drawt\0";
	localizations[Language_russian].saveFileTitle             = L"Сохранить Drawt сцену";
	localizations[Language_russian].openFileTitle             = L"Открыть Drawt сцену";
	localizations[Language_russian].unsavedScene              = L"Эта сцена не сохранена. Продолжить?";
	localizations[Language_russian].unsavedScenes             = L"Не все сцены сохранены. Выйти?";
}
void initGlobals(HINSTANCE instance) {
	TIMED_FUNCTION;
	emptySceneHash = getHash(scenes + 0);
	executableDirectory.resize(512);
	GetModuleFileNameW(instance, executableDirectory.data(), executableDirectory.size() + 1);
	executableDirectory.resize(executableDirectory.rfind(L'\\') + 1);
	logFile = _wfopen((executableDirectory + L"log.txt").data(), L"w");
	QueryPerformanceFrequency(&performanceFrequncy);

	{TIMED_BLOCK("AllocConsole");
		AllocConsole();
		consoleWindow = GetConsoleWindow();
		hideConsoleWindow();

		SetConsoleCtrlHandler([](DWORD CtrlType) -> BOOL {
			switch (CtrlType) {
				case CTRL_C_EVENT:
					hideConsoleWindow();
					return true;
				case CTRL_CLOSE_EVENT:
					exitAppAbnormal();
					return true;
				default:
					return false;
			}
		}, true);

		freopen("CONOUT$", "w", stdout);
	}

	initThreadPool(&threadPool, 4);
}

bool equals(Scene const *sa, Scene const *sb) {
	if (sa->postLastVisibleActionIndex != sb->postLastVisibleActionIndex)
		return false;
	for (u32 i = 0; i < sa->postLastVisibleActionIndex; ++i) {
		auto &a = sa->actions[i];
		auto &b = sb->actions[i];
		if(a.type != b.type)
			return false;
		switch (a.type) {
			case ActionType_none:
				return true;
			case ActionType_pencil: {
				auto &ap = a.pencil;
				auto &bp = b.pencil;
				if (ap.lines.size() != bp.lines.size())
					return false;
				if (!memequ(ap.lines.data(), bp.lines.data(), sizeof(Line) * ap.lines.size()))
					return false;
			} break;
			case ActionType_line:{
				auto &al = a.line;
				auto &bl = b.line;
				if (!memequ(&al.line, &bl.line, sizeof(Line)))
					return false;
			} break;
			case ActionType_grid:{
				auto &ag = a.grid;
				auto &bg = b.grid;
				if (!memequ(&ag.startPoint, &bg.startPoint, sizeof(ag.startPoint))) return false;
				if (!memequ(&ag.endPosition, &bg.endPosition, sizeof(ag.endPosition))) return false;
				if (!memequ(&ag.cellCount, &bg.cellCount, sizeof(ag.cellCount))) return false;
			} break;
		}
	}
	return true;
}

u64 addHash(u64 v, u64 acc) { return acc ^ rotateLeft(acc ^ 0xB0A82D45578D69D6, (acc ^ 0x21) & 63) ^ v; }
u64 getHash(u32 v) { return (v | ((u64)v << 32)) ^ 0xF7E6D5C4B3A29180; }
u64 getHash(f32 v) { return getHash(*(u32 *)&v); }
u64 getHash(u64 v) { return v; }
u64 getHash(v2f v) { return *(u64 *)&v; }
u64 getHash(v3f v) { return ((u64 *)&v)[0] ^ ((u32 *)&v)[2]; }
u64 getHash(v4f v) { return ((u64 *)&v)[0] ^ ((u64 *)&v)[1]; }
u64 getHash(Point const &point) { return addHash(getHash(point.position), getHash(point.thickness)); }
u64 getHash(Line const &line) { return addHash(getHash(line.a), getHash(line.b)); }
u64 getHash(Scene const *scene) {
	u64 result = 0x0123456789ABCDEF;
	auto mix = [&](auto const &v) { result = addHash(getHash(v), result); };
	mix(scene->postLastVisibleActionIndex);
	for (u32 i = 0; i < scene->postLastVisibleActionIndex; ++i) {
		auto &a = scene->actions[i];
		switch (a.type) {
			case ActionType_none: {
				INVALID_CODE_PATH("scene->actions should not contain ActionType_none");
			} break;
			case ActionType_pencil: {
				auto &pencil = a.pencil;
				mix(pencil.lines.size());
				for (auto &line : pencil.lines) {
					mix(line);
				}
			} break;
			case ActionType_line: {
				auto &line = a.line;
				mix(line.line);
			} break;
			case ActionType_grid: {
				auto &grid = a.grid;
				mix(grid.startPoint);
				mix(grid.endPosition);
				mix(grid.cellCount);
			} break;
		}
	}
	return result;
}

void initializeScene(Scene *scene) {
	TIMED_FUNCTION;
	scene->initialized = true;
	scene->savedHash = emptySceneHash;

	D3D11_BUFFER_DESC d{};
	d.ByteWidth = sizeof(scene->constantBufferData);
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	DHR(d3d11State.device->CreateBuffer(&d, 0, &scene->constantBuffer));
	scene->constantBufferData.drawThickness = scene->drawThickness;
}

bool keyHeld(u32 k) { return keysHeld[k]; }
bool keyDown(u32 k) { return keysDown[k]; }
bool keyDownRep(u32 k) { return keysDownRep[k]; }
bool keyUp(u32 k) { return keysUp[k]; }
bool keyChanged(u32 k) { return keyDown(k) || keyUp(k); }
bool mouseButtonHeld(u32 k) { return mouseButtons[k]; }
bool mouseButtonDown(u32 k) { return mouseButtons[k] && !prevMouseButtons[k]; }
bool mouseButtonUp(u32 k) { return !mouseButtons[k] && prevMouseButtons[k]; }
bool cursorHeld() { return mouseButtonHeld(0); }
bool cursorDown() { return mouseButtonDown(0); }
bool cursorUp() { return mouseButtonUp(0); }

f32 getDrawThickness(Scene const *scene) { return scene->drawThickness * scene->cameraDistance; }

v2f getWorldPos(v2s screenPos) { return currentScene->cameraPosition + ((v2f)screenPos - 0.5f * (v2f)windowSize) * currentScene->cameraDistance; }
Point const *getEndpoint(Action const &action) {
	switch (action.type) {
		case ActionType_pencil: return &action.pencil.lines.back().b;
		case ActionType_line: return &action.line.line.b;
		case ActionType_grid:
		case ActionType_none: return (Point const *)0;
	}
	INVALID_CODE_PATH();
}

TransformedLine transform(Line line) {
	TransformedLine result;
	m2 rotation = m2::rotation(atan2(line.b.position - line.a.position));
	result.a.transform = m2::scaling(line.a.thickness) * rotation;
	result.b.transform = m2::scaling(line.b.thickness) * rotation;
	result.a.position = line.a.position;
	result.b.position = line.b.position;
	return result;
}

void pushLine(PencilAction &pencil, Line line) {
	pencil.lines.push_back(line);
	pencil.transformedLines.push_back(transform(line));
}

void cleanup(Action &action) {
	switch (action.type) {
		case ActionType_pencil: release(action.pencil.lineBuffer); break;
		case ActionType_line:   release(action.line.buffer); break;
		case ActionType_grid:   release(action.grid.buffer); break;
		case ActionType_none: break;
	}
}

void resizeRenderTargets() {
	release(d3d11State.backBuffer);

	DHR(d3d11State.swapChain->ResizeBuffers(1, windowSize.x, windowSize.y, DXGI_FORMAT_UNKNOWN, 0));

	d3d11State.createBackBuffer();

	for (auto &scene : scenes) {
		scene.needResize = true;
	}
}

ID3D11VertexShader *createVertexShader(char const *src, umm srcSize, char const *name) {
	ID3DBlob *bc;
	ID3DBlob *errors;
	HRESULT compileResult = D3DCompile(src, srcSize, name, 0, 0, "main", "vs_5_0", 0, 0, &bc, &errors);
	if (errors) {
		LOG("%s", (char *)errors->GetBufferPointer());
		errors->Release();
	}
	if (FAILED(compileResult)) {
		LOG("VS compilation failed");
		DHR(compileResult);
	}
	ID3D11VertexShader *result;
	DHR(d3d11State.device->CreateVertexShader(bc->GetBufferPointer(), bc->GetBufferSize(), 0, &result));
	bc->Release();
	return result;
}
ID3D11PixelShader *createPixelShader(char const *src, umm srcSize, char const *name) {
	ID3DBlob *bc;
	ID3DBlob *errors;
	HRESULT compileResult = D3DCompile(src, srcSize, name, 0, 0, "main", "ps_5_0", 0, 0, &bc, &errors);
	if (errors) {
		LOG("%s", (char *)errors->GetBufferPointer());
		errors->Release();
	}
	if (FAILED(compileResult)) {
		LOG("VS compilation failed");
		DHR(compileResult);
	}
	ID3D11PixelShader *result;
	DHR(d3d11State.device->CreatePixelShader(bc->GetBufferPointer(), bc->GetBufferSize(), 0, &result));
	bc->Release();
	return result;
}

#define SCENE_CBUFFER_SOURCE \
R"(
cbuffer _ : register(b0) {
	float2 scenePosition;
	float2 sceneScale;
	float3 drawColor;
	float drawThickness;
	float2 mouseWorldPos;
}
float2 sceneToNDC(float2 p) { return (p - scenePosition) * sceneScale; }
float2 sceneToNDC(float2 p, float2 offset) { return (p - scenePosition + offset) * sceneScale; }
)"
#define GLOBAL_CBUFFER_SOURCE \
R"(
cbuffer _ : register(b1) {
	float4x4 matrixWindowToNDC;
	float2 windowSize;
	float windowAspect;
};
)"
#define PIE_CBUFFER_SOURCE \
R"(
cbuffer _ : register(b2) {
	float2 piePos;
	float pieAngle;
	float pieSize;
	float pieAlpha;
}
)"
#define COLOR_CBUFFER_SOURCE \
R"(
cbuffer _ : register(b3) {
	float2 colorMenuPos;
	float colorMenuSize;
	float colorMenuAlpha;
	float3 colorMenuHueColor;
}
)"
#define ACTION_CBUFFER_SOURCE \
R"(
cbuffer _ : register(b4) {
	float3 actionColor;
}
)"

void initLineShader() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[1024*3];
	u32 const vertexShaderSourceSize = sprintf_s(vertexShaderSourceData, sizeof(vertexShaderSourceData), SCENE_CBUFFER_SOURCE R"(
#define VERTICES_PER_LINE_COUNT %u

struct Point {
	float2x2 transform;
	float2 position;
};
struct Line {
	Point a, b;
};
StructuredBuffer<Line> lines : register(t0);

struct Vertex {
	float2 position;
	float t;
};
struct Out {
	float4 position : SV_Position;
};
struct In {
	uint id : SV_VertexId;
};

#define maxx 0.5f
#define SIN(x) (sin((x) / 180.0f * 3.1415926535897932384626433832795f) * maxx)
#define COS(x) (cos((x) / 180.0f * 3.1415926535897932384626433832795f) * maxx)
#define CS(x) {COS(x),SIN(x)}

#define HALF(x, a, b)								        \
	{CS(x+270  ), a}, {CS(x+ 90  ), a}, {CS(x+270  ), b},   \
	{CS(x+180  ), a}, {CS(x+ 90  ), a}, {CS(x+270  ), a},	\
	{CS(x+135  ), a}, {CS(x+ 90  ), a}, {CS(x+180  ), a},	\
	{CS(x+112.5), a}, {CS(x+ 90  ), a}, {CS(x+135  ), a},	\
	{CS(x+157.5), a}, {CS(x+135  ), a}, {CS(x+180  ), a},	\
	{CS(x+225  ), a}, {CS(x+180  ), a}, {CS(x+270  ), a},	\
	{CS(x+202.5), a}, {CS(x+180  ), a}, {CS(x+225  ), a},	\
	{CS(x+247.5), a}, {CS(x+225  ), a}, {CS(x+270  ), a}

static const Vertex vertexBuffer[] = {
	HALF(  0, 0, 1),
	HALF(180, 1, 0),
};
float4 main(in In i) : SV_Position {
	Out o;
	Vertex vertex = vertexBuffer[i.id %% VERTICES_PER_LINE_COUNT];
	Line l = lines[i.id / VERTICES_PER_LINE_COUNT];
	float2x2 transform = lerp(l.a.transform, l.b.transform, vertex.t);
	float2 position = lerp(l.a.position, l.b.position, vertex.t);
	return float4(sceneToNDC(mul(transform, vertex.position) + position), 0, 1);
}
)", vertexPerLineCount);
	lineShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "line_vs");

	char pixelShaderSourceData[] = ACTION_CBUFFER_SOURCE R"(
float4 main() : SV_Target {
	return float4(actionColor, 1.0f);
}
)";
	u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
	lineShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "line_ps");
}
void initUIQuadShader() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[] = GLOBAL_CBUFFER_SOURCE SCENE_CBUFFER_SOURCE R"(
struct Out {
	float2 uv : UV;
	float4 color : COLOR;
	float4 position : SV_Position;
};
struct In {
	uint id : SV_VertexId;
};

struct Quad {
	float2 position;
	float2 size;
	float2 uvMin;
	float2 uvMax;
	float4 color;
};
StructuredBuffer<Quad> quads : register(t0);

static const float2 vertexData[] = {
	{0, 0},
	{0, 1},
	{1, 0},
	{0, 1},
	{1, 1},
	{1, 0},
};

Out main(in In i) {
	Out o;

	Quad quad = quads[i.id / 6];	

	float2 vertexPos = vertexData[i.id % 6];

	o.uv = lerp(quad.uvMin, quad.uvMax, vertexPos);
	o.position = mul(matrixWindowToNDC, float4(quad.position + vertexPos * quad.size, 0, 1));
	o.color = quad.color;
	return o;
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
	quadShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "quad_vs");

	char pixelShaderSourceData[] = R"(
Texture2D srcTex : register(t0);
SamplerState samplerState;
float4 main(in float2 uv : UV, in float4 color : COLOR) : SV_Target {
	return srcTex.Sample(samplerState, uv) * color;
}
)";
	u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
	quadShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "quad_ps");
}
void initBlitShader() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[] = GLOBAL_CBUFFER_SOURCE R"(
struct Output {
	float2 uv : UV;
	float4 position : SV_Position;
};
static const float2 vertexData[] = {
	{-1,-1},
	{-1, 3},
	{ 3,-1},
};
void main(in uint id : SV_VertexId, out Output o) {
	o.uv = (vertexData[id] + 1) * 0.5f;
	o.uv.y = 1 - o.uv.y;
	o.uv *= windowSize;
	o.position = float4(vertexData[id], 0, 1);
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
	blitShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "blit_vs");

	char pixelShaderSourceData[] = R"(
Texture2D srcTex : register(t0);
float4 main(in float2 uv : UV) : SV_Target {
	return srcTex.Load(int3(uv, 0));
}
)";
	u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
	blitShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "blit_ps");
}
void initBlitShaderMS() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[] = GLOBAL_CBUFFER_SOURCE R"(
struct Output {
	float2 uv : UV;
	float4 position : SV_Position;
};
static const float2 vertexData[] = {
	{-1,-1},
	{-1, 3},
	{ 3,-1},
};
void main(in uint id : SV_VertexId, out Output o) {
	o.uv = (vertexData[id] + 1) * 0.5f;
	o.uv.y = 1 - o.uv.y;
	o.uv *= windowSize;
	o.position = float4(vertexData[id], 0, 1);
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
	blitShaderMS.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "blitms_vs");

	char pixelShaderSourceData[1024];
	u32 const pixelShaderSourceSize = sprintf_s(pixelShaderSourceData, sizeof(pixelShaderSourceData), R"(
#define MSAA_SAMPLE_COUNT %u
#if MSAA_SAMPLE_COUNT == 1
Texture2D srcTex : register(t0);
float4 main(in float2 uv : UV) : SV_Target {
	return srcTex.Load(uv, 0);
}
#else
Texture2DMS<float4> srcTex : register(t0);
float4 main(in float2 uv : UV) : SV_Target {
	float4 color = 0;
	for (uint i = 0; i < MSAA_SAMPLE_COUNT; ++i)
		color += srcTex.Load(uv, i);
	return color / MSAA_SAMPLE_COUNT;// + float4(uv / 1000, 0, 1);
}
#endif
)", msaaSampleCount);
	blitShaderMS.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "blitms_ps");
}
void initCircleShader() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[] = SCENE_CBUFFER_SOURCE R"(
#define DEG(x) ((x) / 180.0f * 3.1415926535897932384626433832795f)
#define STEP 15
#define LINE(x) {cos(DEG(x)), sin(DEG(x))}, {cos(DEG(x+STEP)), sin(DEG(x+STEP))}
static const float2 vertexData[] = {
	LINE(  0), LINE( 15),
	LINE( 30), LINE( 45),
	LINE( 60), LINE( 75),
	LINE( 90), LINE(105),
	LINE(120), LINE(135),
	LINE(150), LINE(165),
	LINE(180), LINE(195),
	LINE(210), LINE(225),
	LINE(240), LINE(255),
	LINE(270), LINE(285),
	LINE(300), LINE(315),
	LINE(330), LINE(345),
};
float4 main(in uint id : SV_VertexId) : SV_Position {
	return float4(sceneToNDC(mouseWorldPos, vertexData[id] * 0.5f * drawThickness), 0, 1);
	//return mul(matrixSceneToNDC, float4(mouseWorldPos + vertexData[id] * 0.5f * drawThickness, 0, 1));
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
	circleShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "circle_vs");

	char pixelShaderSourceData[] = SCENE_CBUFFER_SOURCE R"(
float4 main() : SV_Target {
	return float4(drawColor, 1);
}
)";
	u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
	circleShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "circle_ps");
}
void initPieSelShader() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[] = PIE_CBUFFER_SOURCE GLOBAL_CBUFFER_SOURCE R"(
#define DEG(x) ((x) / 180.0f * 3.1415926535897932384626433832795f)
#define COS(x) cos(DEG(x))
#define SIN(x) sin(DEG(x))
struct Vertex {
	float2 position;
	float alpha;
};
#define MR 0.05f
#define MCOS(x) (COS(x)*MR)
#define MSIN(x) (SIN(x)*MR)
static const Vertex vertexData[] = {
	{{0, 0}, 0.5f}, {{-MCOS( 0  ), MSIN( 0  )}, 0.0f}, {{-MCOS(22.5), MSIN(22.5)}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(22.5), MSIN(22.5)}, 0.0f}, {{-MCOS(45  ), MSIN(45  )}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(45  ), MSIN(45  )}, 0.0f}, {{  COS(45  ),  SIN(45  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(45  ),  SIN(45  )}, 0.0f}, {{  COS(44  ),  SIN(44  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(44  ),  SIN(44  )}, 0.0f}, {{  COS(34  ),  SIN(34  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(34  ),  SIN(34  )}, 0.0f}, {{  COS(22  ),  SIN(22  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(22  ),  SIN(22  )}, 0.0f}, {{  COS(11  ),  SIN(11  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(11  ),  SIN(11  )}, 0.0f}, {{  COS( 0  ),  SIN( 0  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS( 0  ),  SIN( 0  )}, 0.0f}, {{  COS(11  ), -SIN(11  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(11  ), -SIN(11  )}, 0.0f}, {{  COS(22  ), -SIN(22  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(22  ), -SIN(22  )}, 0.0f}, {{  COS(34  ), -SIN(34  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(34  ), -SIN(34  )}, 0.0f}, {{  COS(44  ), -SIN(44  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(44  ), -SIN(44  )}, 0.0f}, {{  COS(45  ), -SIN(45  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(45  ), -SIN(45  )}, 0.0f}, {{-MCOS(45  ),-MSIN(45  )}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(45  ),-MSIN(45  )}, 0.0f}, {{-MCOS(22.5),-MSIN(22.5)}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(22.5),-MSIN(22.5)}, 0.0f}, {{-MCOS( 0  ),-MSIN( 0  )}, 0.0f},
};
void main(out float alpha : ALPHA, out float4 position : SV_Position, in uint id : SV_VertexId) {
	Vertex v = vertexData[id];
	float2x2 rotation = {
		 cos(pieAngle),-sin(pieAngle),
		 sin(pieAngle), cos(pieAngle),
	};
	alpha = v.alpha * pieAlpha;
	position = mul(matrixWindowToNDC, float4(piePos + mul(rotation, v.position) * pieSize * 1.5f, 0, 1));
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
	pieSelShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "pieSel_vs");

	char pixelShaderSourceData[] = SCENE_CBUFFER_SOURCE R"(
float4 main(in float alpha : ALPHA) : SV_Target {
	return float4(1, 1, 1, alpha);
}
)";
	u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
	pieSelShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "pieSel_ps");
}
void initColorMenuShader() {
	TIMED_FUNCTION;
	char vertexShaderSourceData[] = COLOR_CBUFFER_SOURCE GLOBAL_CBUFFER_SOURCE SCENE_CBUFFER_SOURCE R"(
static const float3 vertexData[] = {
	{-0.05,-0.05, 1},
	{-0.05, 1.05, 1},
	{ 1.05,-0.05, 1},
	{-0.05, 1.05, 1},
	{ 1.05, 1.05, 1},
	{ 1.05,-0.05, 1},

	{0, 0, 0},
	{0, 1, 0},
	{1, 0, 0},
	{0, 1, 0},
	{1, 1, 0},
	{1, 0, 0},
};
void main(out float3 uv : UV, out float4 position : SV_Position, in uint id : SV_VertexId) {
	float3 v = vertexData[id];
	uv = v;
	position = mul(matrixWindowToNDC, float4(colorMenuPos + v.xy * colorMenuSize, 0, 1));
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
	colorMenuShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "colorMenu_vs");

	char pixelShaderSourceData[] = COLOR_CBUFFER_SOURCE SCENE_CBUFFER_SOURCE R"(
float4 main(in float3 uv : UV) : SV_Target {
	return float4(lerp(lerp(1, colorMenuHueColor, uv.x) * uv.y, drawColor, uv.z), colorMenuAlpha);
}
)";
	u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
	colorMenuShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "colorMenu_ps");
}

void setViewport(f32 x, f32 y, f32 w, f32 h) {
	D3D11_VIEWPORT v{};
	v.TopLeftX = x;
	v.TopLeftY = y;
	v.Width = w;
	v.Height = h;
	v.MaxDepth = 1.0f;
	d3d11State.immediateContext->RSSetViewports(1, &v);
}

bool shouldRepaint(Scene const *scene) {
	return scene->drawColorDirty || scene->constantBufferDirty || scene->matrixSceneToNDCDirty || scene->needRepaint;
}

v2u getUv(Tool tool) {
	switch (tool) {
		case Tool_pencil: return {0, 1};
		case Tool_line: return {1, 1};
		case Tool_grid: return {0, 0};
		case Tool_dropper: return {1, 0};
		default:
			INVALID_CODE_PATH();
			break;
	}
}
template <class T>
void setUv(T &quad, v2u uv) {
	quad.uvMin = V2f(uv.x * 0.25f, uv.y * 0.25f);
	quad.uvMax = quad.uvMin + 0.25f;
}

void drawAction(Action const &action) {
	ActionConstantBufferData actionData;
	switch (action.type) {
		case ActionType_none: {
		} break;
		case ActionType_pencil: {
			actionData.color = action.pencil.color;
			d3d11State.immediateContext->UpdateSubresource(actionConstantBuffer, 0, 0, &actionData, 0, 0);
			d3d11State.immediateContext->VSSetShader(lineShader.vs, 0, 0);
			d3d11State.immediateContext->PSSetShader(lineShader.ps, 0, 0);
			d3d11State.immediateContext->VSSetShaderResources(0, 1, &action.pencil.lineBuffer.srv);
			d3d11State.immediateContext->Draw(action.pencil.lines.size() * vertexPerLineCount, 0);
		} break;
		case ActionType_line: {
			actionData.color = action.line.color;
			d3d11State.immediateContext->UpdateSubresource(actionConstantBuffer, 0, 0, &actionData, 0, 0);
			d3d11State.immediateContext->VSSetShader(lineShader.vs, 0, 0);
			d3d11State.immediateContext->PSSetShader(lineShader.ps, 0, 0);
			d3d11State.immediateContext->VSSetShaderResources(0, 1, &action.line.buffer.srv);
			d3d11State.immediateContext->Draw(vertexPerLineCount, 0);
		} break;
		case ActionType_grid: {
			actionData.color = action.grid.color;
			d3d11State.immediateContext->UpdateSubresource(actionConstantBuffer, 0, 0, &actionData, 0, 0);
			d3d11State.immediateContext->VSSetShader(lineShader.vs, 0, 0);
			d3d11State.immediateContext->PSSetShader(lineShader.ps, 0, 0);
			d3d11State.immediateContext->VSSetShaderResources(0, 1, &action.grid.buffer.srv);
			u32 lineCount = action.grid.cellCount * 2 + 2;
			d3d11State.immediateContext->Draw(lineCount * vertexPerLineCount, 0);
		} break;
	}
}
void repaintScene(Scene *scene, bool drawCursor) {
	scene->needRepaint = false;
	needRepaint = true;
	
	if (scene->needResize) {
		scene->needResize = false;
		release(scene->canvasRT);
		release(scene->canvasRTMS);
		scene->canvasRT = d3d11State.createRenderTexture(windowSize.x, windowSize.y, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_CPU_ACCESS_READ);
		scene->canvasRTMS = d3d11State.createRenderTexture(windowSize.x, windowSize.y, msaaSampleCount, DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	if (scene->matrixSceneToNDCDirty) {
		scene->matrixSceneToNDCDirty = false;
		scene->constantBufferDirty = true;
		scene->constantBufferData.scenePosition = scene->cameraPosition;
		scene->constantBufferData.sceneScale = reciprocal(scene->cameraDistance) / (v2f)windowSize * 2.0f;
		//scene->constantBufferData.matrixSceneToNDC = m4::scaling(reciprocal(scene->cameraDistance)) * m4::scaling(2.0f / (v2f)windowSize, 1) * m4::translation(-scene->cameraPosition, 0);
	}
	if (scene->drawColorDirty) {
		scene->drawColorDirty = false;
		scene->constantBufferData.drawColor = scene->drawColor;
		scene->constantBufferDirty = true;
	}
	if (scene->constantBufferDirty) {
		scene->constantBufferDirty = false;
		d3d11State.immediateContext->UpdateSubresource(scene->constantBuffer, 0, 0, &scene->constantBufferData, 0, 0);
	} 

	// clearing canvas instead of back buffer for dropper to work
	d3d11State.clearRenderTarget(scene->canvasRTMS, canvasColor.data());
	//d3d11State.clearRenderTarget(scene->canvasRTMS, v4f{1,1,1,0}.data());
	d3d11State.immediateContext->OMSetRenderTargets(1, &scene->canvasRTMS.rtv, 0);
	d3d11State.immediateContext->VSSetConstantBuffers(0, 1, &scene->constantBuffer);
	d3d11State.immediateContext->PSSetConstantBuffers(0, 1, &scene->constantBuffer);
	
	d3d11State.immediateContext->OMSetBlendState(0, v4f{}.data(), ~0);

	d3d11State.immediateContext->RSSetState(currentRasterizer);
	for (u32 i = 0; i < scene->postLastVisibleActionIndex; ++i) {
		drawAction(scene->actions[i]);
	}
	drawAction(currentAction);
	d3d11State.immediateContext->RSSetState(defaultRasterizer);
	
	if (drawCursor) {
		d3d11State.immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

		d3d11State.immediateContext->VSSetShader(circleShader.vs, 0, 0);
		d3d11State.immediateContext->PSSetShader(circleShader.ps, 0, 0);
		d3d11State.immediateContext->Draw(48, 0);

		d3d11State.immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}
	
	d3d11State.immediateContext->OMSetRenderTargets(1, &scene->canvasRT.rtv, 0);
	d3d11State.immediateContext->PSSetShaderResources(0, 1, &scene->canvasRTMS.srv);
	d3d11State.setVertexShader(blitShaderMS.vs);
	d3d11State.setPixelShader(blitShaderMS.ps);
	d3d11State.immediateContext->Draw(3, 0);
}
void updatePieBuffer(PieMenu &menu) {
	if (menu.T) {
		PieConstantBufferData data;
		data.piePos = menu.position;
		data.pieSize = min(windowSize.x, windowSize.y) * pieMenuSize;
		data.pieAlpha = menu.T * menu.alpha;
		data.pieAngle = atan2((v2f)mousePosBL - menu.position);
		d3d11State.immediateContext->UpdateSubresource(menu.constantBuffer, 0, 0, &data, 0, 0);
	}
}
void repaint(bool drawCursor) {
	if (windowSizeDirty) {
		windowSizeDirty = false;
		globalConstantBufferData.matrixWindowToNDC = m4::translation(-1, -1, 0) * m4::scaling(2.0f / (v2f)windowSize, 1);
		globalConstantBufferData.windowSize = (v2f)windowSize;
		globalConstantBufferData.windowAspect = (f32)windowSize.x / windowSize.y;
		d3d11State.immediateContext->UpdateSubresource(globalConstantBuffer, 0, 0, &globalConstantBufferData, 0, 0);
	}

	setViewport(0, 0, windowSize.x, windowSize.y);

	if (shouldRepaint(currentScene)) {
		repaintScene(currentScene, drawCursor);
	}
	if (playingSceneShiftAnimation) {
		if (shouldRepaint(previousScene)) {
			repaintScene(previousScene, drawCursor);
		}
	}
	
	if (needRepaint) {
		needRepaint = false;
		
		d3d11State.setRenderTarget(d3d11State.backBuffer);
		d3d11State.setVertexShader(blitShader.vs);
		d3d11State.setPixelShader(blitShader.ps);
		
		if (playingSceneShiftAnimation) {
			D3D11_VIEWPORT v{};
			v.Width = windowSize.x;
			v.Height = windowSize.y;
			v.MaxDepth = 1.0f;

			if (currentScene > previousScene)
				v.TopLeftX = lerp(0.0f, -v.Width, sceneShiftT);
			else
				v.TopLeftX = lerp(0.0f, v.Width, sceneShiftT);

			d3d11State.immediateContext->RSSetViewports(1, &v);
			d3d11State.immediateContext->PSSetShaderResources(0, 1, &previousScene->canvasRT.srv);
			d3d11State.immediateContext->Draw(3, 0);
			
			if (currentScene > previousScene)
				v.TopLeftX = lerp(v.Width, 0.0f, sceneShiftT);
			else
				v.TopLeftX = lerp(-v.Width, 0.0f, sceneShiftT);
			d3d11State.immediateContext->RSSetViewports(1, &v);
		}

		d3d11State.immediateContext->PSSetShaderResources(0, 1, &currentScene->canvasRT.srv);
		d3d11State.immediateContext->Draw(3, 0);
		
		d3d11State.immediateContext->OMSetBlendState(alphaBlend, v4f{}.data(), ~0);

		auto makeQuad = [](v2f position, v2f size, v2u uv, v4f color) {
			Quad q;
			q.position = position;
			q.position.y -= size.y;
			q.size = size;
			q.color = color;
			setUv(q, uv);
			return q;
		};
		auto pushShadowedQuad = [&](auto &quads, v2f position, v2f size, v2u uv, v4f color) {
			auto quad = makeQuad(position, size, uv, color);
			auto shadow = quad;
			setUv(shadow, {2, 1});
			shadow.position -= toolImageSize;
			shadow.size += toolImageSize * 2;
			shadow.color.w *= 0.5f;
			quads.push_back(shadow);
			quads.push_back(quad);
		};
		auto drawPieMenu = [&] (PieMenu &menu) {
			updatePieBuffer(menu);
			if (menu.T) {
				StaticList<Quad, 16> quads;

				d3d11State.immediateContext->VSSetConstantBuffers(2, 1, &menu.constantBuffer);
				d3d11State.immediateContext->VSSetShader(pieSelShader.vs, 0, 0);
				d3d11State.immediateContext->PSSetShader(pieSelShader.ps, 0, 0);

				d3d11State.immediateContext->Draw(48, 0);
			
				f32 extent = min(windowSize.x, windowSize.y) * pieMenuSize;

				for (u32 i = 0; i < menu.items.size(); ++i) {
					v2f offset = m2::rotation(map(i, 0.0f, menu.items.size(), 0.0f, pi * 2)) * V2f(-extent, 0);
					auto &item = menu.items[i];
					pushShadowedQuad(quads, menu.position + offset * menu.T + v2f{-1, 1} * toolImageSize * 0.5f, V2f(toolImageSize), item.uv, V4f(1, 1, 1, menu.T));
				}
			
				d3d11State.immediateContext->VSSetShader(quadShader.vs, 0, 0);
				d3d11State.immediateContext->PSSetShader(quadShader.ps, 0, 0);

				d3d11State.immediateContext->VSSetShaderResources(0, 1, &uiSBuffer.srv);
				d3d11State.immediateContext->PSSetShaderResources(0, 1, &toolAtlas.srv);

				d3d11State.updateStructuredBuffer(uiSBuffer, quads.size(), sizeof(Quad), quads.data());

				d3d11State.immediateContext->Draw(quads.size() * 6, 0);
			}
		};
		drawPieMenu(mainPieMenu);
		drawPieMenu(toolPieMenu);
		drawPieMenu(canvasPieMenu);

		d3d11State.immediateContext->OMSetBlendState(alphaBlend, v4f{}.data(), ~0);
		if (drawCursor) {

			Quad cursorQuad = makeQuad((v2f)mousePosBL, V2f(toolImageSize), getUv(currentScene->currentTool), V4f(1));
			
			d3d11State.immediateContext->VSSetShader(quadShader.vs, 0, 0);
			d3d11State.immediateContext->PSSetShader(quadShader.ps, 0, 0);

			d3d11State.immediateContext->VSSetShaderResources(0, 1, &uiSBuffer.srv);
			d3d11State.immediateContext->PSSetShaderResources(0, 1, &toolAtlas.srv);

			d3d11State.updateStructuredBuffer(uiSBuffer, 1, sizeof(Quad), &cursorQuad);

			d3d11State.immediateContext->Draw(6, 0);
		}

		if (colorMenuAlpha) {

			colorConstantBufferData.size = min(windowSize.x, windowSize.y) * 0.2f;
			colorConstantBufferData.position = colorMenuPosition;
			colorConstantBufferData.alpha = colorMenuAlpha;
			colorConstantBufferData.hueColor = hsvToRgb(colorMenuHue, 1, 1);
			d3d11State.immediateContext->UpdateSubresource(colorConstantBuffer, 0, 0, &colorConstantBufferData, 0, 0);

			d3d11State.immediateContext->VSSetShader(colorMenuShader.vs, 0, 0);
			d3d11State.immediateContext->PSSetShader(colorMenuShader.ps, 0, 0);
			d3d11State.immediateContext->Draw(12, 0);
		}

		d3d11State.swapChain->Present(1, 0);
	} else {
		Sleep(16);
	}
}

void showCursor() { while (ShowCursor(1) < 0); }
void hideCursor() { while (ShowCursor(0) >= 0); }

template <class T>
void writeValue(HANDLE file, T const &value) {
	DWORD bytesWritten;
	if (!WriteFile(file, &value, sizeof(value), &bytesWritten, 0) || bytesWritten != sizeof(value))
		INVALID_CODE_PATH("WriteFile failed");
}
bool readValue(Span<u8 const> &data, void *dst, umm dstSize) {
	if (dstSize > data.size())
		return false;
	memcpy(dst, data.begin(), dstSize);
	data._begin += dstSize;
	return true;
}
template <class T>
bool readValue(Span<u8 const> &data, T &value) {
	return readValue(data, &value, sizeof(value));
}

void writeScene(HANDLE file, Scene *scene) {
	writeValue(file, scene->cameraDistance);
	writeValue(file, scene->cameraPosition);
	writeValue(file, scene->currentTool);
	writeValue(file, scene->drawColor);
	writeValue(file, scene->drawThickness);
	writeValue(file, scene->postLastVisibleActionIndex);
	for (auto &action : scene->actions) {
		switch (action.type) {
			case ActionType_none: INVALID_CODE_PATH("scene->actions should not contain ActionType_none"); break;
			case ActionType_pencil: {
				writeValue(file, 'P');
				auto count = action.pencil.lines.size();
				writeValue(file, count);
				DWORD bytesWritten;
				WriteFile(file, action.pencil.lines.data(), count * sizeof(Line), &bytesWritten, 0);
			} break;
			case ActionType_line: {
				writeValue(file, 'L');
				writeValue(file, action.line.line);
			} break;
			case ActionType_grid: {
				writeValue(file, 'G');
				writeValue(file, action.grid.startPoint);
				writeValue(file, action.grid.endPosition);
				writeValue(file, action.grid.cellCount);
			} break;
		}
	}
	scene->savedHash = getHash(scene);
}

wchar *getFilename(std::wstring &path) {
	return path.data() + path.rfind(L'\\') + 1;
}

void saveScene(Scene *scene, wchar const *path) {
	LOGW(L"saving scene: '%s'", path);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (file == INVALID_HANDLE_VALUE) {
		showConsoleWindow();
		LOGW(L"failed to open file for writing: %s", path);
	} else {
		scene->path = path;
		scene->filename = getFilename(scene->path);
		writeScene(file, scene);
		CloseHandle(file);
	}
}
void saveSceneAs(Scene *scene) {
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
	if (GetSaveFileNameW(&f)) {
		saveScene(scene, f.lpstrFile);
		updateWindowText = true;
	} else {
		auto err = CommDlgExtendedError();
		LOG("GetSaveFileNameW failed; CommDlgExtendedError: %u", err);
	}
	hideCursor();
}
void saveScene(Scene *scene) {
	if (scene->path.size()) {
		saveScene(scene, scene->path.c_str());
	} else {
		saveSceneAs(scene);
	}
}

bool readAction(Span<u8 const> &data, Action &result) {
	DWORD bytesRead;
	char actionType;
	if (!readValue(data, actionType))
		return false;
	switch (actionType) {
		case 'P': {
			PencilAction pencil;
			umm count;
			if (!readValue(data, count))
				return false;
			pencil.lines.resize(count);
			if (!readValue(data, pencil.lines.data(), count * sizeof(Line)))
				return false;
			pencil.transformedLines.reserve(count);
			for (auto line : pencil.lines) {
				pencil.transformedLines.push_back(transform(line));
			}
			pencil.lineBuffer = d3d11State.createStructuredBuffer(pencil.transformedLines.size(), sizeof(TransformedLine), pencil.transformedLines.data(), D3D11_USAGE_IMMUTABLE);
			result = std::move(pencil);
		} break;
		case 'L': {
			LineAction line;
			if (!readValue(data, line.line))
				return false;
			TransformedLine transformedLine = transform(line.line);
			line.buffer = d3d11State.createStructuredBuffer(1, sizeof(TransformedLine), &transformedLine, D3D11_USAGE_DYNAMIC);
			result = std::move(line);
		} break;
		default: {
			LOG("action data corrupted");
			return false;
		} break;
	}
	return true;
}
bool readScene(Span<u8 const> data, Scene *scene) {
	Scene prevScene = std::move(*scene);
	if (!readValue(data, scene->cameraDistance)) return false;
	if (!readValue(data, scene->cameraPosition)) return false;
	if (!readValue(data, scene->currentTool)) return false;
	if (!readValue(data, scene->drawColor)) return false;
	if (!readValue(data, scene->drawThickness)) return false;
	if (!readValue(data, scene->postLastVisibleActionIndex)) return false;

	scene->constantBufferDirty = true;
	scene->drawColorDirty = true;
	scene->matrixSceneToNDCDirty = true;
	scene->constantBufferData.drawThickness = getDrawThickness(scene);

	for (;;) {
		if (!data.size()) {
			break;
		}
		Action action;
		if (readAction(data, action)) {
			scene->actions.push_back(std::move(action));
		}
		else {
			*scene = std::move(prevScene);
			return false;
		}
	}
	needRepaint = true;
	scene->savedHash = getHash(scene);
	return true;
}
bool loadScene(Scene *scene, HANDLE file) {
	LARGE_INTEGER endPointer;
	SetFilePointerEx(file, {}, &endPointer, FILE_END);
	SetFilePointerEx(file, {}, 0, FILE_BEGIN);
	auto data = malloc(endPointer.QuadPart);
	DEFER { free(data); };
	DWORD bytesRead;
	if (!ReadFile(file, data, endPointer.QuadPart, &bytesRead, 0) || bytesRead != endPointer.QuadPart) {
		LOG("ReadFile failed");
		return false;
	}
	return readScene({(u8 *)data, (u8 *)data + endPointer.QuadPart}, scene);
}
bool loadScene(Scene *scene, wchar const *path) {
	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (file == INVALID_HANDLE_VALUE) {
		LOGW(L"Failed to open file: %s", path);
		return false;
	} else {
		DEFER { CloseHandle(file); };
		if (loadScene(scene, file)) {
			scene->path = path;
			scene->filename = getFilename(scene->path);
			return true;
		} else {
			LOGW(L"Failed to load scene from: %s", path);
			return false;
		}
	}
}
bool openScene(Scene *scene) {
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
	if (GetOpenFileNameW(&f)) {
		return loadScene(scene, f.lpstrFile);
	} else {
		auto err = CommDlgExtendedError();
		LOG("GetOpenFileNameW failed; CommDlgExtendedError: %u", err);
	}
	return false;
}

bool isUnsaved(Scene const *scene) {
	if (scene->initialized) {
		if (scene->savedHash != getHash(scene)) {
			return true;
		} else if (scene->path.size()) {
			Scene savedScene;
			if (!loadScene(&savedScene, scene->path.c_str())) {
				return true;
			}
			if (!equals(scene, &savedScene)) {
				return true;
			}
		}
	}
	return false;
}
bool tryExitApp() {
	showCursor();
	for (auto &scene : scenes) {
		if (isUnsaved(&scene)) {
			currentScene = &scene;
			needRepaint = true;
			repaint(false);
			if (MessageBoxW(mainWindow, localizations[language].unsavedScenes, localizations[language].warning, MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
				running = false;
				return true;
			} else {
				return false;
			}
		}
	}
	running = false;
	return true;
}
void exitAppAbnormal() {
	bool hasUnsavedScenes = false;
	for (u32 i = 0; i < _countof(scenes); ++i) {
		Scene *scene = scenes + i;
		if (isUnsaved(scene)) {
			hasUnsavedScenes = true;
			break;
		}
	}
	if (hasUnsavedScenes) {
		SYSTEMTIME time = {};
		GetLocalTime(&time);
		std::wstring year   = std::to_wstring(time.wYear);
		std::wstring month  = std::to_wstring(time.wMonth);  if ( month.size() == 1)  month.insert( month.begin(), '0');
		std::wstring day    = std::to_wstring(time.wDay);    if (   day.size() == 1)    day.insert(   day.begin(), '0');
		std::wstring hour   = std::to_wstring(time.wHour);   if (  hour.size() == 1)   hour.insert(  hour.begin(), '0');
		std::wstring minute = std::to_wstring(time.wMinute); if (minute.size() == 1) minute.insert(minute.begin(), '0');
		std::wstring second = std::to_wstring(time.wSecond); if (second.size() == 1) second.insert(second.begin(), '0');

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
		for (u32 i = 0; i < _countof(scenes); ++i) {
			Scene *scene = scenes + i;
			if (isUnsaved(scene)) {
				savePath.resize(savePathSize);
				savePath += std::to_wstring(i);
				savePath += L".drawt";
				saveScene(scene, savePath.data());
			}
		}
	}
	running = false;
}

void initD3D11() {
	TIMED_FUNCTION;
	{TIMED_BLOCK("D3D11::createState");
		UINT deviceFlags = 0;
#if BUILD_DEBUG
		deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
		d3d11State = D3D11::createState(mainWindow, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, true, 1, deviceFlags);
	}
	msaaSampleCount = d3d11State.getMaxMsaaSampleCount(DXGI_FORMAT_R8G8B8A8_UNORM);
	WorkQueue d3d11Queue = makeWorkQueue(&threadPool);

	d3d11State.immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	
	D3D11_BUFFER_DESC d{};
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	d.ByteWidth = sizeof(globalConstantBufferData);
	DHR(d3d11State.device->CreateBuffer(&d, 0, &globalConstantBuffer));
	d3d11State.immediateContext->VSSetConstantBuffers(1, 1, &globalConstantBuffer);
	d3d11State.immediateContext->PSSetConstantBuffers(1, 1, &globalConstantBuffer);
	
	d.ByteWidth = sizeof(ActionConstantBufferData);
	DHR(d3d11State.device->CreateBuffer(&d, 0, &actionConstantBuffer));
	d3d11State.immediateContext->VSSetConstantBuffers(4, 1, &actionConstantBuffer);
	d3d11State.immediateContext->PSSetConstantBuffers(4, 1, &actionConstantBuffer);

	d.ByteWidth = sizeof(colorConstantBufferData);
	DHR(d3d11State.device->CreateBuffer(&d, 0, &colorConstantBuffer));
	d3d11State.immediateContext->VSSetConstantBuffers(3, 1, &colorConstantBuffer);
	d3d11State.immediateContext->PSSetConstantBuffers(3, 1, &colorConstantBuffer);

	d.ByteWidth = sizeof(PieConstantBufferData);
	DHR(d3d11State.device->CreateBuffer(&d, 0, &mainPieMenu.constantBuffer));
	DHR(d3d11State.device->CreateBuffer(&d, 0, &toolPieMenu.constantBuffer));
	DHR(d3d11State.device->CreateBuffer(&d, 0, &canvasPieMenu.constantBuffer));

	d3d11Queue.push([&] { initLineShader();   });
	d3d11Queue.push([&] { initUIQuadShader(); });
	d3d11Queue.push([&] { initBlitShader();   });
	d3d11Queue.push([&] { initBlitShaderMS();   });
	d3d11Queue.push([&] { initCircleShader(); });
	d3d11Queue.push([&] { initPieSelShader(); });
	d3d11Queue.push([&] { initColorMenuShader(); });

	d3d11Queue.push([&] {
		TIMED_BLOCK("Init samplers");
		D3D11_SAMPLER_DESC d{};
		d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		d.MaxLOD = FLT_MAX;
		ID3D11SamplerState *samplerState;
		DHR(d3d11State.device->CreateSamplerState(&d, &samplerState));
		d3d11State.immediateContext->PSSetSamplers(0, 1, &samplerState);
	});

	d3d11Queue.push([&] {
		TIMED_BLOCK("Init blend");
		D3D11_BLEND_DESC d{};
		d.RenderTarget[0].BlendEnable = true;
		d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		d.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		d.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
		d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		DHR(d3d11State.device->CreateBlendState(&d, &alphaBlend));
	});
	
	{
		static constexpr u32 atlasData[] {
#include "atlas.h"
		};
		static_assert(_countof(atlasData) == 128*128);
		d3d11Queue.push([&] {
			TIMED_BLOCK("toolAtlas");
			toolAtlas = d3d11State.createTexture(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, atlasData);
		});
	}

	uiSBuffer = d3d11State.createStructuredBuffer(16, sizeof(Quad), 0, D3D11_USAGE_DYNAMIC);
	
	d3d11Queue.push([&] {
		TIMED_BLOCK("CreateRasterizerState");
		D3D11_RASTERIZER_DESC d = {};
		d.AntialiasedLineEnable = true;
		d.CullMode = D3D11_CULL_BACK;
		d.FillMode = D3D11_FILL_SOLID;
		d.MultisampleEnable = true;
		DHR(d3d11State.device->CreateRasterizerState(&d, &defaultRasterizer));
		d.FillMode = D3D11_FILL_WIREFRAME;
		DHR(d3d11State.device->CreateRasterizerState(&d, &wireframeRasterizer));

		currentRasterizer = defaultRasterizer;
	});
	
	d3d11Queue.completeAllWork();
}

void openPieMenu(PieMenu &menu) {
	menu.opened = true;
	menu.position = (v2f)mousePosBL;
	menu.T = 0.0f;
	menu.alpha = 0.0f;
}
void openColorMenu(ColorMenuTarget target) {
	colorMenuOpened = true;
	colorMenuPosition = (v2f)mousePosBL;
	colorMenuTarget = target;
};

void start(HINSTANCE instance, LPWSTR args) {
	TIMED_FUNCTION;
	initGlobals(instance);

	WorkQueue startQueue = makeWorkQueue(&threadPool);

	startQueue.push([] {
		initLocalization();
	});

	LOGW("args: %s", args);

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
				tryExitApp();
			} return 0;
			case WM_SIZE: {
				v2u newSize;
				newSize.x = GET_X_LPARAM(lp);
				newSize.y = GET_Y_LPARAM(lp);
				if (newSize.x == 0 || newSize.y == 0)
					return 0;
				windowSize = newSize;
				windowResized = true;
				if (d3d11State.swapChain) {
					resizeRenderTargets();
					currentScene->matrixSceneToNDCDirty = true;
					windowSizeDirty = true;
					repaint(false);
				}
			} return 0;
			case WM_MOVE: {
				windowPos.x = GET_X_LPARAM(lp);
				windowPos.y = GET_Y_LPARAM(lp);
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
	wc.hInstance = instance;
	wc.hCursor = LoadCursorA(0, IDC_ARROW);
	wc.lpszClassName = "drawt_main";
	if (!RegisterClassExA(&wc)) {
		LOG("RegisterClassExA failed");
		exit(-1);
	}

	DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_MAXIMIZE;
	mainWindow = CreateWindowExA(0, wc.lpszClassName, "Drawt", windowStyle, 0, 0, 1600, 900, NULL, NULL, instance, NULL);
	if (!mainWindow) {
		LOG("CreateWindowExA failed");
		exit(-1);
	}
	
	DHR(OleInitialize(0));
	DHR(RegisterDragDrop(mainWindow, &dropTarget));
	
	initD3D11();
	
	startQueue.completeAllWork();

	initializeScene(currentScene);
	
	std::wstring initialScenePath = args;
	if (initialScenePath.size() && initialScenePath.back() == L'"')
		initialScenePath.pop_back();
	if (initialScenePath.size() && initialScenePath.front() == L'"')
		initialScenePath.erase(initialScenePath.begin());

	if (initialScenePath.size()) {
		loadScene(currentScene, initialScenePath.c_str());
		targetCameraDistance = currentScene->cameraDistance;
	}
	
	PieMenuItem item;
	
	item.uv = {2,0};
	item.onSelect = [] { openPieMenu(toolPieMenu); };
	mainPieMenu.items.push_back(item);
	
	item.uv = {3,1};
	item.onSelect = [] { openPieMenu(canvasPieMenu); };
	mainPieMenu.items.push_back(item);
	

	item.uv = {0,1};
	item.onSelect = [] { currentScene->currentTool = Tool_pencil; };
	toolPieMenu.items.push_back(item);

	item.uv = {1,1};
	item.onSelect = [] { currentScene->currentTool = Tool_line; };
	toolPieMenu.items.push_back(item);

	item.uv = {0,0};
	item.onSelect = [] { currentScene->currentTool = Tool_grid; };
	toolPieMenu.items.push_back(item);

	item.uv = {1,0};
	item.onSelect = [] { currentScene->currentTool = Tool_dropper; };
	toolPieMenu.items.push_back(item);
	
	item.uv = {3,0};
	item.onSelect = [] { openColorMenu(ColorMenuTarget_draw); };
	toolPieMenu.items.push_back(item);
	
	
	item.uv = {3,0};
	item.onSelect = [] { openColorMenu(ColorMenuTarget_canvas); };
	canvasPieMenu.items.push_back(item);
	

	ShowWindow(mainWindow, SW_SHOW);

	running = true;
}

void setCursorPos(v2s p) {
	SetCursorPos(p.x, p.y);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR args, int) {
	timeBeginPeriod(1);
	DEFER { timeEndPeriod(1); };

	start(instance, args);

	while (running) {
		mouseDelta = {};
		mouseWheel = {};
		memset(keysDownRep, 0, sizeof(keysDownRep));
		memset(keysDown, 0, sizeof(keysDown));
		memset(keysUp, 0, sizeof(keysUp));
		memcpy(prevMouseButtons, mouseButtons, sizeof(mouseButtons));
		MSG message;
		while (PeekMessageA(&message, 0, 0, 0, PM_REMOVE)) {
			switch (message.message) {
				case WM_KEYUP:
				case WM_KEYDOWN:
				case WM_SYSKEYUP:
				case WM_SYSKEYDOWN: {
					auto code = (u8)message.wParam;
					[[maybe_unused]] bool extended = message.lParam & u32(1 << 24);
					bool alt = message.lParam & u32(1 << 29);
					bool isRepeated = message.lParam & u32(1 << 30);
					bool wentUp = message.lParam & u32(1 << 31);
					if (code == VK_F4 && alt) {
						tryExitApp();
					}
					keysDownRep[code] = !wentUp;
					if (isRepeated == wentUp) {
						keysDown[code] = !wentUp;
						keysHeld[code] = !wentUp;
						keysUp[code] = wentUp;
					}
				} continue;
				case WM_LBUTTONDOWN: mouseButtons[0] = true;  continue;
				case WM_RBUTTONDOWN: mouseButtons[1] = true;  continue;
				case WM_MBUTTONDOWN: mouseButtons[2] = true;  continue;
				case WM_LBUTTONUP:   mouseButtons[0] = false; continue;
				case WM_RBUTTONUP:   mouseButtons[1] = false; continue;
				case WM_MBUTTONUP:   mouseButtons[2] = false; continue;
				case WM_MOUSEWHEEL: mouseWheel = GET_WHEEL_DELTA_WPARAM(message.wParam) / WHEEL_DELTA; continue;
			}
			TranslateMessage(&message);
			DispatchMessageA(&message);
		}
		resetKeys |= lostFocus || exitSizeMove;
		if (resetKeys) {
			resetKeys = false;
			memset(keysDown    , 0, sizeof(keysDown));
			memset(keysUp      , 0, sizeof(keysUp));
			memset(keysHeld    , 0, sizeof(keysHeld));
			memset(mouseButtons, 0, sizeof(mouseButtons));
		}
		lostFocus = false;
		exitSizeMove = false;

		bool smoothMouseWorldPosChanged = false;
		
		f32 colorMenuSize = min(windowSize.x, windowSize.y) * 0.2f;
		bool hoveringColorMenu = colorMenuOpened && inBounds((v2f)mousePosBL, aabbMinDim(colorMenuPosition, V2f(colorMenuSize)));

		prevMousePos = mousePos;

		if (ignoreCursorFrameCount) {
			--ignoreCursorFrameCount;
		} else {
			POINT cursorPos;
			GetCursorPos(&cursorPos);
			mousePos.x = cursorPos.x - windowPos.x;
			mousePos.y = cursorPos.y - windowPos.y;
		}
		
		mousePosBL.x = mousePos.x;
		mousePosBL.y = windowSize.y - mousePos.y;

		mouseDelta = mousePos - prevMousePos;
		
		static bool prevMouseHovering = false;
		bool mouseHovering = inBounds(mousePos, aabbMinMax(v2s{}, (v2s)windowSize));
		bool mouseExited = !mouseHovering && prevMouseHovering;
		prevMouseHovering = mouseHovering;

		if (mouseHovering) {
			hideCursor();
		} else {
			showCursor();
		}

		bool gridCellCountChanged = false;
		if (!hoveringColorMenu && mouseWheel) {
			if (keyHeld(VK_CONTROL)) {
				switch (currentAction.type) {
					case ActionType_none: break;
					case ActionType_pencil: break;
					case ActionType_line: break;
					case ActionType_grid: {
						displayGridSize = currentAction.grid.cellCount = (u32)max((s32)currentAction.grid.cellCount + mouseWheel, 1);
						updateWindowText = true;
						release(currentAction.grid.buffer);
						currentAction.grid.buffer = d3d11State.createStructuredBuffer(currentAction.grid.cellCount * 2 + 2, sizeof(TransformedLine), 0, D3D11_USAGE_DYNAMIC);
						gridCellCountChanged = true;
					} break;
					default:
						break;
				}
			} else if (keyHeld(VK_SHIFT)) {
				targetThickness = clamp(targetThickness / pow(1.1f, mouseWheel), 2, 1000);
			} else {
				targetCameraDistance /= pow(1.1f, mouseWheel);
				targetCameraDistance = clamp(targetCameraDistance, 1.0f / 8, 64);
			}
		}
		oldCameraDistance = currentScene->cameraDistance;
		if (distance(currentScene->cameraDistance, targetCameraDistance) < currentScene->cameraDistance * 0.001f) {
			currentScene->cameraDistance = targetCameraDistance;
		} else {
			currentScene->cameraDistance = lerp(currentScene->cameraDistance, targetCameraDistance, 0.5f);
		}

		oldThickness = currentScene->drawThickness;
		if (distance(currentScene->drawThickness, targetThickness) < currentScene->cameraDistance * 0.001f) {
			currentScene->drawThickness = targetThickness;
		} else {
			currentScene->drawThickness = lerp(currentScene->drawThickness, targetThickness, 0.5f);
		}


		bool thicknessChanged = currentScene->drawThickness != oldThickness;
		bool cameraDistanceChanged = currentScene->cameraDistance != oldCameraDistance;
		bool mousePosChanged = mouseDelta.x || mouseDelta.y;
		bool mouseWorldPosChanged = mousePosChanged || cameraDistanceChanged;

		if (currentAction.type == ActionType_none) {
			u32 numberKeyDown = ~0;
				 if (keyDown('1')) numberKeyDown = 1;
			else if (keyDown('2')) numberKeyDown = 2;
			else if (keyDown('3')) numberKeyDown = 3;
			else if (keyDown('4')) numberKeyDown = 4;
			else if (keyDown('5')) numberKeyDown = 5;
			else if (keyDown('6')) numberKeyDown = 6;
			else if (keyDown('7')) numberKeyDown = 7;
			else if (keyDown('8')) numberKeyDown = 8;
			else if (keyDown('9')) numberKeyDown = 9;
			else if (keyDown('0')) numberKeyDown = 0;
			if (numberKeyDown != ~0 && numberKeyDown != (currentScene - scenes)) {
				previousScene = currentScene;
				currentScene = scenes + numberKeyDown;
				if (!currentScene->initialized)
					initializeScene(currentScene);
				thicknessChanged = true;
				smoothMouseWorldPosChanged = true;
				targetCameraDistance = currentScene->cameraDistance;
				cameraDistanceChanged = true;
				mouseWorldPosChanged = true;
				playingSceneShiftAnimation = true;
				sceneShiftT = 0.0f;
				updateWindowText = true;
			}
		}

		if (playingSceneShiftAnimation) {
			sceneShiftT = lerp(sceneShiftT, 1.0f, 0.3f);
			needRepaint = true;
			if (1.0f - sceneShiftT < 0.001f) {
				sceneShiftT = 0;
				playingSceneShiftAnimation = false;
				
				D3D11_VIEWPORT v{};
				v.Width = windowSize.x;
				v.Height = windowSize.y;
				v.MaxDepth = 1.0f;
				d3d11State.immediateContext->RSSetViewports(1, &v);
			}
		}

		if (mouseButtonHeld(2)) {
			cameraVelocity = -(v2f)mouseDelta * currentScene->cameraDistance;
			cameraVelocity.y = -cameraVelocity.y;
		} else {
			cameraVelocity *= 0.75f;
		}

		if (lengthSqr(cameraVelocity) < 0.5f * currentScene->cameraDistance) {
			cameraVelocity = {};
		} else {
			currentScene->cameraPosition += cameraVelocity;
			currentScene->matrixSceneToNDCDirty = true;
		}
		
		mouseWorldPos = getWorldPos(mousePosBL);
		
		static v2f smoothMouseWorldPos = {};
		static v2f oldSmoothMouseWorldPos = {};
		static f32 smoothMouseWorldPosT = 1.0f;
		smoothMouseWorldPosT = moveTowards(smoothMouseWorldPosT, keyHeld(VK_MENU) && cursorHeld() ? 0.1f : 1.0f, 0.1f);
		oldSmoothMouseWorldPos = smoothMouseWorldPos;
		smoothMouseWorldPos = lerp(smoothMouseWorldPos, mouseWorldPos, smoothMouseWorldPosT);
		smoothMouseWorldPosChanged |= (cursorHeld() || mouseHovering) && distanceSqr(smoothMouseWorldPos, oldSmoothMouseWorldPos) > currentScene->cameraDistance * 0.01f;
		
		if (cameraDistanceChanged) {
			currentScene->matrixSceneToNDCDirty = true;
		}
		if (cameraDistanceChanged || thicknessChanged) {
			currentScene->constantBufferData.drawThickness = getDrawThickness(currentScene);
			currentScene->constantBufferDirty = true;
		}

		if (keyDownRep('Z')) {
			if (keyHeld(VK_CONTROL)) {
				if (currentAction.type == ActionType_none) {
					if (keyHeld(VK_SHIFT)) {
						if (currentScene->postLastVisibleActionIndex < currentScene->actions.size()) {
							currentScene->postLastVisibleActionIndex++;
							currentScene->needRepaint = true;
						}
					} else {
						if (currentScene->postLastVisibleActionIndex) {
							currentScene->postLastVisibleActionIndex--;
							currentScene->needRepaint = true;
						}
					}
				}
			}
		} else if (keyDown('S')) {
			if (keyHeld(VK_CONTROL)) {
				if (keyHeld(VK_SHIFT)) {
					resetKeys = true;
					saveSceneAs(currentScene);
				} else {
					resetKeys = true;
					saveScene(currentScene);
				}
			}
		} else if (keyDown('O')) {
			if (keyHeld(VK_CONTROL)) {
				showCursor();
				resetKeys = true;
				bool openNewScene = true;
				if (isUnsaved(currentScene)) {
					if (MessageBoxW(mainWindow, localizations[language].unsavedScene, localizations[language].warning, MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
						openNewScene = false;
					}
				}
				if (openNewScene) {
					auto newScene = std::move(*currentScene);
					openScene(&newScene);
					targetCameraDistance = newScene.cameraDistance;
					*currentScene = std::move(newScene);
					updateWindowText = true;
				}
			}
		} else if (keyDown(VK_F1)) {
			toggleConsoleWindow();
		} else if (keyDown(VK_F2)) {
			DEBUG_BREAK;
		} else if (keyDown(VK_F3)) {
			if (currentRasterizer == defaultRasterizer)
				currentRasterizer = wireframeRasterizer;
			else
				currentRasterizer = defaultRasterizer;
			currentScene->needRepaint = true;
		}
		if (colorMenuOpened && keyDown(VK_ESCAPE)) {
			colorMenuOpened = false;
		}
		if (hoveringColorMenu) {
			showCursor();
			if (cursorDown()) {
				colorMenuSelecting = true;
			}
			if (mouseWheel) {
				v3f rgb;
				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: rgb = currentScene->drawColor; currentScene->drawColorDirty = true; break;
					case ColorMenuTarget_canvas: rgb = canvasColor.xyz; currentScene->needRepaint = true; break;
				}

				colorMenuHue = frac(colorMenuHue + mouseWheel * 0.01f);
				v3f hsv = rgbToHsv(rgb);
				hsv.x = colorMenuHue;
				rgb = hsvToRgb(hsv);

				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: currentScene->drawColor = rgb; break;
					case ColorMenuTarget_canvas: canvasColor.xyz = rgb; break;
				}
			}
		}
		if (colorMenuSelecting) {
			if (cursorUp()) {
				colorMenuSelecting = false;
			}
			if (cursorHeld()) {
				v3f rgb;
				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: rgb = currentScene->drawColor; currentScene->drawColorDirty = true; break;
					case ColorMenuTarget_canvas: rgb = canvasColor.xyz; currentScene->needRepaint = true; break;
				}

				v2f d = clamp(((v2f)mousePosBL - colorMenuPosition) / colorMenuSize, v2f{}, V2f(1.0f));
				v3f hsv = rgbToHsv(rgb);
				hsv.x = colorMenuHue;
				hsv.y = d.x;
				hsv.z = d.y;
				rgb = hsvToRgb(hsv);
				
				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: currentScene->drawColor = rgb; break;
					case ColorMenuTarget_canvas: canvasColor.xyz = rgb; break;
				}
			}
		} else {
			if (cursorDown()) {
				colorMenuOpened = false;
			}
			if (!mainPieMenu.opened && !toolPieMenu.opened && !canvasPieMenu.opened) {
				if (cursorDown()) {
					smoothMouseWorldPos = mouseWorldPos;
					smoothMouseWorldPosChanged = true;
					switch (currentScene->currentTool) {
						case Tool_pencil: {
							PencilAction pencil = {};
							pencil.color = currentScene->drawColor;
							pencil.newLineStartPoint.position = smoothMouseWorldPos;
							// pencil.newLineStartPoint.color = currentScene->drawColor;
							pencil.newLineStartPoint.thickness = getDrawThickness(currentScene);
							pencil.lineBuffer = d3d11State.createStructuredBuffer(pencilLineBufferDefElemCount, sizeof(TransformedLine), 0, D3D11_USAGE_DEFAULT);
							currentAction = std::move(pencil);
						} break;
						case Tool_line: {
							LineAction line = {};
							line.color = currentScene->drawColor;
							line.initialPosition = line.line.b.position = smoothMouseWorldPos;
							line.initialThickness = line.line.b.thickness = getDrawThickness(currentScene);
							// line.line.a.color = line.line.b.color = currentScene->drawColor;
							Point const *endpoint = 0;
							if (keyHeld(VK_CONTROL) && currentScene->postLastVisibleActionIndex) {
								endpoint = getEndpoint(currentScene->actions[currentScene->postLastVisibleActionIndex - 1]);
							}
							if (endpoint) {
								line.line.a.position = endpoint->position;
								line.line.a.thickness = endpoint->thickness;
							} else {
								line.line.a.position = line.initialPosition;
								line.line.a.thickness = line.initialThickness;
							}
							line.buffer = d3d11State.createStructuredBuffer(1, sizeof(TransformedLine), 0, D3D11_USAGE_DYNAMIC);
							currentAction = std::move(line);
						} break;
						case Tool_grid: {
							GridAction grid = {};
							grid.color = currentScene->drawColor;
							grid.startPoint.position = grid.endPosition = smoothMouseWorldPos;
							grid.startPoint.thickness = getDrawThickness(currentScene);
							// grid.startPoint.color = currentScene->drawColor;
							grid.cellCount = displayGridSize = 4;
							updateWindowText = true;
							grid.buffer = d3d11State.createStructuredBuffer(grid.cellCount * 2 + 2, sizeof(TransformedLine), 0, D3D11_USAGE_DYNAMIC);
							currentAction = std::move(grid);
						} break;
					}
				} 
				if (cursorHeld()) {
					switch (currentScene->currentTool) {
						case Tool_dropper: {
							if (mousePosChanged || cursorDown()) {
								if (d3d11State.device3) {
									DHR(d3d11State.immediateContext->Map(currentScene->canvasRT.tex, 0, D3D11_MAP_READ, 0, 0));
									D3D11_BOX box = {};
									box.left = mousePos.x;
									box.top = mousePos.y;
									box.right = box.left + 1;
									box.bottom = box.top + 1;
									box.back = 1;
									struct {
										u8 r, g, b, _;
									} pixel;
									d3d11State.device3->ReadFromSubresource(&pixel, 1, 1, currentScene->canvasRT.tex, 0, &box);
									d3d11State.immediateContext->Unmap(currentScene->canvasRT.tex, 0);
									currentScene->drawColor.x = pixel.r / 255.0f;
									currentScene->drawColor.y = pixel.g / 255.0f;
									currentScene->drawColor.z = pixel.b / 255.0f;
									currentScene->drawColorDirty = true;
								} else {
									LOG("How to read from texture on device1?");
								}
							}
						} break;
						case Tool_pencil:
						case Tool_line:
						case Tool_grid: {
							switch (currentAction.type) {
								case ActionType_none: break;
								case ActionType_pencil: {
									auto &pencil = currentAction.pencil;
									if (smoothMouseWorldPosChanged || thicknessChanged || currentScene->drawColorDirty) {
										if (pencil.popNextTime) {
											pencil.lines.pop_back();
											pencil.transformedLines.pop_back();
										}
										Line newLine = {};
						
										Point const *endpoint = 0;
										if (pencil.lines.size()) {
											Line lastLine = pencil.lines.back();
											newLine.a.position = lastLine.b.position;
											newLine.a.thickness = lastLine.b.thickness;
											// newLine.a.color = lastLine.b.color;
										} else {
											if (keyHeld(VK_CONTROL) && currentScene->postLastVisibleActionIndex) {
												endpoint = getEndpoint(currentScene->actions[currentScene->postLastVisibleActionIndex - 1]);
											}
											if (endpoint) {
												newLine.a.position = endpoint->position;
												newLine.a.thickness = endpoint->thickness;
												// newLine.a.color = endpoint->color;
											} else {
												newLine.a = pencil.newLineStartPoint;
											}
										}
						
										newLine.b.position = smoothMouseWorldPos;

										f32 endThickness = getDrawThickness(currentScene);
										f32 dist = distance(newLine.a.position, newLine.b.position) / endThickness;
										if (!endpoint) {
											// Pressure simulation on mouse
											// endThickness *= (1.0f / (max(0, dist - minPencilLineLength) * 0.5f + 1.0f));
										}
										pencil.popNextTime = dist < minPencilLineLength;

										// Corner smoothing, looks bad
#if 0
										u32 numSteps = 0;
										if (pencil.lines.size()) {
											numSteps = floorToInt(distance(smoothMouseWorldPos, newLine.a.position) / currentScene->cameraDistance / currentScene->drawThickness);
										}
										if (numSteps > 1) {
											Line lastLine = pencil.lines.back();
											v2f lastDir = normalize(lastLine.b.position - lastLine.a.position);
											v2f corner = lastLine.b.position + lastDir * dot(lastDir, newLine.b.position - newLine.a.position);
											v2f toCorner = corner - lastLine.b.position;
											v2f toEnd = smoothMouseWorldPos - corner;

											newLine.b = lastLine.b;
											for (u32 i = 1; i <= numSteps; ++i) {
												f32 t = (f32)i / numSteps;

												newLine.a = newLine.b;
												newLine.b.position = lastLine.b.position + toCorner*t + toEnd*t*t;
												newLine.b.color.r = lerp(lastLine.b.color.r, currentScene->drawColor.r, t);
												newLine.b.color.g = lerp(lastLine.b.color.g, currentScene->drawColor.g, t);
												newLine.b.color.b = lerp(lastLine.b.color.b, currentScene->drawColor.b, t);
												newLine.b.thickness = lerp(lastLine.b.thickness, endThickness, t);
												pushLine(pencil, newLine);
											}
							
											//release(pencil.lineBuffer);
											//pencil.lineBuffer = d3d11State.createStructuredBuffer(pencil.transformedLines.size(), sizeof(TransformedLine), pencil.transformedLines.data(), D3D11_USAGE_IMMUTABLE);

											u32 bufferElemCount = pencil.lineBuffer.size / sizeof(TransformedLine);
											if (pencil.transformedLines.size() > bufferElemCount) {
												release(pencil.lineBuffer);
												pencil.lineBuffer = d3d11State.createStructuredBuffer(bufferElemCount + pencilLineBufferDefElemCount, sizeof(TransformedLine), pencil.transformedLines.data(), D3D11_USAGE_DEFAULT);
											}
											d3d11State.updateStructuredBuffer(pencil.lineBuffer, numSteps, sizeof(TransformedLine), pencil.transformedLines.end() - numSteps, pencil.transformedLines.size() - numSteps);

											currentScene->needRepaint = true;
										} else 
#endif
										{
											// newLine.b.color = currentScene->drawColor;
											if (pencil.popNextTime)
												newLine.b.thickness = newLine.a.thickness;
											else
												newLine.b.thickness = endThickness;

											pushLine(pencil, newLine);

											u32 bufferElemCount = pencil.lineBuffer.size / sizeof(TransformedLine);
											if (pencil.transformedLines.size() > bufferElemCount) {
												release(pencil.lineBuffer);
												pencil.lineBuffer = d3d11State.createStructuredBuffer(bufferElemCount + pencilLineBufferDefElemCount, sizeof(TransformedLine), pencil.transformedLines.data(), D3D11_USAGE_DEFAULT);
											}
											d3d11State.updateStructuredBuffer(pencil.lineBuffer, 1, sizeof(TransformedLine), pencil.transformedLines.end() - 1, pencil.transformedLines.size() - 1);

											currentScene->needRepaint = true;
										}
									}
								} break;
								case ActionType_line: {
									auto &act = currentAction.line;
									bool updateLine = thicknessChanged || currentScene->drawColorDirty;
									if (currentScene->postLastVisibleActionIndex) {
										Point const *endpoint = 0;
										if (keyDown(VK_CONTROL)) {
											endpoint = getEndpoint(currentScene->actions[currentScene->postLastVisibleActionIndex - 1]);
										}
										if (endpoint) {
											act.line.a.position = endpoint->position;
											act.line.a.thickness = endpoint->thickness;
											currentScene->needRepaint = true;
											updateLine = true;
										} 
										if (keyUp(VK_CONTROL)) {
											act.line.a.position = act.initialPosition;
											act.line.a.thickness = act.initialThickness;
											currentScene->needRepaint = true;
											updateLine = true;
										}
									}
									if (updateLine || smoothMouseWorldPosChanged) {
										act.line.b.position = smoothMouseWorldPos;
										act.line.b.thickness = getDrawThickness(currentScene);
										// act.line.b.color = currentScene->drawColor;

										TransformedLine transformedLine = transform(act.line);
										d3d11State.updateStructuredBuffer(act.buffer, 1, sizeof(TransformedLine), &transformedLine);
										currentScene->needRepaint = true;
									}
								} break;
								case ActionType_grid: {
									auto &grid = currentAction.grid;
									bool update = thicknessChanged || gridCellCountChanged || currentScene->drawColorDirty || keyChanged(VK_CONTROL);
									if (update || smoothMouseWorldPosChanged) {
										if (keyHeld(VK_CONTROL)) {
											v2f dir = smoothMouseWorldPos - grid.startPoint.position;
											grid.endPosition = grid.startPoint.position + sign(dir) * max(absolute(dir.x), absolute(dir.y));
										} else {
											grid.endPosition = smoothMouseWorldPos;
										}
										grid.startPoint.thickness = getDrawThickness(currentScene);
										// grid.startPoint.color = currentScene->drawColor;

										List<TransformedLine> lines;
										lines.reserve(grid.cellCount * 2 + 2);
										for (u32 i = 0; i <= grid.cellCount; ++i) {
											Line line;
											// line.a.color = line.b.color = grid.startPoint.color;
											line.a.thickness = line.b.thickness = grid.startPoint.thickness;

											line.a.position.y = grid.startPoint.position.y;
											line.b.position.y = grid.endPosition.y;
											line.a.position.x = line.b.position.x = lerp(grid.startPoint.position.x, grid.endPosition.x, (f32)i / grid.cellCount);
											lines.push_back(transform(line));
							
											line.a.position.x = grid.startPoint.position.x;
											line.b.position.x = grid.endPosition.x;
											line.a.position.y = line.b.position.y = lerp(grid.startPoint.position.y, grid.endPosition.y, (f32)i / grid.cellCount);
											lines.push_back(transform(line));
										}
										d3d11State.updateStructuredBuffer(grid.buffer, lines.size(), sizeof(TransformedLine), lines.data());

										currentScene->needRepaint = true;
									}
								} break;
							}
						} break;
					}
				} 
				if (cursorUp()) {
					if (currentAction.type != ActionType_none) {
						switch (currentAction.type) {
							case ActionType_pencil: {
								auto &pencil = currentAction.pencil;
								pencil.transformedLines = {};
							} break;
							case ActionType_line: {
							} break;
							case ActionType_grid: {
								displayGridSize = 0;
								updateWindowText = true;
							} break;
						}
						if (currentScene->postLastVisibleActionIndex != currentScene->actions.size()) {
							for (umm i = currentScene->postLastVisibleActionIndex; i < currentScene->actions.size(); ++i) {
								cleanup(currentScene->actions[i]);
							}
							currentScene->actions.resize(currentScene->postLastVisibleActionIndex);
						}
						currentScene->actions.push_back(std::move(currentAction));
						currentScene->postLastVisibleActionIndex++;
						currentAction.reset();
					}
				}
			}
		}

		if (keyDown(VK_TAB)) {
			canvasPieMenu.opened = false;
			toolPieMenu.opened = false;
			colorMenuOpened = false;
			openPieMenu(mainPieMenu);
		}
		if (keyHeld(VK_TAB)) {
			needRepaint = true;
		}
		auto updatePieMenu = [&](PieMenu &menu) {
			menu.T = moveTowards(menu.T, (f32)menu.opened, 0.2f);
			menu.TChanged = menu.T != menu.prevT;
			menu.prevT = menu.T;
			f32 oldAlpha = menu.alpha;
			menu.alpha = moveTowards(menu.alpha, (f32)(distance((v2f)mousePosBL, menu.position) > min(windowSize.x, windowSize.y) * pieMenuSize * 0.5f), 0.1f);
			if (menu.TChanged || menu.alpha != oldAlpha) {
				needRepaint = true;
			}
			if (menu.opened) {
				if (keyDown(VK_ESCAPE)) {
					menu.opened = false;
				}
				if (cursorDown() || keyUp(VK_TAB)) {
					//ignoreCursorFrameCount = 1;

					menu.opened = false;
					if (distance((v2f)mousePosBL, menu.position) > min(windowSize.x, windowSize.y) * pieMenuSize * 0.5f) {
						f32 a = frac(map(atan2((v2f)mousePosBL - menu.position), -pi, pi, 0, 1) + 0.5f / menu.items.size()) * menu.items.size();
						
						mousePos = (v2s)menu.position;
						mousePos.y = windowSize.y - mousePos.y;

						setCursorPos(mousePos + windowPos);

						mousePosBL = mousePos;
						mousePosBL.y = windowSize.y - mousePos.y;

						for (u32 i = 0; i < menu.items.size(); ++i) {
							if (a <= i + 1) {
								menu.items[i].onSelect();
								break;
							}
						}
					}
				}
			}
		};

		static f32 prevColorMenuAlpha = colorMenuAlpha;
		colorMenuAlpha = moveTowards(colorMenuAlpha, (f32)colorMenuOpened, 0.125f);

		if (colorMenuAlpha != prevColorMenuAlpha) {
			needRepaint = true;
		}

		updatePieMenu(toolPieMenu);
		updatePieMenu(canvasPieMenu);
		updatePieMenu(mainPieMenu);
		
		if (smoothMouseWorldPosChanged) {
			currentScene->constantBufferData.mouseWorldPos = smoothMouseWorldPos;
			currentScene->constantBufferDirty = true;
		}

		if (windowResized) {
			windowResized = false;
			needRepaint = true;
			windowSizeDirty = true;
			resizeRenderTargets();
			currentScene->matrixSceneToNDCDirty = true;
		}

		if (mousePosChanged && mouseHovering) {
			needRepaint = true;
		}
		if (mouseExited) {
			currentScene->needRepaint = true;
		}

		repaint(mouseHovering && !hoveringColorMenu);
		
		if (updateWindowText) {
			updateWindowText = false;
			wchar buf[1024];
			if (currentScene->path.size()) {
				if (displayGridSize) {
					swprintf_s(buf, _countof(buf), localizations[language].windowTitle_path_gridSize, currentScene->filename, (u32)(currentScene - scenes), displayGridSize);
				} else {
					swprintf_s(buf, _countof(buf), localizations[language].windowTitle_path, currentScene->filename, (u32)(currentScene - scenes));
				}
			} else {
				if (displayGridSize) {
					swprintf_s(buf, _countof(buf), localizations[language].windowTitle_gridSize, (u32)(currentScene - scenes), displayGridSize);
				} else {
					swprintf_s(buf, _countof(buf), localizations[language].windowTitle, (u32)(currentScene - scenes));
				}
			}
			SetWindowTextW(mainWindow, buf);
		}
	}
	deinitThreadPool(&threadPool);
}
