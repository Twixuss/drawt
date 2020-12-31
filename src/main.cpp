// TODO: finish detection hovering entity
// TODO: entity reordering
// TODO: entity moving, rotating, scaling

#include "platform.h"
#include <variant>
#include <optional>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <random>

#include <stdio.h>

#include "../dep/stb/stb_image.h"

#define CURRENT_VERSION ((u16)0)

#include "../dep/tl/include/tl/common.h"
#include "../dep/tl/include/tl/list.h"
#include "../dep/tl/include/tl/math.h"
#include "../dep/tl/include/tl/thread.h"
#include "../dep/tl/include/tl/string.h"
#include "../dep/tl/include/tl/console.h"
#include "../dep/tl/include/tl/file.h"
#include "../dep/tl/include/tl/debug.h"
//#include "../dep/tl/include/tl/net.h"

#define memequ(a, b, s) (memcmp(a, b, s) == 0)
#define strequ(a, b) (strcmp(a, b) == 0)
#define wcsequ(a, b) (wcscmp(a, b) == 0)

#include "../dep/tl/include/tl/thread.h"

#include "renderer.h"

Entity *getEntityById(Scene *scene, EntityId id) {
	auto it = scene->entities.find(id);
	if (it == scene->entities.end())
		return 0;
	return &it->second;
}

enum ImageScalingAnchor {
	ImageScaling_left,
	ImageScaling_right,
	ImageScaling_top,
	ImageScaling_bottom,
	ImageScaling_topLeft,
	ImageScaling_topRight,
	ImageScaling_bottomLeft,
	ImageScaling_bottomRight,
};

#undef ACTIONS
#undef ENTITIES
#undef DECLARE_MEMBER
#undef DECLARE_CONSTRUCTOR
#undef CASE_DESTRUCTOR
#undef CASE_NEW_MOVE


//using NetMessage = u32;
//enum : NetMessage {
//	Net_startAction,
//	Net_updateAction,
//	Net_stopAction,
//};

struct LoadedImage {
	std::wstring path;
	u32 refCount;
	void *renderData;
	bool operator==(LoadedImage const &that) const {
		return path == that.path;
	}
};

namespace std {

template <>
struct hash<LoadedImage> {
	size_t operator()(LoadedImage const &image) const {
		return (size_t)image.renderData >> 3;
	}
};

}

static std::thread imageLoaderThread;
static std::unordered_map<std::wstring, LoadedImage> allLoadedImages;
static CircularQueue<LoadedImage *, 1024> imagesToLoad;
static Mutex loadedImagesMutex;

ThreadPool<TL_DEFAULT_ALLOCATOR> threadPool;

Language language;
Localization localizations[Language_count];

Renderer *renderer;

constexpr f32 minPencilLineLength = 4;

Scene scenes[10];
Scene *currentScene = scenes + 1;
Scene *previousScene;
u64 emptySceneHash;
Entity *currentEntity;
Action *currentAction;
//NetAction currentNetAction;
v2f cameraVelocity;
f32 oldCameraDistance = 1.0f;
f32 targetThickness = 16.0f;
f32 oldThickness = 16.0f;
bool playingSceneShiftAnimation;
f32 sceneShiftT;
v2u displayGridSize;
PieMenu mainPieMenu;
f32 pieMenuSize;
constexpr f32 cameraMoveSpeed = 0.5f;

Entity *draggingEntity = 0;
v2f draggingEntityOffset;

Entity *rotatingEntity = 0;
f32 rotatingEntityInitialAngle;

ImageEntity *scalingImage = 0;
v2f draggingPointUv;
ImageScalingAnchor scalingImageAnchor;

Entity *hoveredEntity = 0;
v2f hoveredImageUv;

bool cameraPanning;
f32 toolImageSize;

bool colorMenuOpened;
bool colorMenuSelectingH;
bool colorMenuSelectingSV;
f32 prevColorMenuAnimTime;
f32 colorMenuAnimTime;
f32 colorMenuAnimValue;
v2f colorMenuPositionTL;
f32 colorMenuHue;
ColorMenuTarget colorMenuTarget;

List<DebugPoint> debugPoints;

bool thicknessChanged;
bool mouseScenePosChanged;
bool smoothMousePosChanged;
bool cameraDistanceChanged;
bool cursorVisible;

bool previousMouseHovering = false;
v2f smoothMousePos = {};
v2f oldSmoothMousePos = {};
f32 smoothMousePosT = 1.0f;
f32 targetImageRotation = 0;
Entity *previousHoveredEntity = 0;

bool drawBounds;
bool debugPencil;

// net::Socket listener, connection;
// MutexCircularQueue<Action, 1024> receivedActions;

bool app_loadScene(Scene *, wchar const *);
void generateGfxData(Scene *);
u64 getSceneHash(Scene *scene);
void app_exitAbnormal();
void resizeRenderTargets();
bool proceedCloseScene();

void calculateBounds(EntityBase &e);

bool manipulatingEntity() {
	return draggingEntity || rotatingEntity || scalingImage;
}
bool smoothable(Tool tool) {
	return tool == Tool_pencil;
}

void ungetImageData(Span<wchar> path) {
	SCOPED_LOCK(loadedImagesMutex);
	auto &img = allLoadedImages.at(std::wstring(path.data(), path.size()));
	--img.refCount;
	if (!img.refCount) {
		renderer->releaseImageData(img.renderData);
	}
}
void *getOrLoadImageData(Span<wchar const> path) {
	SCOPED_LOCK(loadedImagesMutex);

	auto str = std::wstring(path.data(), path.size());
	auto &img = allLoadedImages[str];
	if (!img.refCount) {
		if (!img.path.size())
			img.path = str;
		img.renderData = renderer->createImageData();
		renderer->setUnloadedTexture(img.renderData);
		imagesToLoad.push(&img);
	}
	++img.refCount;
	return img.renderData;
}

void cleanup(Entity &e) {
	LOG("cleanup(%{%})", toString(e.type), e.id);
	renderer->releaseEntity(e);
	switch (e.type) {
		case Entity_image:
			ungetImageData(e.image.path);
			DEALLOCATE(TL_DEFAULT_ALLOCATOR, e.image.path.data());
			break;
		case Entity_pencil: 
		case Entity_line:   
		case Entity_grid:   
		case Entity_circle:
			break;

		default: INVALID_CODE_PATH();
	}
	e.reset();
}

Action *pushAction(Scene *scene, Action &&a) {
	if (scene->postLastVisibleActionIndex != scene->actions.size()) {
		for (umm i = scene->postLastVisibleActionIndex; i < scene->actions.size(); ++i) {
			auto &action = scene->actions[i];
			switch (action.type)
			{
			case Action_create: {
				auto &create = action.create;
				cleanup(scene->entities.at(create.targetId));
				scene->entities.erase(create.targetId);
			} break;
			case Action_translate:
			case Action_rotate:
			case Action_scale:
				break;
			default:
				INVALID_CODE_PATH();
				break;
			}
		}
		scene->actions.resize(scene->postLastVisibleActionIndex);
	}
	LOG("pushAction(scenes[%], %)", indexof(scene), toString(a.type));
	scene->actions.push_back(std::move(a));
	scene->postLastVisibleActionIndex++;
	scene->modifiedPostLastVisibleActionIndex = min(scene->modifiedPostLastVisibleActionIndex, scene->postLastVisibleActionIndex);
	scene->showAsterisk = true;
	updateWindowText = true;
	return &scene->actions.back();
}

Entity *pushEntity(Scene *scene, Entity &&e) {
	ASSERT(e.id == invalidEntityId, "pushEntity: Entity already registered");
	
	auto id = scene->entityIdCounter++;

	CreateAction create = {};
	create.targetId = id;
	pushAction(currentScene, std::move(create));
	
	e.id = id;
	e.visible = true;
	LOG("pushEntity(scenes[%], %{%})", indexof(scene), toString(e.type), id);
	scene->entities.emplace(id, std::move(e));

	// TODO: maybe dont search???
	return &scene->entities.at(id);
}

bool loadImageInfo(ImageEntity &image) {
	auto imageFile = _wfopen(nullTerminate(image.path).data(), L"rb");
	if (!imageFile) {
		LOGW(L"Failed to load file: %", image.path);
		return false;
	}
	DEFER { fclose(imageFile); };

	int width, height;
	if (!stbi_info_from_file(imageFile, &width, &height, 0)) {
		LOGW(L"Failed stbi_info_from_file: %", image.path);
		return false;
	}

	image.size.x = width;
	image.size.y = height;
	image.initialAspectRatio = image.size.x / image.size.y;
	return true;
}

void app_onWindowClose() {
	app_tryExit();
}
void app_onWindowResize() {
	if (renderer) {
		resizeRenderTargets();
		currentScene->matrixSceneToNDCDirty = true;
		windowSizeDirty = true;
		renderer->repaint(false, false);
	}
	
	toolImageSize = 1 << max(findHighestOneBit(minWindowDim / 16), 4);
	pieMenuSize = toolImageSize * 3;
}
void app_onWindowMove() {
	if (renderer) {
		needRepaint = true;
		renderer->repaint(false, false);
	}
}
void app_onDragAndDrop(Span<Span<wchar>> paths) {
	for (auto path : paths) {
		bool freePath = true;
		DEFER {
			if (freePath)
				DEALLOCATE(TL_DEFAULT_ALLOCATOR, path.data());
		};
		setToLowerCase(path);
		if (endsWith(path.data(), L".drawt")) {
			if (proceedCloseScene()) {
				app_loadScene(currentScene, path.data());
				generateGfxData(currentScene);
				updateWindowText = true;
			}
			break;
		} else if (endsWith(path.data(), L".png") ||
			       endsWith(path.data(), L".jpg") ||
			       endsWith(path.data(), L".jpeg") ||
			       endsWith(path.data(), L".bmp") ||
			       endsWith(path.data(), L".tga") ||
			       endsWith(path.data(), L".gif")) {

			freePath = false;

			ImageEntity image;
			image.path = path;
			if (loadImageInfo(image)) {
				image.size /= max(image.size.x, image.size.y);
				image.size *= currentScene->cameraDistance * min(clientSize.x, clientSize.y) * 0.5f;
				image.position = mouseScenePos;
				image.renderData = getOrLoadImageData(image.path);
				if (image.renderData) {
					currentScene->needRepaint = true;
				}
				
				calculateBounds(*pushEntity(currentScene, std::move(image)));
			}
		} else {
			LOGW(L"Unknown file format: %", path.data());
		}
	}
}

void initLocalization() {
	Localization *loc;

	loc = &localizations[Language_english];
	loc->windowTitlePrefix             = L"Drawt - Scene #% - ";
	loc->windowTitle_path_gridSize     = L"%% - Grid size: %x%";
	loc->windowTitle_path              = L"%%";
	loc->windowTitle_untitled_gridSize = L"*untitled* - Grid size: %x%";
	loc->windowTitle_untitled          = L"*untitled*";
	loc->warning                       = L"Warning!";
	loc->fileFilter                    = L"Drawt scene\0*.drawt\0";
	loc->saveFileTitle                 = L"Save Drawt Scene";
	loc->openFileTitle                 = L"Open Drawt Scene";
	loc->unsavedScene                  = L"This scene has unsaved changes. Discard?";
	loc->unsavedScenes                 = L"There are unsaved scenes. Exit?";
	loc->sceneAlreadyExists            = L"Scene with this name already exists. Overwrite?";

	loc = &localizations[Language_russian];
	loc->windowTitlePrefix             = L"Drawt - Сцена #% - ";
	loc->windowTitle_path_gridSize     = L"%% - Размер сетки: %x%";
	loc->windowTitle_path              = L"%%";
	loc->windowTitle_untitled_gridSize = L"*untitled* - Размер сетки: %x%";
	loc->windowTitle_untitled          = L"*untitled*";
	loc->warning                       = L"Внимание!";
	loc->fileFilter                    = L"Drawt сцена\0*.drawt\0";
	loc->saveFileTitle                 = L"Сохранить Drawt сцену";
	loc->openFileTitle                 = L"Открыть Drawt сцену";
	loc->unsavedScene                  = L"Эта сцена не сохранена. Продолжить?";
	loc->unsavedScenes                 = L"Не все сцены сохранены. Выйти?";
	loc->sceneAlreadyExists            = L"Сцена с таким именем уже существует. Перезаписать?";
}

void initGlobals() {
	initThreadPool(&threadPool, 4);

	emptySceneHash = getSceneHash(scenes + 0);
}

template <class Callback, class GetAction, class GetEntity, class OnActionAdded, class OnEntityAdded, class Revert>
bool traverseSceneSaveableData(Scene *scene, Callback &&callback, GetAction &&getAction, GetEntity &&getEntity, OnActionAdded &&onActionAdded, OnEntityAdded &&onEntityAdded, Revert &&revert, bool ignoreUnnecessary = false) {

#define CALLBACK(x, size) if (!callback(x, size, #x)) return false
#define VAR_CALLBACK(x) CALLBACK(&x, sizeof(x))

	constexpr u32 defaultSignature = 'twrd';
	u32 signature = defaultSignature;
	VAR_CALLBACK(signature);
	if (signature != defaultSignature) {
		LOG("This is not a .drawt file (signature missing)");
		return false;
	}
	
	auto version = CURRENT_VERSION;
	VAR_CALLBACK(version);
	if (version != CURRENT_VERSION) {
		LOG("Warning! This file was saved using another version of the program");
	}

	if (!ignoreUnnecessary) {
		VAR_CALLBACK(scene->cameraDistance);
		VAR_CALLBACK(scene->cameraPosition);
		VAR_CALLBACK(scene->tool);
		VAR_CALLBACK(scene->drawColor);
		VAR_CALLBACK(scene->windowDrawThickness);
	}
	VAR_CALLBACK(scene->postLastVisibleActionIndex);
	VAR_CALLBACK(scene->canvasColor); 
	VAR_CALLBACK(scene->entityIdCounter);
	for (u32 i = 0; i < scene->postLastVisibleActionIndex; ++i) {
		decltype(auto) a = getAction();
		VAR_CALLBACK(a.type);
		switch (a.type) {
			case Action_create: {
				auto &create = a.create;
				VAR_CALLBACK(create.targetId);
				decltype(auto) e = getEntity(create.targetId);
				VAR_CALLBACK(e.type);
				VAR_CALLBACK(e.position);
				VAR_CALLBACK(e.rotation);
				VAR_CALLBACK(e.visible);
				switch (e.type) {
					case Entity_pencil: {
						PencilEntity &pencil = e.pencil;
						VAR_CALLBACK(pencil.color);
						u32 lineCount = pencil.lines.size();
						VAR_CALLBACK(lineCount);
						if (lineCount == 0) {
							LOG("pencil.lines.size() is zero");
							return false;
						}
						if (pencil.lines.size() != lineCount) {
							pencil.lines.resize(lineCount);
						}
						auto lineData = pencil.lines.data();
						CALLBACK(lineData, lineCount * sizeof(Line));
					} break;
					case Entity_line: {
						LineEntity &line = e.line;
						VAR_CALLBACK(line.color);
						VAR_CALLBACK(line.line);
					} break;
					case Entity_grid: {
						GridEntity &grid = e.grid;
						VAR_CALLBACK(grid.color);
						VAR_CALLBACK(grid.thickness);
						VAR_CALLBACK(grid.size);
						VAR_CALLBACK(grid.cellCount.x);
						VAR_CALLBACK(grid.cellCount.y);
					} break;
					case Entity_circle: {
						CircleEntity &circle = e.circle;
						VAR_CALLBACK(circle.color);
						VAR_CALLBACK(circle.thickness);
						VAR_CALLBACK(circle.radius);
					} break;
					case Entity_image: {
						ImageEntity &image = e.image;
						VAR_CALLBACK(image.size);
						u16 len = image.path.size();
						VAR_CALLBACK(len);
						if (image.path.size() != len) {
							DEALLOCATE(TL_DEFAULT_ALLOCATOR, image.path.data());
							image.path = {ALLOCATE_T(TL_DEFAULT_ALLOCATOR, wchar, len, 0), len};
						}
						wchar *data = image.path.data();
						CALLBACK(data, len * sizeof(wchar));
					} break;
					default: 
						LOG("invalid entity");
						return false;
				}
				onEntityAdded(e);
			} break;
			case Action_translate: {
				auto &translate = a.translate;
				VAR_CALLBACK(translate.targetId);
				VAR_CALLBACK(translate.startPosition);
				VAR_CALLBACK(translate.endPosition);
			} break;
			case Action_rotate: {
				auto &rotate = a.rotate;
				VAR_CALLBACK(rotate.targetId);
				VAR_CALLBACK(rotate.startAngle);
				VAR_CALLBACK(rotate.endAngle);
			} break;
			case Action_scale: {
				auto &scale = a.scale;
				VAR_CALLBACK(scale.targetId);
				VAR_CALLBACK(scale.startPosition);
				VAR_CALLBACK(scale.endPosition);
				VAR_CALLBACK(scale.startSize);
				VAR_CALLBACK(scale.endSize);
			} break;
			default: 
				LOG("invalid action"); 
				return false;
		}
		onActionAdded(a);
	}
	return true;
}
bool equals(Entity const &ea, Entity const &eb) {
	if (ea.type != eb.type) return false;
	if (ea.position != eb.position) return false;
	if (ea.rotation != eb.rotation) return false;
	switch (ea.type) {
		case Entity_pencil: {
			auto &a = ea.pencil;
			auto &b = eb.pencil;
			if (!memequ(&a.color, &b.color, sizeof(a.color))) return false;
			if (a.lines.size() != b.lines.size()) return false;
			if (!memequ(a.lines.data(), b.lines.data(), sizeof(Line) * a.lines.size())) return false;
		} break;
		case Entity_line: {
			auto &a = ea.line;
			auto &b = eb.line;
			if (!memequ(&a.color, &b.color, sizeof(a.color))) return false;
			if (!memequ(&a.line, &b.line, sizeof(Line))) return false;
		} break;
		case Entity_grid: {
			auto &a = ea.grid;
			auto &b = eb.grid;
			if (!memequ(&a.color, &b.color, sizeof(a.color))) return false;
			if (!memequ(&a.thickness, &b.thickness, sizeof(a.thickness))) return false;
			if (!memequ(&a.size, &b.size, sizeof(a.size))) return false;
			if (!memequ(&a.cellCount, &b.cellCount, sizeof(a.cellCount))) return false;
		} break;
		case Entity_circle: {
			auto &a = ea.circle;
			auto &b = eb.circle;
			if (!memequ(&a.color, &b.color, sizeof(a.color))) return false;
			if (!memequ(&a.thickness, &b.thickness, sizeof(a.thickness))) return false;
			if (!memequ(&a.radius, &b.radius, sizeof(a.radius))) return false;
		} break;
		case Entity_image: {
			auto &a = ea.image;
			auto &b = eb.image;
			if (!memequ(&a.size, &b.size, sizeof(a.size))) return false;
			if (!equals(a.path, b.path)) return false;
		} break;
		default: INVALID_CODE_PATH();
	}
	return true;
}
bool equals(Scene *sceneA, Scene *sceneB) {
	if (sceneA->postLastVisibleActionIndex != sceneB->postLastVisibleActionIndex) return false;
	if (!memequ(&sceneA->canvasColor, &sceneB->canvasColor, sizeof(sceneA->canvasColor))) return false;
	for (u32 i = 0; i < sceneA->postLastVisibleActionIndex; ++i) {
		Action const &actionA = sceneA->actions[i];
		Action const &actionB = sceneB->actions[i];
		if(actionA.type != actionB.type)
			return false;
		switch (actionA.type) {
			case Action_create: {
				auto &a = actionA.create;
				auto &b = actionB.create;
				if (!memequ(&a.targetId, &b.targetId, sizeof(a.targetId))) return false;
				if (!equals(sceneA->entities.at(a.targetId), sceneB->entities.at(b.targetId))) return false;
			} break;
			case Action_translate: {
				auto &a = actionA.translate;
				auto &b = actionB.translate;
				if (!memequ(&a.targetId, &b.targetId, sizeof(a.targetId))) return false;
				if (!memequ(&a.startPosition, &b.startPosition, sizeof(a.startPosition))) return false;
				if (!memequ(&a.endPosition, &b.endPosition, sizeof(a.endPosition))) return false;
			} break;
			case Action_rotate: {
				auto &a = actionA.rotate;
				auto &b = actionB.rotate;
				if (!memequ(&a.targetId, &b.targetId, sizeof(a.targetId))) return false;
				if (!memequ(&a.startAngle, &b.startAngle, sizeof(a.startAngle))) return false;
				if (!memequ(&a.endAngle, &b.endAngle, sizeof(a.endAngle))) return false;
			} break;
			case Action_scale: {
				auto &a = actionA.scale;
				auto &b = actionB.scale;
				if (!memequ(&a.targetId, &b.targetId, sizeof(a.targetId))) return false;
				if (!memequ(&a.startPosition, &b.startPosition, sizeof(a.startPosition))) return false;
				if (!memequ(&a.endPosition, &b.endPosition, sizeof(a.endPosition))) return false;
				if (!memequ(&a.startSize, &b.startSize, sizeof(a.startSize))) return false;
				if (!memequ(&a.endSize, &b.endSize, sizeof(a.endSize))) return false;
			} break;
			default: INVALID_CODE_PATH();
		}
	}
	return true;
}

u64 getSceneHash(Scene *scene) {
	u64 result = 0x0123456789ABCDEF;
	u32 mixIndex = 0;
	auto mixer = [&](void *data, umm size, char const *name) {
		while (size >= 8) {
			result ^= *(u64 *)data;
			data = (u8 *)data + 8;
			size -= 8;
		}
		while (size >= 4) {
			((u32 *)&result)[mixIndex & 1] ^= *(u32 *)data;
			data = (u8 *)data + 4;
			size -= 4;
			++mixIndex;
		}
		while (size >= 2) {
			((u16 *)&result)[mixIndex & 3] ^= *(u16 *)data;
			data = (u8 *)data + 2;
			size -= 2;
			++mixIndex;
		}
		while (size) {
			((u8 *)&result)[mixIndex & 7] ^= *(u8 *)data;
			data = (u8 *)data + 1;
			size -= 1;
			++mixIndex;
		}
		return true;
	};
	auto actionIterator = scene->actions.begin();
	auto getAction = [&]() -> Action & { return *actionIterator++; };
	auto getEntity = [&](EntityId id) -> Entity & { return scene->entities.at(id); };
	auto empty = [](auto&){};
	auto revert = [](u32 amount) {
		INVALID_CODE_PATH("revert should never be called here");
	};
	if (!traverseSceneSaveableData(scene, mixer, getAction, getEntity, empty, empty, revert, true)) {
		INVALID_CODE_PATH("traverseSceneSaveableData should never fail here");
	}
	return result;
}

void initializeScene(Scene *scene) {
	scene->initialized = true;
	scene->savedHash = emptySceneHash;
	renderer->initScene(scene);
}

bool keyHeld(u32 k) { return keysHeld[k]; }
bool keyDown(u32 k) { return keysDown[k]; }
bool keyDownRep(u32 k) { return keysDownRep[k]; }
bool keyUp(u32 k) { return keysUp[k]; }
bool keyChanged(u32 k) { return keyDown(k) || keyUp(k); }
bool mouseButtonHeld(u32 k) { return mouseButtons[k]; }
bool mouseButtonDown(u32 k) { return mouseButtons[k] && !prevMouseButtons[k]; }
bool mouseButtonUp(u32 k) { return !mouseButtons[k] && prevMouseButtons[k]; }

v2f clientToScenePos(v2f screenPos) { return (screenPos - 0.5f * (v2f)clientSize) * currentScene->cameraDistance + currentScene->cameraPosition; }
v2f clientToScenePos(v2s screenPos) { return clientToScenePos((v2f)screenPos); }
v2f sceneToClientPos(v2f scenePos) { return (scenePos - currentScene->cameraPosition) / currentScene->cameraDistance + 0.5f * (v2f)clientSize; }
Optional<Point> getEndpoint(Scene *scene, EntityId excludeId) {
	if (scene->postLastVisibleActionIndex == 0)
		return {};

	Action const *action = scene->actions.end();

	while (1) {
		do {
			action--;
			if (action < scene->actions.begin())
				return {};
		} while (action->type != Action_create);
		if (action->create.targetId == excludeId)
			continue;
		auto &e = scene->entities.at(action->create.targetId);
		Point result;
		switch (e.type) {
			case Entity_pencil: 
				result = e.pencil.lines.back().b;
				break;
			case Entity_line: 
				result = e.line.line.b;
				break;
			case Entity_grid:
			case Entity_circle:
			case Entity_image: 
				continue;
			default: INVALID_CODE_PATH();
		}
		result.position += e.position;
		return result;
	}
}

void pushLine(PencilEntity &pencil, Line line) {
	pencil.lines.push_back(line);
	renderer->onLinePushed(pencil, line);
}

void resizeRenderTargets() {
	renderer->resize();

	for (auto &scene : scenes) {
		scene.needResize = true;
	}
}

void closeScene(Scene *scene) {
	for (auto &[id, e] : scene->entities) {
		cleanup(e);
	}
	scene->entities.clear();
	scene->actions.clear();
	scene->postLastVisibleActionIndex = 0;
	scene->needRepaint = true;
	scene->savedHash = emptySceneHash;
	scene->entityIdCounter = 0;
	scene->path = {};
}

StringBuilder<> app_writeScene(Scene *scene) {
	StringBuilder<> builder;
	auto appender = [&](void *data, umm size, char const *name) {
		builder.appendBytes(data, size);
		return true;
	};
	auto actionIterator = scene->actions.begin();
	auto entityIterator = scene->entities.begin();
	auto getAction = [&]() -> Action & { return *actionIterator++; };
	auto getEntity = [&](EntityId id) -> Entity & { return scene->entities.at(id); };
	auto empty = [](auto&){};
	auto revert = [](u32 amount) {
		INVALID_CODE_PATH("revert should never be called here");
	};
	if (!traverseSceneSaveableData(scene, appender, getAction, getEntity, empty, empty, revert)) {
		INVALID_CODE_PATH("traverseSceneSaveableData should never fail here");
	}
	
	scene->savedHash = getSceneHash(scene);
	
	scene->showAsterisk = false;
	scene->savedPostLastVisibleActionIndex = scene->postLastVisibleActionIndex;
	scene->modifiedPostLastVisibleActionIndex = scene->postLastVisibleActionIndex;
	updateWindowText = true;

	return builder;
}

void saveScene(Scene *scene) {
	if (scene->path.size()) {
		platform_saveScene(scene, scene->path.c_str());
	} else {
		saveSceneDialog(scene);
	}
}

bool readScene(Span<u8 const> data, Scene *dstScene) {
	Scene tempScene;
	auto reader = [&] (void *dst, umm size, char const *name) {
		if (size > data.size()) {
			LOG("Failed to read %", name);
			return false;
		}
		memcpy(dst, data.begin(), size);
		data._begin += size;
		return true;
	};
	auto getAction = [] { return Action{}; };
	auto getEntity = [] (EntityId id) { 
		return Entity(CreateEntity_zeroMemory, id);
	};
	auto onActionAdded = [&](Action &a){ tempScene.actions.push_back(std::move(a)); };
	auto onEntityAdded = [&](Entity &e){
		calculateBounds(e);
		tempScene.entities.emplace(e.id, std::move(e));
	};
	auto revert = [&](u32 amount) {
		data._begin -= amount;
	};
	if (!traverseSceneSaveableData(&tempScene, reader, getAction, getEntity, onActionAdded, onEntityAdded, revert)) {
		return false;
	}
	
	for (auto &[id, e] : dstScene->entities) {
		cleanup(e);
	}
	auto renderData = dstScene->renderData;
	*dstScene = std::move(tempScene);
	dstScene->renderData = renderData;
	dstScene->initialized = true;
	dstScene->savedHash = getSceneHash(dstScene);
	dstScene->savedPostLastVisibleActionIndex = dstScene->postLastVisibleActionIndex;
	dstScene->modifiedPostLastVisibleActionIndex = dstScene->postLastVisibleActionIndex;
	dstScene->showAsterisk = false;
	dstScene->needRepaint = true;
	return true;
}
bool app_loadScene(Scene *scene, wchar const *path) {
	auto file = readEntireFile(path);
	if (!file) {
		LOGW(L"Failed to open '%'", path);
		return false;
	}
	DEFER { free(file); };

	if (readScene({(u8 *)file.begin(), (u8 *)file.end()}, scene)) {
		scene->path = path;
		scene->filename = getFilename(scene->path);
		return true;
	} else {
		LOGW(L"Failed to load scene '%'", path);
		return false;
	}

}
void generateGfxData(Scene *scene) {
	scene->needRepaint = true;
	scene->constantBufferDirty = true;
	scene->drawColorDirty = true;
	scene->matrixSceneToNDCDirty = true;
	scene->targetCameraDistance = scene->cameraDistance;
	scene->targetCameraPosition = scene->cameraPosition;
	renderer->update(scene);

	colorMenuHue = rgbToHsv(scene->drawColor).x;

	for (auto &[id, e] : scene->entities) {
		renderer->initEntity(e);
		switch (e.type) {
			case Entity_pencil:
			case Entity_line:  
			case Entity_grid:  
			case Entity_circle:
				break;
			case Entity_image: {
				auto& image = e.image;
				image.renderData = getOrLoadImageData(image.path);
			} break;
		}
		renderer->initEntityData(scene, e);
	}
}

void loadSceneStressTest() {
	if (openSceneDialog(currentScene)) {
		generateGfxData(currentScene);
		showConsoleWindow();
		for (u32 i = 0; i < 1024; ++i) {
			u32 s = 0, l = 0, g = 0;
			{
				auto begin = std::chrono::high_resolution_clock::now();
				platform_saveScene(currentScene, currentScene->path.data());
				s = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - begin).count();
			}
			{
				auto begin = std::chrono::high_resolution_clock::now();
				if (!app_loadScene(currentScene, currentScene->path.data()))
					break;
				l = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - begin).count();
			}
			{
				auto begin = std::chrono::high_resolution_clock::now();
				generateGfxData(currentScene);
				g = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - begin).count();
			}
			LOG("s = %; l = %; g = %", s, l, g);
		}
	}
}

bool isUnsaved(Scene *scene) {
	if (scene->initialized) {
		if (scene->savedHash != getSceneHash(scene)) {
			return true;
		} else if (scene->path.size()) {
			Scene savedScene;
			if (!app_loadScene(&savedScene, scene->path.c_str())) {
				return true;
			}
			if (scene->savedHash != savedScene.savedHash) {
				return true;
			}
			if (!equals(scene, &savedScene)) {
				return true;
			}
		}
	}
	return false;
}
bool app_tryExit() {
	showCursor();
	for (auto &scene : scenes) {
		if (isUnsaved(&scene)) {
			currentScene = &scene;
			needRepaint = true;
			renderer->repaint(false, false);
			if (platform_messageBox(localizations[language].unsavedScenes, localizations[language].warning, MessageBoxType::warning)) {
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
void app_exitAbnormal() {
	bool unsavedScenes[countof(scenes)];
	bool hasUnsavedScenes = false;
	for (u32 i = 0; i < countof(scenes); ++i) {
		if (isUnsaved(scenes + i)) {
			hasUnsavedScenes = true;
			unsavedScenes[i] = true;
		} else {
			unsavedScenes[i] = false;
		}
	}
	if (hasUnsavedScenes) {
		platform_emergencySave(unsavedScenes);
	}
	running = false;
}

void openPieMenu(PieMenu &menu) {
	menu.opened = true;
	menu.position = (v2f)mousePosBL;
	menu.animTime = 0.0f;
	menu.handAlphaTime = 0.0f;
	menu.rightMouseReleased = false;
	currentScene->needRepaint = true;
}
void closePieMenu(PieMenu &menu) {
	menu.opened = false;
	currentScene->needRepaint = true;
	cursorVisible = false;
};
void openColorMenu(ColorMenuTarget target) {
	colorMenuOpened = true;
	colorMenuPositionTL = (v2f)mousePosTL;
	colorMenuTarget = target;
	renderer->onColorMenuOpen(target);
};

void pushPieMenuItem(PieMenu &menu, v2u uv, PieMenuItemType type, void (*onSelect)()) {
	PieMenuItem item;
	item.uv = uv;
	item.type = type;
	item.onSelect = onSelect;
	menu.items.push_back(item);
}

void start(Span<wchar *> args) {
	initGlobals();

	initLocalization();

	for (u32 i = 0; i < args.size(); ++i) {
		LOGW(L"arg[%]: %", i, args[i]);
	}

	createWindow();
	
	renderer = createRenderer();
	
	imageLoaderThread = std::thread([&]{
		while (running) {
			loopUntil([&] {
				lock(loadedImagesMutex);
				if (imagesToLoad.size()) {
					LoadedImage *image = imagesToLoad.front();
					imagesToLoad.pop();
					unlock(loadedImagesMutex);

					auto imageBuffer = readEntireFile(image->path.data());

					if (imageBuffer) {
						DEFER { free(imageBuffer); };

						int width, height;
						stbi_uc *pixels = 0;

						pixels = stbi_load_from_memory((stbi_uc *)imageBuffer.data(), imageBuffer.size(), &width, &height, 0, 4);
						
						if (pixels) {
							DEFER { stbi_image_free(pixels); };
							renderer->setTexture(image->renderData, pixels, width, height);
							currentScene->needRepaint = true;
						} else {							
							LOGW(L"%: stbi_load_from_memory failed: %", image->path.data(), stbi_failure_reason());
						}
					} else {
						LOGW(L"readEntireFile failed: %", image->path.data());
					}
					return true;
				} else {
					unlock(loadedImagesMutex);
				}
				return !running;
			});
		}
	});

	initializeScene(currentScene);
	
	if (args.size() > 1) {
		std::wstring initialScenePath = args[1];
		if (initialScenePath.size() && initialScenePath.back() == L'"')
			initialScenePath.pop_back();
		if (initialScenePath.size() && initialScenePath.front() == L'"')
			initialScenePath.erase(initialScenePath.begin());

		if (initialScenePath.size()) {
			app_loadScene(currentScene, initialScenePath.c_str());
			generateGfxData(currentScene);
		}
	}
	
	pushPieMenuItem(mainPieMenu, getUv(Tool_pencil), 0, [] { currentScene->tool = Tool_pencil; });
	pushPieMenuItem(mainPieMenu, getUv(Tool_line), 0, [] { currentScene->tool = Tool_line; });
	pushPieMenuItem(mainPieMenu, getUv(Tool_circle), 0, [] { currentScene->tool = Tool_circle; });
	pushPieMenuItem(mainPieMenu, getUv(Tool_grid), 0, [] { currentScene->tool = Tool_grid; });
	pushPieMenuItem(mainPieMenu, getUv(Tool_dropper), 0, [] { currentScene->tool = Tool_dropper; });
	pushPieMenuItem(mainPieMenu, getUv(Tool_hand), 0, [] { currentScene->tool = Tool_hand; });
	pushPieMenuItem(mainPieMenu, {3,0}, PieMenuItem_toolColor, [] { openColorMenu(ColorMenuTarget_draw); });
	pushPieMenuItem(mainPieMenu, {3,0}, PieMenuItem_canvasColor, [] { openColorMenu(ColorMenuTarget_canvas); });
}
void updateAsterisk(Scene *scene) {
	if (scene->savedPostLastVisibleActionIndex <= scene->modifiedPostLastVisibleActionIndex) {
		scene->showAsterisk = scene->postLastVisibleActionIndex != scene->savedPostLastVisibleActionIndex;
		updateWindowText = true;
	}
}
void undo() {
	if (!currentScene->postLastVisibleActionIndex)
		return;

	currentScene->postLastVisibleActionIndex--;
	currentScene->needRepaint = true;

	auto &action = currentScene->actions[currentScene->postLastVisibleActionIndex];
	switch (action.type) {
		case Action_create: {
			auto &create = action.create;
			auto target = getEntityById(currentScene, create.targetId);
			ASSERT(target, "bad create.targetId");
			target->visible = false;
			--currentScene->entityIdCounter;
		} break;
		case Action_translate: {
			auto &translate = action.translate;
			auto target = getEntityById(currentScene, translate.targetId);
			ASSERT(target, "bad translate.targetId");
			target->position = translate.startPosition;
			calculateBounds(*target);
		} break;
		case Action_rotate: {
			auto &rotate = action.rotate;
			auto target = getEntityById(currentScene, rotate.targetId);
			ASSERT(target, "bad rotate.targetId");
			target->rotation = rotate.startAngle;
			calculateBounds(*target);
		} break;
		case Action_scale: {
			auto &scale = action.scale;
			auto target = getEntityById(currentScene, scale.targetId);
			ASSERT(target, "bad scale.targetId");
			ASSERT(target->type == Entity_image, "scale.targetId can only refer to image");
			target->position   = scale.startPosition;
			target->image.size = scale.startSize;
			calculateBounds(*target);
		} break;
	}
	updateAsterisk(currentScene);
}
void redo() {
	if (currentScene->postLastVisibleActionIndex >= currentScene->actions.size())
		return;
	
	currentScene->postLastVisibleActionIndex++;
	currentScene->needRepaint = true;

	auto &action = currentScene->actions[currentScene->postLastVisibleActionIndex - 1];

	switch (action.type) {
		case Action_create: {
			auto &create = action.create;
			auto target = getEntityById(currentScene, create.targetId);
			ASSERT(target, "bad create.targetId");
			target->visible = true;
			++currentScene->entityIdCounter;
		} break;
		case Action_translate: {
			auto &translate = action.translate;
			auto target = getEntityById(currentScene, translate.targetId);
			ASSERT(target, "bad translate.targetId");
			target->position = translate.endPosition;
			calculateBounds(*target);
		} break;
		case Action_rotate: {
			auto &rotate = action.rotate;
			auto target = getEntityById(currentScene, rotate.targetId);
			ASSERT(target, "bad rotate.targetId");
			target->rotation = rotate.endAngle;
			calculateBounds(*target);
		} break;
		case Action_scale: {
			auto &scale = action.scale;
			auto target = getEntityById(currentScene, scale.targetId);
			ASSERT(target, "bad scale.targetId");
			ASSERT(target->type == Entity_image, "scale.targetId can only refer to image");
			target->position   = scale.endPosition;
			target->image.size = scale.endSize;
			calculateBounds(*target);
		} break;
	}
	updateAsterisk(currentScene);
}

void calculateBounds(EntityBase &e) {
	e.bounds.min = V2f(+INFINITY);
	e.bounds.max = V2f(-INFINITY);
	
	auto upd = [&] (v2f p) {
		e.bounds.min = min(e.bounds.min, p);
		e.bounds.max = max(e.bounds.max, p);
	};

	auto updr = [&] (v2f p, f32 r) {
		e.bounds.min = min(e.bounds.min, p - r);
		e.bounds.max = max(e.bounds.max, p + r);
	};

	m2 rotation = m2::rotation(-e.rotation);
	switch (e.type)
	{
		case Entity_pencil: {
			auto &pencil = *(PencilEntity*)&e;
			for (auto &l : pencil.lines) {
				updr(rotation * l.a.position, l.a.thickness * 0.5f);
				updr(rotation * l.b.position, l.b.thickness * 0.5f);
			}
			e.bounds.min += pencil.position;
			e.bounds.max += pencil.position;
		} break;
		case Entity_line: {
			auto &line = *(LineEntity*)&e;
			updr(line.position + rotation * line.line.a.position, line.line.a.thickness * 0.5f);
			updr(line.position + rotation * line.line.b.position, line.line.b.thickness * 0.5f);
		} break;
		case Entity_grid: {
			auto &grid = *(GridEntity*)&e;
			minmax(grid.position, grid.position + rotation * grid.size, e.bounds.min, e.bounds.max);
			e.bounds.min -= grid.thickness * 0.5f;
			e.bounds.max += grid.thickness * 0.5f;
		} break;
		case Entity_circle: {
			// This calculation considers only corner points.
			// Is is not accurate, but it is a lot faster and
			// works not so bad
			auto &circle = *(CircleEntity*)&e;
			v2f corners[4] {
				{+circle.radius.x,+circle.radius.y},
				{+circle.radius.x,-circle.radius.y},
				{-circle.radius.x,+circle.radius.y},
				{-circle.radius.x,-circle.radius.y},
			};
			for (auto c : corners) {
				upd(rotation * c);
			}
			e.bounds.min += circle.position;
			e.bounds.max += circle.position;
			e.bounds.min -= circle.thickness * 0.5f;
			e.bounds.max += circle.thickness * 0.5f;
		} break;
		case Entity_image: {
			auto &image = *(ImageEntity*)&e;
			v2f halfSize = image.size * 0.5f;
			v2f corners[4] {
				{+halfSize.x,+halfSize.y},
				{+halfSize.x,-halfSize.y},
				{-halfSize.x,+halfSize.y},
				{-halfSize.x,-halfSize.y},
			};
			for (auto c : corners) {
				upd(rotation * c);
			}
			e.bounds.min += image.position;
			e.bounds.max += image.position;
		} break;
		default:
			INVALID_CODE_PATH();
			break;
	}
}

void findHoveredEntity() {
	if (!hoveredEntity) {
		List<Entity *> candidates;
		for (auto &[id, e] : currentScene->entities) {
			if (e.visible && inBounds(mouseScenePos, e.bounds)) {
				candidates.push_back(&e);
			}
		}
		std::sort(candidates.begin(), candidates.end(), [&](Entity *a, Entity *b) {
			return a->id > b->id;
		});
		for (auto ptr : candidates) {
			auto &e = *ptr;
			auto mouseRelativePos = m2::rotation(e.rotation) * (mouseScenePos - e.position) + e.position;
			auto hovering = [&](Line l, v2f offset) {
				auto a = l.a.position + offset;
				auto b = l.b.position + offset;
				auto p = mouseRelativePos;
				f32 t = dot(normalize(a - b), a - p) / length(a - b);

				if (t > 1) {
					return distance(p, b) < l.b.thickness * 0.5f;
				} else if (t < 0) {
					return distance(p, a) < l.a.thickness * 0.5f;
				} else {
					v2f c = cross(normalize(a - b));
					f32 d = absolute(dot(c, a - p));
					return d < lerp(l.a.thickness, l.b.thickness, clamp(t, 0, 1)) * 0.5f;
				}
			};

			switch (e.type) {
				case Entity_image: {
					ImageEntity &image = e.image;
					auto uv = map(mouseRelativePos, image.position - image.size * 0.5f, image.position + image.size * 0.5f, 0, 1);
					if (inBounds(uv, aabbMinMax(V2f(0), V2f(1)))) {
						hoveredImageUv = uv;
						hoveredEntity = &e;
						return;
					}
				} break;
				case Entity_pencil: {
					PencilEntity &pencil = e.pencil;
					for (auto l : pencil.lines) {
						if (hovering(l, pencil.position)) {
							hoveredEntity = &e;
							return;
						}
					}
				} break;
				case Entity_line: {
					LineEntity &line = e.line;
					if (hovering(line.line, line.position)) {
						hoveredEntity = &e;
						return;
					}
				} break;
				case Entity_grid: {
					GridEntity &grid = e.grid;
					auto lines = getGridLines(grid);
					for (auto l : lines) {
						if (hovering(l, grid.position)) {
							hoveredEntity = &e;
							return;
						}
					}
				} break;
				case Entity_circle: {
					CircleEntity &circle = e.circle;
					auto lines = getCircleLines(circle);
					for (auto l : lines) {
						if (hovering(l, circle.position)) {
							hoveredEntity = &e;
							return;
						}
					}
				} break;
				default: {
					INVALID_CODE_PATH();
				} break;
			}
		}
	}
}

//void openImageContextMenu(ImageAction &image) {
//
//}

bool proceedCloseScene() {
	bool proceed = true;
	if (isUnsaved(currentScene)) {
		showCursor();
		proceed = platform_messageBox(localizations[language].unsavedScene, localizations[language].warning, MessageBoxType::warning);
		hideCursor();
	}
	return proceed;
}

void app_onKeyDown(u8 key) {
	keysDown[key] = true;
	keysHeld[key] = true;
	if (!draggingEntity && !rotatingEntity && !scalingImage && !cameraPanning) {
		if (keyHeld(Key_control)) {
			if (key == 'S') {
				if (keyHeld(Key_shift)) {
					saveSceneDialog(currentScene);
				} else {
					saveScene(currentScene);
				}
			} else if (key == 'O') {
				resetKeys = true;
				if (proceedCloseScene()) {
					if (openSceneDialog(currentScene)) {
						generateGfxData(currentScene);
						updateWindowText = true;
					}
				}
			} else if (key == 'W') {
				bool close = true;
				if (isUnsaved(currentScene)) {
					showCursor();
					close = platform_messageBox(localizations[language].unsavedScene, localizations[language].warning, MessageBoxType::warning);
					hideCursor();
				}
				if (close) {
					closeScene(currentScene);
					updateWindowText = true;
				}
			}
		} else {
			if (key == Key_tab) {
				colorMenuOpened = false;
				if (mainPieMenu.opened) {
					closePieMenu(mainPieMenu);
				} else {
					if (!draggingEntity && !scalingImage && !rotatingEntity && !keyHeld(Key_shift)) {
						openPieMenu(mainPieMenu);
					}
				}
			} else if (key == Key_f1) {
				toggleConsoleWindow();
			} else if (key == Key_f2) {
				if (isDebuggerAttached())
					DEBUG_BREAK;
			} else if (key == Key_f3) {
				renderer->switchRasterizer();
				currentScene->needRepaint = true;
			} else if (key == Key_f4) {
				renderer->setMultisampleEnabled(!renderer->isMultisampleEnabled());
				currentScene->needRepaint = true;
			} else if (key == Key_f5) {
				loadSceneStressTest();
			} else if (key == Key_f6) {
				drawBounds = !drawBounds;
			} else if (key == Key_f7) {
				debugPencil = !debugPencil;
			//} else if (key == Key_f8) {
			//	renderer->debugSaveRenderTarget();
#if 0
			} else if (key == Key_f8) {
				if (net::init()) {
					listener = net::createSocket();
					if (listener) {
						net::bind(listener, 12345);
						net::listen(listener);
						std::thread([&] {
							auto server = net::createServer(listener);
							server->onClientConnected = [](net::Server *server, void *context, net::Socket s, char *host, char *service, u16 port) {
								LOG("New connection: '%', '%', '%', '%'", s, host, service, port);
							};
							server->onClientDisconnected = [](net::Server *server, void *context, net::Socket s) {
								LOG("Disconnected: '%'", s);
							};
							server->onClientMessageReceived = [](net::Server *server, void *context, net::Socket s, u8 *data, u32 size) {
								LOG("Client message received (% bytes)", size);

#define READ(x) x = *(decltype(x) *)data; data += sizeof(x); size -= sizeof(x)

								while (size) {
									NetMessage messageType;
									READ(messageType);
									switch (messageType)
									{
									case Net_startAction: {
										SCOPED_LOCK(renderer->getMutex());
										SCOPED_LOCK(currentNetAction.mutex);
										PencilAction pencil = {};
										renderer->initPencilAction(pencil);
										READ(pencil.color);
										renderer->initDynamicLineArray(pencil.renderData, 0, pencilLineBufferDefElemCount);
										currentNetAction.action = std::move(pencil);
										registerAction(currentScene, currentNetAction.action);
									} break;
									case Net_updateAction: {
										SCOPED_LOCK(renderer->getMutex());
										SCOPED_LOCK(currentNetAction.mutex);
										auto &pencil = currentNetAction.action.pencil;
										Line line;
										READ(line);
										pushLine(pencil, line);
										renderer->resizePencilLineArray(pencil);
										renderer->updateLastElement(pencil);
										currentScene->needRepaint = true;
									} break;
									case Net_stopAction: {
										SCOPED_LOCK(currentNetAction.mutex);
										switch (currentNetAction.action.type) {
											case Action_pencil: {
												auto &pencil = currentNetAction.action.pencil;
												renderer->freeze(pencil);
												receivedActions.push(std::move(currentNetAction.action));
											} break;
											case Action_grid:
											case Action_line: 
											case Action_circle:
												break;
											default: INVALID_CODE_PATH();
										}
									} break;
									default:
										break;
									}
								}
							};
							net::run(server);
						}).detach();
						LOG("Server initialized");
					} else {
						LOG("net::createSocket() failed");
					}
				} else {
					LOG("net::init() failed");
				}
			} else if (key == Key_f9) {
				if (net::init()) {
					connection = net::createSocket();
					if (connection) {
						if (net::connect(connection, "127.0.0.1", 12345)) {
							LOG("Connected");
						} else {
							LOG("net::connect() failed");
						}
					} else {
						LOG("net::createSocket() failed");
					}
				} else {
					LOG("net::init() failed");
				}
#endif
			} else if (key >= '0' && key <= '9') {
				if (!currentEntity) {
					u32 newIndex = key - '0';
					u32 currentIndex = currentScene - scenes;
					if (newIndex != currentIndex) {
						previousScene = currentScene;
						currentScene = scenes + newIndex;
						if (!currentScene->initialized)
							initializeScene(currentScene);
						thicknessChanged = true;
						cameraDistanceChanged = true;
						mouseScenePosChanged = true;
						playingSceneShiftAnimation = true;
						sceneShiftT = 0.0f;
						updateWindowText = true;
					}
				}
			}
		}
	}
	if (key == Key_escape) {
		colorMenuOpened = false;
	}
}
void app_onKeyDownRepeated(u8 key) {
	keysDownRep[key] = true;
	if (!draggingEntity && !rotatingEntity && !scalingImage) {
		if (key == 'Z') {
			if (keyHeld(Key_control)) {
				if (!currentEntity) {
					if (keyHeld(Key_shift)) {
						redo();
					} else {
						undo();
					}
				}
			}
		}
	}
}
void app_onKeyUp(u8 key) {
	keysUp[key] = true;
	keysHeld[key] = false;
	if (key == Key_tab) {
		if (mainPieMenu.opened) {
			mainPieMenu.rightMouseReleased = true;
		}
	}
}

void app_onMouseDown(u8 button) {
	mouseButtons[button] = true;
	if (button == mouseButton_middle) {
		cameraPanning = true;
	}
}
void app_onMouseUp(u8 button) {
	mouseButtons[button] = false;
	if (button == mouseButton_middle) {
		cameraPanning = false;
	}
}

struct f32c {
	static constexpr u32 exponentBias = 127;
	static constexpr u32 exponentMask = 0xFF;
	static constexpr u32 mantissaMask = 0x7FFFFF;
	static constexpr u32 mantissaHighestBit = 0x800000;
	static constexpr u32 mantissaCarryBit = 0x1000000;
	static constexpr u32 biasedZero = 0x81;
	static constexpr u32 biasedOne = 0x82;

	struct s {
		s32 exponent;
		u32 mantissa;
		bool negative;
		bool isNan;
		bool isInfinity;
	};
	inline static s getS(f32c v) {
		// Step 2:
		// Extract the sign, the biased exponent, and the mantissa. 
		// Add any implicit leading bit to the mantissa.
		// Check whatever special rules there are: 
		// In the IEEE 754 32 and 64 bit format, a biased exponent of 0 means that there is no implicit leading bit in the mantissa,
		// but the exponent should be treated as if it was 1. Add three extra bits containing zeroes to the mantissa.
		s result = {};
		result.exponent = (v.u >> 23) & exponentMask;
		result.mantissa = v.u & mantissaMask;
		result.negative = v.u >> 31;

		if (result.exponent) {
			if (result.exponent == exponentMask) {
				if (result.mantissa) {
					result.isNan = true;
					return result;
				} else {
					result.isInfinity = true;
					return result;
				}
			}
			result.mantissa |= 0x800000;
			result.exponent -= exponentBias;
		} else {
			result.exponent = result.mantissa == 0 ? 129 : 1;
		}
		result.mantissa <<= 3;
		return result;
	}
	inline static f32c make(s s) {
		// Step 7:
		// Perform rounding according to IEEE 754 rules.
		// That's where the three extra mantissa bits are used. Remove the extra 3 mantissa bits.
		// Due to the "round to even" rule, you round up (increase the mantissa by 1) if the last bit is 0 and the bits removed are 101 or larger,
		// or the last bit is 1 and the bits removed are 100 or larger.
		u32 lastBits = s.mantissa & 0x7;
		s.mantissa >>= 3;
		bool roundedUp = false;
		if (s.mantissa & 1) {
			if (lastBits >= 4) {
				++s.mantissa;
				roundedUp = true;
			}
		} else {
			if (lastBits >= 5) {
				++s.mantissa;
				roundedUp = true;
			}
		}

		// Step 8:
		// If you rounded up and there is a carry then shift the mantissa one bit to the right and increase the exponent.
		// If the biased exponent is too large then the result is infinity.
		// If the biased exponent is 1 and the highest mantissa bit is zero then the biased exponent is changed to 0.
		if (roundedUp) {
			if (s.mantissa & mantissaCarryBit) {
				s.mantissa >>= 1;
				++s.exponent;
				if ((s.exponent == 1) && ((s.mantissa & mantissaHighestBit) != mantissaHighestBit)) {
					s.exponent = biasedZero;
				}
			}
		}
		
		// Step 9:
		// Drop the highest bit of the mantissa, and pack sign, biased exponent, and mantissa together. 
		f32c r;
		r.u = ((u32)s.negative << 31) | ((u32)((s.exponent + exponentBias) & exponentMask) << 23) | (s.mantissa & mantissaMask);
		return r;
	}

	union {
		u32 u;
		f32 f;
	};
	f32c() {}
	f32c(f32 f) : f(f) {}
	friend f32c operator+(f32c a, f32c b) {
	beginning:
		f32c check = a.f + b.f;
		
		// Step 1: 
		// Determine if any of the operands is an Infinity or a Not-A-Number. 
		// In this case, look up the IEEE 754 standard and determine the result accordingly.


		s as = getS(a);
		s bs = getS(b);
		s cs = getS(check);

		if (as.isNan || bs.isNan) {
			return NAN;
		}

		//ASSERT(make(as).u == a.u);
		//ASSERT(make(bs).u == b.u);
		//ASSERT(make(cs).u == check.u);
		
		// Step 3:
		// Make sure the exponents are the same.
		// If one number has an exponent that is smaller by k, then shift that numbers mantissa to the right by k bit positions.
		// If k is greater than 3, then set the last bit of the mantissa to 1 if any non-zero bits were shifted out. 
		// If k is so large that all mantissa bits would be shifted out then set the mantissa to zero, and don't try to shift by some huge amount.
		if (as.exponent != bs.exponent) {
			s *min, *max;
			if (as.exponent > bs.exponent) {
				min = &bs;
				max = &as;
			} else {
				min = &as;
				max = &bs;
			}

			u32 k = max->exponent - min->exponent;
			if (k >= 27) {
				min->mantissa = 0;
			} else if (k > 3) {
				bool hasBits = min->mantissa & ((1 << k) - 1);
				min->mantissa >>= k;
				if (hasBits) {
					min->mantissa |= 1;
				}
			} else {
				min->mantissa >>= k;
			}
			min->exponent = max->exponent;
		}

		// Step 4: 
		// If the operands have different signs and the second mantissa is larger then exchange the operands. 
		// Add or subtract the mantissas, depending on the sign bits.
		s rs;
		if (as.negative != bs.negative && bs.mantissa > as.mantissa) {
			std::swap(as, bs);
		}
		if (as.negative == bs.negative) {
			rs.mantissa = as.mantissa + bs.mantissa;
		} else {
			if (as.mantissa == bs.mantissa && as.exponent == bs.exponent) {
				return 0.0f;
			}
			rs.mantissa = as.mantissa - bs.mantissa;
		}
		rs.negative = as.negative;

		// Step 5: 
		// If you added the mantissas and there was a carry, then increase the exponent, and shift the mantissa one bit position to the right.
		// If the bit that is shifted out is non-zero, then set the last bit of the mantissa.
		rs.exponent = as.exponent;
		if (rs.mantissa & (mantissaCarryBit << 3)) {
			++rs.exponent;
			u32 lastBit = rs.mantissa & 1;
			rs.mantissa >>= 1;
			rs.mantissa |= lastBit;
		}

		// Step 6:
		// If you subtracted the mantissas then count the number of leading zero bits in the mantissa.
		// If all mantissa bits are zero then set the biased exponent to 1.
		// Otherwise, shift the mantissa to the left and decrease the exponent, making sure that the biased exponent doesn't get smaller than 1.
		if (as.negative != bs.negative) {
			u32 lzc = countLeadingZeros(rs.mantissa) - 5;
			if (rs.mantissa == 0) {
				rs.exponent = 129;
			} else if (lzc) {
				if (rs.exponent > lzc) {
					rs.mantissa <<= lzc;
					rs.exponent -= lzc;
				} else {
					rs.mantissa <<= lzc - rs.exponent;
					rs.exponent = 1;
				}
			}
		}

		f32c result = make(rs);

		if (result.u != check.u) {
			DEBUG_BREAK;
			goto beginning;
		}

		return result;
	}
};

void f32test() {
	f32c fa;
	f32c fb;
	f32c fc;
	fa = -522975.969f;
	fb = 66674.0625f;
	fc = fa + fb;
	fa = -321593.813;
	fb = 323737.875;
	fc = fa + fb;
	fa = 124659.813;
	fb = -124659.813;
	fc = fa + fb;
	fa = -1.95598008e+10f;
	fb = 4.73311009e-18f;
	fc = fa + fb;
	fa = -8.23324077e-20f;
	fb = 1.01678475e-12f;
	fc = fa + fb;

	std::mt19937 mt{};
	for (u32 i = 0; i < 0x1000; ++i) {
		fa.u = mt();
		fb.u = mt();
		fc = fa + fb;
	}
}

int wmain(int argc, wchar **argv) {
	Span<wchar *> args = {argv, (umm)argc};
	platform_init();
	DEFER { platform_deinit(); };

	start(args);

	//f32test();

	while (running) {
		mouseDelta = {};
		mouseWheel = {};
		memset(keysDownRep, 0, sizeof(keysDownRep));
		memset(keysDown, 0, sizeof(keysDown));
		memset(keysUp, 0, sizeof(keysUp));
		memcpy(prevMouseButtons, mouseButtons, sizeof(mouseButtons));

		thicknessChanged = false;
		mouseScenePosChanged = false;
		smoothMousePosChanged = false;
		cameraDistanceChanged = false;

		hoveredEntity = 0;
		exitSizeMove = false;
		lostFocus = false;

		platform_beginFrame();

		if (!running)
			break;
		resetKeys |= lostFocus || exitSizeMove;
		if (resetKeys) {
			resetKeys = false;
			memset(keysDown    , 0, sizeof(keysDown));
			memset(keysUp      , 0, sizeof(keysUp));
			memset(keysHeld    , 0, sizeof(keysHeld));
			memset(mouseButtons, 0, sizeof(mouseButtons));
		}

		needRepaint = exitSizeMove;
		
		prevMousePos = mousePosTL;

		mousePosTL = getMousePos();
		
		
		f32 colorMenuSize = min(clientSize.x, clientSize.y) * 0.2f;
		v2f cursorRelativeColorMenu = ((v2f)mousePosTL - colorMenuPositionTL) / colorMenuSize;

		auto containsTriangle = [](v2f point, f32 size) {
			return 
				dot(point, v2f{ 0,                    1   }) <= 0.5f * size &&
				dot(point, v2f{ cosf(radians(30.0f)),-0.5f}) <= 0.5f * size &&
				dot(point, v2f{-cosf(radians(30.0f)),-0.5f}) <= 0.5f * size;
		};

		// inBounds((v2f)mousePosBL, aabbMinDim(colorMenuPosition, V2f(colorMenuSize)));
		bool hoveringColorMenu = colorMenuOpened && containsTriangle(cursorRelativeColorMenu, 1.0f);

		mousePosBL.x = mousePosTL.x;
		mousePosBL.y = clientSize.y - mousePosTL.y;
		
		if (debugPencil) {
			static f32 time = 0;
			static f32 printTimer = 0;
			static u32 lineCounter = 0;
			static auto previousTimePoint = std::chrono::high_resolution_clock::now();
			smoothMousePosChanged = true;
			mouseScenePosChanged = true;
			mousePosBL = (v2s)(v2f{cosf(time), sinf(time)} * map(sinf(time * sqrt2), -1, 1, 0, minWindowDim * 0.49f) + (v2f)clientSize * 0.5f);
			f32 delta = 0.1f;
			time += delta;
			++lineCounter;
			auto currentTimePoint = std::chrono::high_resolution_clock::now();
			printTimer += (currentTimePoint - previousTimePoint).count() / 1000000000.0;
			if (printTimer >= 1) {
				printTimer -= 1;
				LOG("lines added: %", lineCounter);
				lineCounter = 0;
			}
			previousTimePoint = currentTimePoint;
			mouseButtons[mouseButton_left] = true;
		}
		renderer->setVSync(!debugPencil);

		mouseDelta = mousePosTL - prevMousePos;
		
		bool mouseHovering = inBounds(mousePosTL, aabbMinMax(v2s{}, (v2s)clientSize));
		bool mouseExited = !mouseHovering && previousMouseHovering;

		cursorVisible = !mouseHovering;

		bool gridCellCountChanged = false;
		if (!hoveringColorMenu && mouseWheel) {
			if (draggingEntity) {
				/*
				if (keyHeld(Key_control)) {
					u32 draggingImageIndex = (u32)((Action *)draggingImage - currentScene->actions.data());
					u32 swapImageIndex = draggingImageIndex - sign(mouseWheel);
					if (swapImageIndex < currentScene->postLastVisibleActionIndex) {
						std::swap(currentScene->actions[draggingImageIndex], currentScene->actions[swapImageIndex]);
						draggingImage = &currentScene->actions[swapImageIndex].image;
					}
				} else {
					f32 t = pow(1.1f, mouseWheel);
					draggingImage->size *= t;
				}
				*/
			} else if (!rotatingEntity) {
				if (keyHeld(Key_control)) {
					if (currentEntity) {
						switch (currentEntity->type) {
							case Entity_grid: {
								u32 dimIndex = !keyHeld(Key_shift);
								displayGridSize[dimIndex] = 
									currentEntity->grid.cellCount[dimIndex] = 
									(u32)max((s32)currentEntity->grid.cellCount[dimIndex] + mouseWheel, 1);
								updateWindowText = true;
								renderer->reinitDynamicLineArray(
									currentEntity->grid.renderData, 0, 
									currentEntity->grid.cellCount.x + currentEntity->grid.cellCount.y + 2
								);
								gridCellCountChanged = true;
							} break;
						}
					}
				} else if (keyHeld(Key_shift)) {
					targetThickness = clamp(targetThickness / pow(1.1f, mouseWheel), 4, maxWindowDim);
				} else {
					f32 oldTargetCameraDistance = currentScene->targetCameraDistance;
					currentScene->targetCameraDistance /= pow(1.1f, mouseWheel);
					currentScene->targetCameraDistance = clamp(currentScene->targetCameraDistance, 1.0f / 8, 64);
					currentScene->targetCameraPosition = lerp(mouseScenePos, currentScene->targetCameraPosition, currentScene->targetCameraDistance / oldTargetCameraDistance);
				}
			}
		}
		oldCameraDistance = currentScene->cameraDistance;
		if (distance(currentScene->cameraDistance, currentScene->targetCameraDistance) < currentScene->cameraDistance * 0.001f) {
			currentScene->cameraDistance = currentScene->targetCameraDistance;
		} else {
			currentScene->cameraDistance = lerp(currentScene->cameraDistance, currentScene->targetCameraDistance, cameraMoveSpeed);
		}

		oldThickness = currentScene->windowDrawThickness;
		if (distance(currentScene->windowDrawThickness, targetThickness) < currentScene->cameraDistance * 0.001f) {
			currentScene->windowDrawThickness = targetThickness;
		} else {
			currentScene->windowDrawThickness = lerp(currentScene->windowDrawThickness, targetThickness, 0.5f);
		}


		cameraDistanceChanged |= currentScene->cameraDistance != oldCameraDistance;
		thicknessChanged |= (currentScene->windowDrawThickness != oldThickness) || cameraDistanceChanged;
		bool mousePosChanged = mouseDelta.x || mouseDelta.y;
		mouseScenePosChanged |= mousePosChanged || cameraDistanceChanged;

		if (playingSceneShiftAnimation) {
			sceneShiftT = lerp(sceneShiftT, 1.0f, 0.3f);
			needRepaint = true;
			if (1.0f - sceneShiftT < 0.001f) {
				sceneShiftT = 0;
				playingSceneShiftAnimation = false;
			}
		}

		if (cameraPanning) {
			cameraVelocity = -(v2f)mouseDelta * currentScene->cameraDistance;
			cameraVelocity.y = -cameraVelocity.y;
		} else {
			cameraVelocity *= 0.75f;
		}

		if (lengthSqr(cameraVelocity) < 0.5f * currentScene->cameraDistance) {
			cameraVelocity = {};
		} else {
			currentScene->targetCameraPosition += cameraVelocity;
			currentScene->matrixSceneToNDCDirty = true;
		}
		v2f prevCameraPosition = currentScene->cameraPosition;
		currentScene->cameraPosition = lerp(currentScene->cameraPosition, currentScene->targetCameraPosition, cameraMoveSpeed);
		if (distanceSqr(currentScene->cameraPosition, currentScene->targetCameraPosition) > currentScene->cameraDistance) {
			currentScene->matrixSceneToNDCDirty = true;
		}
		
		mouseScenePos = clientToScenePos(mousePosBL);
		
		smoothMousePosT = moveTowards(
			smoothMousePosT, 
			(keyHeld(Key_shift) && mouseButtonHeld(0) && !manipulatingEntity() && smoothable(currentScene->tool)) ? 0.1f : 1.0f, 
			0.1f
		);
		oldSmoothMousePos = smoothMousePos;
		smoothMousePos = lerp(smoothMousePos, (v2f)mousePosBL, smoothMousePosT);
		smoothMousePosChanged |= (mouseButtonHeld(0) || mouseHovering) && distanceSqr(smoothMousePos, oldSmoothMousePos) > 0.01f;
		v2f smoothMouseScenePos = clientToScenePos(smoothMousePos);
		
		if (cameraDistanceChanged) {
			currentScene->matrixSceneToNDCDirty = true;
		}
		if (thicknessChanged) {
			currentScene->constantBufferDirty = true;
		}

		if (hoveringColorMenu) {
			cursorVisible = true;
			if (mouseButtonDown(0)) {
				if (containsTriangle(cursorRelativeColorMenu, SV_TRIANGLE_SIZE)) {
					colorMenuSelectingSV = true;
				} else {
					colorMenuSelectingH = true;
				}
			}
			//if (mouseWheel) {
			//	v3f rgb;
			//	switch (colorMenuTarget) {
			//		case ColorMenuTarget_draw: rgb = currentScene->drawColor; currentScene->drawColorDirty = true; break;
			//		case ColorMenuTarget_canvas: rgb = currentScene->canvasColor; currentScene->needRepaint = true; break;
			//	}

			//	colorMenuHue = frac(colorMenuHue + mouseWheel * 0.01f);
			//	v3f hsv = rgbToHsv(rgb);
			//	hsv.x = colorMenuHue;
			//	rgb = hsvToRgb(hsv);

			//	currentScene->constantBufferData.colorMenuColor = rgb;
			//	switch (colorMenuTarget) {
			//		case ColorMenuTarget_draw: currentScene->drawColor = rgb; break;
			//		case ColorMenuTarget_canvas: currentScene->canvasColor = rgb; break;
			//	}
			//}
		}
		if (draggingEntity) {
			draggingEntity->position = mouseScenePos - draggingEntityOffset;
			draggingEntity->hovered = true;
			currentScene->needRepaint = true;
			if (drawBounds) {
				calculateBounds(*draggingEntity);
			}
		}
		if (rotatingEntity) {
			targetImageRotation = rotatingEntityInitialAngle - atan2(mouseScenePos - rotatingEntity->position);

			if (keyHeld(Key_control)) {
				f32 const roundFactor = pi * 0.25f;
				targetImageRotation = TL::round(targetImageRotation / roundFactor) * roundFactor;
			}

			rotatingEntity->rotation = lerpWrap(rotatingEntity->rotation, targetImageRotation, 0.5f, pi * 2);

			rotatingEntity->hovered = true;
			currentScene->needRepaint = true;
			if (drawBounds) {
				calculateBounds(*rotatingEntity);
			}
		}
		if (scalingImage) {
			m2 toGlobal = m2::rotation(-scalingImage->rotation);
			m2 toLocal = m2::rotation(scalingImage->rotation);

			struct Point {
				v2f local;
				v2f global;
			};

			Point topRight, topLeft, bottomRight, bottomLeft;
			topRight   .local = scalingImage->size * 0.5f;
			topLeft    .local = (scalingImage->size * v2f{-1, 1}) * 0.5f;
			bottomLeft .local = -topRight.local;
			bottomRight.local = -topLeft.local;
			topRight   .global = (toGlobal * scalingImage->size) * 0.5f;
			topLeft    .global = (toGlobal * (scalingImage->size * v2f{-1, 1})) * 0.5f;
			bottomLeft .global = -topRight.global;
			bottomRight.global = -topLeft.global;
			topRight   .global += scalingImage->position;
			topLeft    .global += scalingImage->position;
			bottomLeft .global += scalingImage->position;
			bottomRight.global += scalingImage->position;

			v2f localMousePos = toLocal * (mouseScenePos - scalingImage->position);

			Point anchor{}, opposite{};
			v2f target{}, scaleMult{}, dir{};
			f32 sizeScale{};
			u32 sizeIndex{};
			switch (scalingImageAnchor) {
				case ImageScaling_left: {
					anchor.global = (bottomRight.global + topRight.global) * 0.5f;
					opposite.global = (bottomLeft.global + topLeft.global) * 0.5f;
					dir = normalize(bottomLeft.global - bottomRight.global);
					sizeScale = 1;
					sizeIndex = 0;
					goto edge;
				} break;
				case ImageScaling_right: {
					anchor.global = (bottomLeft.global + topLeft.global) * 0.5f;
					opposite.global = (bottomRight.global + topRight.global) * 0.5f;
					dir = normalize(bottomRight.global - bottomLeft.global);
					sizeScale = -1;
					sizeIndex = 0;
					goto edge;
				} break;
				case ImageScaling_top: {
					anchor.global = (bottomLeft.global + bottomRight.global) * 0.5f;
					opposite.global = (topLeft.global + topRight.global) * 0.5f;
					dir = normalize(topLeft.global - bottomLeft.global);
					sizeScale = -1;
					sizeIndex = 1;
					goto edge;
				} break;
				case ImageScaling_bottom: {
					anchor.global = (topLeft.global + topRight.global) * 0.5f;
					opposite.global = (bottomLeft.global + bottomRight.global) * 0.5f;
					dir = normalize(bottomLeft.global - topLeft.global);
					sizeScale = 1;
					sizeIndex = 1;
					goto edge;
				} break;
				case ImageScaling_topLeft:     anchor = bottomRight; opposite = topLeft;     scaleMult = v2f{ 1,-1}; break;
				case ImageScaling_topRight:    anchor = bottomLeft;  opposite = topRight;    scaleMult = v2f{-1,-1}; break;
				case ImageScaling_bottomLeft:  anchor = topRight;    opposite = bottomLeft;  scaleMult = v2f{ 1, 1}; break;
				case ImageScaling_bottomRight: anchor = topLeft;     opposite = bottomRight; scaleMult = v2f{-1, 1}; break;
				default:
					INVALID_CODE_PATH("bad scalingImageAnchor");
					break;
			}
			target = lerp(anchor.local, opposite.local, draggingPointUv);
			target = (toGlobal * map(localMousePos, anchor.local, target, anchor.local, opposite.local)) + scalingImage->position;
			// debugPoints.push_back({anchor.global, {0,1,0}});
			// debugPoints.push_back({opposite.global, {0,0,1}});
			// debugPoints.push_back({target, {1,0,0}});
			scalingImage->size = toLocal * (anchor.global - target) * scaleMult;
			goto end;

		edge:
			target = lerp(anchor.global, opposite.global, draggingPointUv.y);
			if (anchor.global.y != target.y && anchor.global.y != opposite.global.y) {
				target.y = map(mouseScenePos.y, anchor.global.y, target.y, anchor.global.y, opposite.global.y);
			}
			if (anchor.global.x != target.x && anchor.global.x != opposite.global.x) {
				target.x = map(mouseScenePos.x, anchor.global.x, target.x, anchor.global.x, opposite.global.x);
			}
			target = anchor.global + dir * dot(dir, target - anchor.global);
			scalingImage->size.s[sizeIndex] = sizeScale * (toLocal * (anchor.global - target)).s[sizeIndex];

		end:
			scalingImage->position = (anchor.global + target) * 0.5f;
			scalingImage->hovered = true;
			currentScene->needRepaint = true;
			if (drawBounds) {
				calculateBounds(*scalingImage);
			}
		}
		if (colorMenuSelectingH) {
			if (mouseButtonUp(0)) {
				colorMenuSelectingH = false;
			}
			if (mouseButtonHeld(0)) {
				v3f rgb;
				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: rgb = currentScene->drawColor; currentScene->drawColorDirty = true; break;
					case ColorMenuTarget_canvas: rgb = currentScene->canvasColor; currentScene->needRepaint = true; break;
				}

				v3f hsv = rgbToHsv(rgb);
				colorMenuHue = hsv.x = frac(map(atan2((v2f)mousePosTL - colorMenuPositionTL), -pi, pi, 0, 1) - 0.25f);
				rgb = hsvToRgb(hsv);
				
				renderer->setColorMenuColor(currentScene, rgb);

				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: currentScene->drawColor = rgb; break;
					case ColorMenuTarget_canvas: currentScene->canvasColor = rgb; break;
				}
			}
		} else if (colorMenuSelectingSV) {
			if (mouseButtonUp(0)) {
				colorMenuSelectingSV = false;
			}
			if (mouseButtonHeld(0)) {
				v2f topCorner   = v2f{ 0,                   -1   };
				v2f rightCorner = v2f{ cosf(radians(30.0f)), 0.5f};
				v2f leftCorner  = v2f{-cosf(radians(30.0f)), 0.5f};
				v2f topRight = (topCorner + rightCorner) * 0.5f;
				f32 saturation = clamp(dot(cursorRelativeColorMenu / SV_TRIANGLE_SIZE - rightCorner, topCorner - rightCorner) / pow2(sqrt3), 0, 1);
				f32 value      = clamp(dot(cursorRelativeColorMenu / SV_TRIANGLE_SIZE - leftCorner, topRight - leftCorner)    / pow2( 1.5f), 0, 1);
				
				v3f rgb = hsvToRgb({colorMenuHue, saturation, value});
				
				renderer->setColorMenuColor(currentScene, rgb);

				switch (colorMenuTarget) {
					case ColorMenuTarget_draw: currentScene->drawColor = rgb; currentScene->drawColorDirty = true; break;
					case ColorMenuTarget_canvas: currentScene->canvasColor = rgb; currentScene->needRepaint = true; break;
				}
			}
		} else {
			if (mouseButtonDown(0)) {
				colorMenuOpened = false;
			}
			if (!draggingEntity && !rotatingEntity && !scalingImage && !currentEntity && !hoveringColorMenu) {
				if (keyHeld(Key_shift)) {
				} else {
				}
			}
			if (mouseButtonUp(0)) {
				if (draggingEntity) {
					currentAction->translate.endPosition = draggingEntity->position;
					calculateBounds(*draggingEntity);
					draggingEntity->hovered = false;
					draggingEntity = 0;
					currentAction = 0;
				}
				if (scalingImage) {
					currentAction->scale.endPosition = scalingImage->position;
					currentAction->scale.endSize = scalingImage->size;
					calculateBounds(*scalingImage);
					scalingImage->hovered = false;
					scalingImage = 0;
					currentAction = 0;
				}
				if (rotatingEntity) {
					rotatingEntity->rotation = positiveModulo(rotatingEntity->rotation, pi * 2);
					currentAction->rotate.endAngle = rotatingEntity->rotation;
					calculateBounds(*rotatingEntity);
					rotatingEntity->hovered = false;
					rotatingEntity = 0;
					currentAction = 0;
				}
			}
			if (!mainPieMenu.opened) {
				if (!draggingEntity && !rotatingEntity && !scalingImage) {
					if (currentScene->tool == Tool_hand) {
						v2f uv;
						findHoveredEntity();
						if (hoveredEntity) {
							hoveredEntity->hovered = true;
							switch (hoveredEntity->type) {
								case Entity_image: {
									auto hoveredImage = &hoveredEntity->image;
									hoveredImage->hovered = true;
									bool inCenter = false;

									v2f uv = hoveredImageUv;

									//
									// draggingPointUv ranges from 0 to 1
									//
									// if dragging the edge, then:
									//
									// draggingPointUv.x is distance from right edge to dragging point 
									// looking from center (if it increases, dragging point moves anti-clockwise)
									//
									// draggingPointUv.y is distance from the opposite side to dragging point
									// Example: dragging top edge
									//        |<---X
									//  ___________ 
									//  |_|___o_|_| -
									//  | |     | | ^
									//  | |     | | |
									//  |_|_____|_| |
									//  |_|_____|_| |
									//              Y
									//

									if (uv.x < 0.1f) {
										if (uv.y < 0.1f) {
											scalingImageAnchor = ImageScaling_bottomLeft;
											draggingPointUv = 1 - uv;
								
											v2f topRight    = hoveredImage->size * 0.5f;
											v2f topLeft     = (hoveredImage->size * v2f{-1, 1}) * 0.5f;
											v2f bottomLeft  = -topRight;
											v2f bottomRight = -topLeft;
											m2 rotation = m2::rotation(-hoveredImage->rotation);
											debugPoints.push_back({(rotation * lerp(topRight, bottomLeft, draggingPointUv)) + hoveredImage->position, {0,1,0}});
										} else if (uv.y > 0.9f) {
											scalingImageAnchor = ImageScaling_topLeft;
											draggingPointUv = {1 - uv.x, uv.y};
										} else {
											scalingImageAnchor = ImageScaling_left;
											draggingPointUv = {1 - uv.y, 1 - uv.x};
										}
									} else if (uv.x > 0.9f) {
										if (uv.y < 0.1f) {
											scalingImageAnchor = ImageScaling_bottomRight;
											draggingPointUv = {uv.x, 1 - uv.y};
										} else if (uv.y > 0.9f) {
											scalingImageAnchor = ImageScaling_topRight;
											draggingPointUv = uv;
										} else {
											scalingImageAnchor = ImageScaling_right;
											draggingPointUv = {uv.y, uv.x};
										}
									} else {
										if (uv.y < 0.1f) {
											scalingImageAnchor = ImageScaling_bottom;
											draggingPointUv = {uv.x, 1 - uv.y};
										} else if (uv.y > 0.9f) {
											scalingImageAnchor = ImageScaling_top;
											draggingPointUv = {1 - uv.x, uv.y};
										} else {
											inCenter = true;
										}
									}
									if (mouseButtonDown(0)) {
										if (keyHeld(Key_shift)) {
											rotatingEntity = hoveredEntity;
											rotatingEntityInitialAngle = rotatingEntity->rotation + atan2(mouseScenePos - rotatingEntity->position);

											RotateAction rotateAction;
											rotateAction.targetId = hoveredImage->id;
											rotateAction.startAngle = hoveredImage->rotation;

											currentAction = pushAction(currentScene, std::move(rotateAction));
										} else {
											if (inCenter) {
												draggingEntity = hoveredEntity;
												draggingEntityOffset = mouseScenePos - hoveredImage->position;

												TranslateAction translateAction;
												translateAction.targetId = hoveredImage->id;
												translateAction.startPosition = hoveredImage->position;

												currentAction = pushAction(currentScene, std::move(translateAction));
											} else {
												scalingImage = hoveredImage;
								
												ScaleAction scaleAction;
												scaleAction.targetId = hoveredImage->id;
												scaleAction.startPosition = hoveredImage->position;
												scaleAction.startSize = hoveredImage->size;

												currentAction = pushAction(currentScene, std::move(scaleAction));
											}
										}
									}
								} break;
								case Entity_pencil:
								case Entity_line: 
								case Entity_grid:
								case Entity_circle: {
									if (mouseButtonDown(0)) {
										if (keyHeld(Key_shift)) {
											rotatingEntity = hoveredEntity;
											rotatingEntityInitialAngle = rotatingEntity->rotation + atan2(mouseScenePos - rotatingEntity->position);

											RotateAction rotateAction;
											rotateAction.targetId = hoveredEntity->id;
											rotateAction.startAngle = hoveredEntity->rotation;

											currentAction = pushAction(currentScene, std::move(rotateAction));
										} else {
											draggingEntity = hoveredEntity;
											draggingEntityOffset = mouseScenePos - hoveredEntity->position;

											TranslateAction translateAction;
											translateAction.targetId = hoveredEntity->id;
											translateAction.startPosition = hoveredEntity->position;

											currentAction = pushAction(currentScene, std::move(translateAction));
										}
									}
								} break;
							}
						}
						if (previousHoveredEntity != hoveredEntity) {
							if (previousHoveredEntity) {
								previousHoveredEntity->hovered = false;
							}
							currentScene->needRepaint = true;
						}
						previousHoveredEntity = hoveredEntity;
					} else {
						if (previousHoveredEntity) {
							previousHoveredEntity->hovered = false;
							currentScene->needRepaint = true;
							previousHoveredEntity = 0;
						}
					}
					if (mouseButtonDown(0)) {
						smoothMousePos = (v2f)mousePosBL;
						smoothMouseScenePos = clientToScenePos(smoothMousePos);
						smoothMousePosChanged = true;
						switch (currentScene->tool) {
							case Tool_pencil: {
								PencilEntity pencil = {};
								renderer->initPencilEntity(pencil);

								Optional<Point> endpoint;
								if (keyHeld(Key_control)) {
									endpoint = getEndpoint(currentScene, pencil.id);
								}
								if (endpoint) {
									pencil.position = endpoint->position;
									pencil.previousEndThickness = pencil.newLineStartPoint.thickness = endpoint->thickness;
								} else {
									pencil.position = smoothMouseScenePos;
									pencil.previousEndThickness = pencil.newLineStartPoint.thickness = getDrawThickness(currentScene);
								}
								pencil.newLineStartPoint.position = {};
								pencil.color = currentScene->drawColor;

								renderer->initDynamicLineArray(pencil.renderData, 0, pencilLineBufferDefElemCount);
								currentEntity = pushEntity(currentScene, std::move(pencil));

								//if (connection) {
								//	StringBuilder<> builder;
								//	builder.appendBytes(Net_startEntity);
								//	builder.appendBytes(pencil.color);
								//	net::send(connection, (StringView)builder.get());
								//}
							} break;
							case Tool_line: {
								LineEntity line = {};
								renderer->initLineEntity(line);
								line.color = currentScene->drawColor;
								line.initialThickness = line.line.b.thickness = getDrawThickness(currentScene);
								Optional<Point> endpoint;
								if (keyHeld(Key_control)) {
									endpoint = getEndpoint(currentScene, line.id);
								}
								if (endpoint) {
									line.position = endpoint->position;
									line.line.a.thickness = endpoint->thickness;
								} else {
									line.position = smoothMouseScenePos;
									line.line.a.thickness = line.initialThickness;
								}
								line.initialPosition = line.position;
								line.line.a.position = {};
								renderer->initDynamicLineArray(line.renderData, 0, 1);
								currentEntity = pushEntity(currentScene, std::move(line));
							} break;
							case Tool_grid: {
								GridEntity grid = {};
								renderer->initGridEntity(grid);
								grid.color = currentScene->drawColor;
								grid.position = smoothMouseScenePos;
								grid.thickness = getDrawThickness(currentScene);
								grid.cellCount = displayGridSize = {4, 4};
								updateWindowText = true;
								renderer->initDynamicLineArray(grid.renderData, 0, grid.cellCount.x + grid.cellCount.y + 2);
								currentEntity = pushEntity(currentScene, std::move(grid));
							} break;
							case Tool_circle: {
								CircleEntity circle = {};
								renderer->initCircleEntity(circle);
								circle.color = currentScene->drawColor;
								circle.position = circle.startPosition = smoothMouseScenePos;
								circle.thickness = getDrawThickness(currentScene);
								circle.radius = {};
								renderer->initDynamicLineArray(circle.renderData, 0, CircleEntity::LINE_COUNT);
								currentEntity = pushEntity(currentScene, std::move(circle));
							} break;
							case Tool_dropper: {
							} break;
							case Tool_hand: {
							} break;
							default: INVALID_CODE_PATH();
						}
					} 
					if (mouseButtonHeld(0)) {
						switch (currentScene->tool) {
							case Tool_dropper: {
								if (mousePosChanged || mouseButtonDown(0)) {
									renderer->pickColor();
								}
							} break;
							case Tool_hand: {
							} break;
							case Tool_pencil:
							case Tool_line:
							case Tool_grid:
							case Tool_circle: {
								if (currentEntity) {
									switch (currentEntity->type) {
										case Entity_pencil: {
											auto &pencil = currentEntity->pencil;
											if (smoothMousePosChanged || thicknessChanged || currentScene->drawColorDirty) {
												if (pencil.popNextTime) {
													pencil.lines.pop_back();
													renderer->onLinePopped(pencil);
												}
												Line newLine = {};
						
												Optional<Point> endpoint;
												if (pencil.lines.size()) {
													Line lastLine = pencil.lines.back();
													newLine.a.position = lastLine.b.position;
													newLine.a.thickness = lastLine.b.thickness;
													// newLine.a.color = lastLine.b.color;
												} else {
													if (keyHeld(Key_control)) {
														endpoint = getEndpoint(currentScene, pencil.id);
													}
													if (endpoint) {
														newLine.a = *endpoint;
														newLine.a.position -= pencil.position;
													} else {
														newLine.a = pencil.newLineStartPoint;
													}
												}
						
												newLine.b.position = smoothMouseScenePos - pencil.position;

												f32 endThickness = getDrawThickness(currentScene);
												f32 sceneDist = distance(newLine.a.position, newLine.b.position) / endThickness;
												f32 clientDist = distance(sceneToClientPos(newLine.a.position), sceneToClientPos(newLine.b.position));
												if (!endpoint) {
													// Pressure simulation on mouse
													endThickness *= (1.0f / (max(0, sceneDist) * 0.5f + 1.0f));
													if (!pencil.popNextTime) {
														endThickness = lerp(endThickness, pencil.previousEndThickness, 0.666f);
													}
												}
												pencil.previousEndThickness = endThickness;
												pencil.popNextTime = clientDist < minPencilLineLength;

												// Corner smoothing, looks bad
		#if 0
												u32 numSteps = 0;
												if (pencil.lines.size()) {
													numSteps = floorToInt(distance(smoothMouseScenePos, newLine.a.position) / currentScene->cameraDistance / currentScene->drawThickness);
												}
												if (numSteps > 1) {
													Line lastLine = pencil.lines.back();
													v2f lastDir = normalize(lastLine.b.position - lastLine.a.position);
													v2f corner = lastLine.b.position + lastDir * dot(lastDir, newLine.b.position - newLine.a.position);
													v2f toCorner = corner - lastLine.b.position;
													v2f toEnd = smoothMouseScenePos - corner;

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
							
													//renderer->release(pencil.lineBuffer);
													//pencil.lineBuffer = d3d11State.createStructuredBuffer(pencil.transformedLines.size(), sizeof(TransformedLine), pencil.transformedLines.data(), D3D11_USAGE_IMMUTABLE);

													u32 bufferElemCount = pencil.lineBuffer.size / sizeof(TransformedLine);
													if (pencil.transformedLines.size() > bufferElemCount) {
														renderer->release(pencil.lineBuffer);
														pencil.lineBuffer = d3d11State.createStructuredBuffer(bufferElemCount + pencilLineBufferDefElemCount, sizeof(TransformedLine), pencil.transformedLines.data(), D3D11_USAGE_DEFAULT);
													}
													d3d11State.updateStructuredBuffer(pencil.lineBuffer, numSteps, sizeof(TransformedLine), pencil.transformedLines.end() - numSteps, pencil.transformedLines.size() - numSteps);

													currentScene->needRepaint = true;
												} else 
		#endif
												{
													// newLine.b.color = currentScene->drawColor;
													newLine.b.thickness = endThickness;

													pushLine(pencil, newLine);

													renderer->resizePencilLineArray(pencil);
													renderer->updateLastElement(pencil);

													currentScene->needRepaint = true;
												}
												//if (!pencil.popNextTime) {
												//	if (connection) {
												//		StringBuilder<> builder;
												//		builder.appendBytes(Net_updateEntity);
												//		builder.appendBytes(newLine);
												//		net::send(connection, (StringView)builder.get());
												//	}
												//}
											}
										} break;
										case Entity_line: {
											auto &line = currentEntity->line;
											bool updateLine = thicknessChanged || currentScene->drawColorDirty || smoothMousePosChanged;
											Optional<Point> endpoint;
											if (keyDown(Key_control)) {
												endpoint = getEndpoint(currentScene, line.id);
											}
											if (endpoint) {
												line.line.a = *endpoint;
												line.line.a.position -= line.position;
												currentScene->needRepaint = true;
												updateLine = true;
											} 
											if (keyUp(Key_control)) {
												line.line.a.position = line.initialPosition;
												line.line.a.thickness = line.initialThickness;
												currentScene->needRepaint = true;
												updateLine = true;
											}
											if (updateLine) {
												line.line.b.position = smoothMouseScenePos - line.position;
												line.line.b.thickness = getDrawThickness(currentScene);
												// act.line.b.color = currentScene->drawColor;

												renderer->updateLines(line.renderData, &line.line, 1, 0);
												currentScene->needRepaint = true;
											}
										} break;
										case Entity_grid: {
											auto &grid = currentEntity->grid;
											auto const squareKey = Key_shift;
											if (thicknessChanged || gridCellCountChanged || keyChanged(squareKey) || mousePosChanged) {
												v2f size = mouseScenePos - grid.position;
												if (keyHeld(squareKey) && !keyHeld(Key_control)) {
													grid.size = sign(size) * max(absolute(size.x), absolute(size.y));
												} else {
													grid.size = size;
												}
												grid.thickness = getDrawThickness(currentScene);

												renderer->updateGridLines(grid);

												currentScene->needRepaint = true;
											}
										} break;
										case Entity_circle: {
											auto &circle = currentEntity->circle;
											auto const ellipseKey = Key_shift;
											if (thicknessChanged || mousePosChanged || keyChanged(ellipseKey)) {
												if (keyHeld(ellipseKey)) {
													circle.position = (circle.startPosition + mouseScenePos) * 0.5f;
													circle.radius = absolute(circle.startPosition - mouseScenePos) * 0.5f;
												} else {
													circle.position = circle.startPosition;
													circle.radius = V2f(length(mouseScenePos - circle.position));
												}
												circle.thickness = getDrawThickness(currentScene);

												renderer->updateCircleLines(circle);
												currentScene->needRepaint = true;
											}
										} break;
										default: INVALID_CODE_PATH();
									}
									if (drawBounds) {
										calculateBounds(*currentEntity);
										currentScene->needRepaint = true;
									}
								}
							} break;
							default: INVALID_CODE_PATH();
						}
					} 
					if (mouseButtonUp(0)) {
						if (currentEntity) {
							calculateBounds(*currentEntity);
							switch (currentEntity->type) {
								case Entity_pencil: {
									auto &pencil = currentEntity->pencil;
									v2f offset = pencil.bounds.middle() - pencil.position;
									pencil.position += offset;
									for (auto &line : pencil.lines) {
										line.a.position -= offset;
										line.b.position -= offset;
									}
									renderer->updateLines(pencil.renderData, pencil.lines.data(), pencil.lines.size(), 0);
									renderer->freeze(pencil);
									//if (connection) {
									//	StringBuilder<> builder;
									//	builder.appendBytes(pencil.type);
									//	builder.appendBytes(pencil.id);
									//	builder.appendBytes(pencil.color);
									//	builder.appendBytes((u32)pencil.lines.size());
									//	for (auto l : pencil.lines) {
									//		builder.appendBytes(l.a);
									//		builder.appendBytes(l.b);
									//	}
									//	net::send(connection, (StringView)builder.get());
									//}
								} break;
								case Entity_line: {
									auto &line = currentEntity->line;
									v2f offset = line.bounds.middle() - line.position;
									line.position += offset;
									line.line.a.position -= offset;
									line.line.b.position -= offset;
									renderer->updateLines(line.renderData, &line.line, 1, 0);
								} break;
								case Entity_grid: {
									displayGridSize = {};
									updateWindowText = true;
								} break;
								case Entity_circle: break;
								default: INVALID_CODE_PATH();
							}
							//if (connection) {
							//	net::sendBytes(connection, Net_stopAction);
							//}
							currentEntity = 0;
							if (drawBounds) {
								currentScene->needRepaint = true;
							}
						}
					}
				}
			}
		}
		//if (currentAction.type != Action_none) {
		//	currentScene->extraActionsToDraw.push_back(&currentAction);
		//}

		//while (1) {
		//	if (auto opt = receivedActions.try_pop()) {
		//		auto action = *std::move(opt);
		//		pushAction(currentScene, std::move(action));
		//		currentScene->needRepaint = true;
		//	} else {
		//		break;
		//	}
		//}

		auto updatePieMenu = [&](PieMenu &menu) {
			menu.animTime = moveTowards(menu.animTime, (f32)menu.opened, 0.1f);
			menu.animValue = easeInOut2(menu.animTime);
			menu.TChanged = menu.animTime != menu.prevT;
			menu.prevT = menu.animTime;
			if (menu.TChanged) {
				needRepaint = true;
			}
			if (menu.opened) {
				cursorVisible = true;
				f32 oldAlpha = menu.handAlphaTime;
				menu.handAlphaTime = moveTowards(menu.handAlphaTime, (f32)(distance((v2f)mousePosBL, menu.position) > pieMenuSize * 0.5f), 0.1f);
				menu.handAlphaValue = easeInOut2(menu.handAlphaTime);
				if (menu.handAlphaTime != oldAlpha) {
					needRepaint = true;
				}
				menu.angle = atan2((v2f)mousePosBL - menu.position);
				if (keyDown(Key_escape)) {
					closePieMenu(menu);
				}
				if (mouseButtonDown(mouseButton_left) || (keyUp(Key_tab))) {
					if (mouseButtonDown(mouseButton_left)) {
						closePieMenu(menu);
					}
					if (distance((v2f)mousePosBL, menu.position) > pieMenuSize * 0.5f) {
						closePieMenu(menu);

						f32 a = frac(map(atan2((v2f)mousePosBL - menu.position), -pi, pi, 0, 1) + 0.5f / menu.items.size()) * menu.items.size();
						
						mousePosTL = (v2s)menu.position;
						mousePosTL.y = clientSize.y - mousePosTL.y;

						setCursorPos(mousePosTL + windowPos);

						mousePosBL = mousePosTL;
						mousePosBL.y = clientSize.y - mousePosTL.y;

						smoothMousePosChanged = true;
						smoothMousePos = (v2f)mousePosBL;

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

		prevColorMenuAnimTime = colorMenuAnimTime;
		colorMenuAnimTime = moveTowards(colorMenuAnimTime, (f32)colorMenuOpened, 0.1f);

		if (colorMenuAnimTime != prevColorMenuAnimTime) {
			colorMenuAnimValue = easeInOut2(colorMenuAnimTime);
			needRepaint = true;
		}

		updatePieMenu(mainPieMenu);
		
		if (smoothMousePosChanged || currentScene->drawColorDirty || thicknessChanged) {
			needRepaint = true;
			renderer->updatePaintCursor(currentScene, smoothMousePos, currentScene->drawColor, currentScene->windowDrawThickness);
		}

		if (windowResized) {
			windowResized = false;
			needRepaint = true;
			windowSizeDirty = true;
			resizeRenderTargets();
			currentScene->matrixSceneToNDCDirty = true;
		}

		if ((mousePosChanged && mouseHovering) || (!mouseHovering && previousMouseHovering)) {
			needRepaint = true;
		}

		if (cursorVisible) {
			showCursor();
		} else {
			hideCursor();
		}

		bool drawCursor = mouseHovering && !hoveringColorMenu && !cursorVisible;
		renderer->repaint(drawCursor, drawCursor && currentScene->tool != Tool_hand);
		for (auto &sc : scenes) {
			if (sc.initialized) {
				sc.extraActionsToDraw.clear();
			}
		}
		
		if (updateWindowText) {
			updateWindowText = false;
			u32 sceneIndex = (u32)(currentScene - scenes);
			auto loc = localizations + language;
			StringBuilder<wchar> builder;
			builder.appendFormat(loc->windowTitlePrefix, sceneIndex);
			if (currentScene->path.size()) {
				if (displayGridSize.x) {
					builder.appendFormat(localizations[language].windowTitle_path_gridSize, currentScene->filename, currentScene->showAsterisk ? '*' : '\n', displayGridSize.x, displayGridSize.y);
				} else {
					builder.appendFormat(localizations[language].windowTitle_path, currentScene->filename, currentScene->showAsterisk ? '*' : '\n');
				}
			} else {
				if (displayGridSize.x) {
					builder.appendFormat(localizations[language].windowTitle_untitled_gridSize, displayGridSize.x, displayGridSize.y);
				} else {
					builder.appendFormat(localizations[language].windowTitle_untitled);
				}
			}
			setWindowTitle(builder.getNullTerminated().data());
		}
		previousMouseHovering = mouseHovering;
	}

	for (auto &scene : scenes) {
		if (!scene.initialized)
			continue;
		for (auto &[id, e] : scene.entities) {
			cleanup(e);
		}
		renderer->releaseScene(&scene);
	}

	deinitThreadPool(&threadPool);
	imageLoaderThread.join();

	renderer->shutdown();

#if TRACK_ALLOCATIONS
	logUnfreedMemory();
#endif
}
