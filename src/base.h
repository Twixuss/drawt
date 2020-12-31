#pragma once
#include "../dep/tl/include/tl/system.h"
using namespace TL;

#include <string>
#include <unordered_map>

#define TRACK_ALLOCATIONS 1//BUILD_DEBUG

#if TRACK_ALLOCATIONS
extern void allocationTracker(void *data, umm size, char const *file, u32 line, char const *message = "");
template <class Allocator>
void *allocationTracker(umm size, umm align, char const *file, u32 line, char const *message = "") {
	void *data = Allocator::allocate(size, align);
	allocationTracker(data, size, file, line, message);
	return data;
}
extern void deallocationTracker(void *data);
extern void logUnfreedMemory();
#define TL_ALLOCATION_TRACKER allocationTracker
#define TL_DEALLOCATION_TRACKER deallocationTracker
#endif

#include "../dep/tl/include/tl/math.h"
#include "../dep/tl/include/tl/list.h"
#include "../dep/tl/include/tl/thread.h"
#include "../dep/tl/include/tl/console.h"

#define INV_ATLAS_TILE_SIZE 0.25f
#define SV_TRIANGLE_SIZE 0.7f

struct f16 {
	u16 v;
	f16 operator+(f16 b) {
		return b;
	}
	operator f32() const {
		u32 sign     = v & 0x8000;
		u32 mantissa = v & 0x03FF;
		u32 exponent;

		if ((v & 0x7C00) != 0) { // The value is normalized
			exponent = ((v >> 10) & 0x1F);
		} else if (mantissa != 0) { // The value is denormalized
			// Normalize the value in the resulting float
			exponent = 1;
			do {
				--exponent;
				mantissa <<= 1;
			} while ((mantissa & 0x0400) == 0);

			mantissa &= 0x03FF;
		} else { // The value is zero
			exponent = (u32)-112;
		}

		u32 result = 
			(sign << 16) |
			((exponent + 112) << 23) |
			(mantissa << 13);

		return *(f32*)&result;

	}
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

using EntityId = u32;
static constexpr EntityId invalidEntityId = ~0;

enum EntityType : u8 {
	Entity_none      = 255,
	Entity_pencil    = 0,
	Entity_line      = 1,
	Entity_grid      = 2,
	Entity_circle    = 3,
	Entity_image     = 4,
	Entity_count,
};

enum ActionType : u8 {
	Action_none      = 255,
	Action_create    = 0,
	Action_translate = 1,
	Action_rotate    = 2,
	Action_scale     = 3,
	Action_count,
};

#define ENTITY_BASE   \
	EntityType type;  \
	EntityId id;      \
	aabb<v2f> bounds; \
	v2f position;     \
	f32 rotation;     \
	bool visible;     \
	bool hovered

#define ACTION_BASE  \
	ActionType type

struct EntityBase {
	ENTITY_BASE;
	EntityBase(EntityType type = Entity_none) {
		this->type = type;
		id = invalidEntityId;
		bounds = {};
		position = {};
		rotation = {};
		visible = false;
		hovered = false;
	}
};

struct ActionBase {
	ACTION_BASE;
	ActionBase(ActionType type = Action_none) : type(type) {}
};

struct PencilEntity : EntityBase {
	PencilEntity() : EntityBase(Entity_pencil) {}

	v3f color = {};
	List<Line> lines;

	Point newLineStartPoint = {};
	bool popNextTime = false;
	f32 previousEndThickness = {};

	void *renderData = {};
};

struct LineEntity : EntityBase {
	LineEntity() : EntityBase(Entity_line) {}

	v3f color = {};
	Line line = {};

	v2f initialPosition = {};
	f32 initialThickness = {};

	void *renderData = {};
};

struct GridEntity : EntityBase {
	GridEntity() : EntityBase(Entity_grid) {}

	v3f color = {};
	f32 thickness = {};
	v2f size = {};
	v2u cellCount = {};

	void *renderData = {};
};

struct CircleEntity : EntityBase {
	CircleEntity() : EntityBase(Entity_circle) {}

	static constexpr u32 LINE_COUNT = 64;

	v3f color = {};
	f32 thickness = {};
	v2f radius = {};

	v2f startPosition = {};
	void *renderData = {};
};

struct ImageEntity : EntityBase {
	ImageEntity() : EntityBase(Entity_image) {}

	Span<wchar> path;
	v2f size = {};

	f32 initialAspectRatio = 1;

	void *renderData = {};
};

struct CreateAction : ActionBase {
	CreateAction() : ActionBase(Action_create) {}
	EntityId targetId = invalidEntityId;
};

struct TranslateAction : ActionBase {
	TranslateAction() : ActionBase(Action_translate) {}

	EntityId targetId = invalidEntityId;
	v2f startPosition = {};
	v2f endPosition = {};
};

struct RotateAction : ActionBase {
	RotateAction() : ActionBase(Action_rotate) {}

	EntityId targetId = invalidEntityId;
	f32 startAngle = {};
	f32 endAngle = {};
};

struct ScaleAction : ActionBase {
	ScaleAction() : ActionBase(Action_scale) {}

	EntityId targetId = invalidEntityId;
	v2f startPosition = {};
	v2f endPosition = {};
	v2f startSize = {};
	v2f endSize = {};
};

#define DECLARE_MEMBER(name, entityType, var, enum) \
	entityType var;

#define DECLARE_CONSTRUCTOR(name, entityType, var, enum)\
	name(entityType const &that) : var(that) {}\
	name(entityType &&that) : var(std::move(that)) {}\
	name& operator=(entityType const &that) { this->~name(); return *new (this) name(that); }\
	name& operator=(entityType &&that) { this->~name(); return *new (this) name(std::move(that)); }

#define CASE_DESTRUCTOR(name, entityType, var, enum) \
	case enum: var.~entityType(); break;

#define CASE_NEW(name, entityType, var, enum) \
	case enum: new (this) name(that.var); break;

#define CASE_NEW_MOVE(name, entityType, var, enum) \
	case enum: new (this) name(std::move(that.var)); break;

#define ENTITIES(X) \
	X(Entity, PencilEntity, pencil, Entity_pencil) \
	X(Entity, LineEntity, line, Entity_line) \
	X(Entity, GridEntity, grid, Entity_grid) \
	X(Entity, CircleEntity, circle, Entity_circle) \
	X(Entity, ImageEntity, image, Entity_image) \

enum CreateEntityFlag {
	CreateEntity_default    = 0,
	CreateEntity_zeroMemory = 1,
};

struct Entity {
	union {
		struct {
			ENTITY_BASE;
		};
		ENTITIES(DECLARE_MEMBER)
	};

	Entity(CreateEntityFlag flags = CreateEntity_default, EntityId id = invalidEntityId) {
		if (flags & CreateEntity_zeroMemory) {
			memset(this, 0, sizeof(*this));
		}
		new (this) EntityBase();
		this->id = id;
	}
	ENTITIES(DECLARE_CONSTRUCTOR)
	Entity(Entity const &that) = delete;
	Entity(Entity &&that) {
		switch (that.type) {
			ENTITIES(CASE_NEW_MOVE)
			case Entity_none: break;
		}
		id = that.id;
		that.reset();
	}
	Entity &operator=(Entity const &that) = delete;
	Entity &operator=(Entity &&that) {
		reset();
		return *new (this) Entity(std::move(that));
	}
	~Entity() {
		switch (type) {
			ENTITIES(CASE_DESTRUCTOR)
		}
		type = Entity_none;
		id = invalidEntityId;
	}
	void reset() { this->~Entity(); }
	operator EntityBase &() { return *(EntityBase *)this; }
};

#define ACTIONS(X) \
	X(Action, CreateAction, create, Action_create) \
	X(Action, TranslateAction, translate, Action_translate) \
	X(Action, RotateAction, rotate, Action_rotate) \
	X(Action, ScaleAction, scale, Action_scale)

struct Action {
	union {
		struct {
			ACTION_BASE;
		};
		ACTIONS(DECLARE_MEMBER)
	};

	Action() {
		new (this) ActionBase();
	}
	ACTIONS(DECLARE_CONSTRUCTOR)
	Action(Action const &that) {
		switch (that.type) {
			ACTIONS(CASE_NEW)
			case Action_none: break;
		}
	}
	Action(Action &&that) {
		switch (that.type) {
			ACTIONS(CASE_NEW_MOVE)
			case Action_none: break;
		}
		that.reset();
	}
	Action &operator=(Action const &that) {
		reset();
		return *new (this) Action(that);
	}
	Action &operator=(Action &&that) {
		reset();
		return *new (this) Action(std::move(that));
	}
	~Action() {
		switch (type) {
			ACTIONS(CASE_DESTRUCTOR)
		}
		type = Action_none;
	}
	void reset() { this->~Action(); }
};

enum Tool {
	Tool_pencil  = 0,
	Tool_line    = 1,
	Tool_grid    = 2,
	Tool_circle  = 3,
	Tool_dropper = 4,
	Tool_hand    = 5,
	Tool_count,
};

struct Scene {
	std::unordered_map<EntityId, Entity> entities;
	List<Action> actions;
	List<Action *> extraActionsToDraw;
	//Mutex actionsMutex;
	u32 postLastVisibleActionIndex = 0;
	Tool tool = Tool_pencil;
	v2f cameraPosition = {};
	f32 cameraDistance = 1.0f;
	v2f targetCameraPosition = {};
	f32 targetCameraDistance = 1.0f;
	f32 windowDrawThickness = 16.0f;
	v3f drawColor = {0.9f,0.8f,0.7f};
	v3f canvasColor = {0.125f, 0.125f, 0.125f};

	std::wstring path;
	wchar *filename = 0;
	u64 savedHash = 0;
	u32 savedPostLastVisibleActionIndex = 0;
	u32 modifiedPostLastVisibleActionIndex = 0;
	bool showAsterisk = false;

	bool needResize = true;
	bool needRepaint = true;
	bool matrixSceneToNDCDirty = true;
	bool drawColorDirty = true;
	bool constantBufferDirty = false;

	bool initialized = false;
	
	u32 entityIdCounter = 0;
	
	void *renderData = {};
};

using PieMenuItemType = u32;
enum : PieMenuItemType {
	PieMenuItem_default,
	PieMenuItem_toolColor,
	PieMenuItem_canvasColor,
};

struct PieMenuItem {
	void (*onSelect)();
	v2u uv;
	PieMenuItemType type;
};
struct PieMenu {
	List<PieMenuItem> items;
	void *renderData = {};
	v2f position = {};
	f32 angle = 0;
	f32 animTime = 0;
	f32 animValue = 0;
	f32 prevT = 0;
	f32 handAlphaTime = 0;
	f32 handAlphaValue = 0;
	bool TChanged = false;
	bool opened = false;
	bool rightMouseReleased = false;
};

enum ColorMenuTarget {
	ColorMenuTarget_draw,
	ColorMenuTarget_canvas,
};

enum Language {
	Language_english,
	Language_russian,
	Language_count
};

struct Localization {
	wchar const *windowTitlePrefix;
	wchar const *windowTitle_path_gridSize;
	wchar const *windowTitle_path;
	wchar const *windowTitle_untitled_gridSize;
	wchar const *windowTitle_untitled;
	wchar const *warning;
	wchar const *fileFilter;
	wchar const *saveFileTitle;
	wchar const *openFileTitle;
	wchar const *unsavedScene;
	wchar const *unsavedScenes;
	wchar const *sceneAlreadyExists;
};

struct DebugPoint {
	v2f position;
	v3f color = v3f{1,1,1};
};

//struct NetAction {
//	Action action;
//	Mutex mutex;
//};

constexpr u32 pencilLineBufferDefElemCount = 1024;

extern Scene scenes[10];
extern Scene *currentScene;
extern Language language;
extern Localization localizations[Language_count];
extern ThreadPool<TL_DEFAULT_ALLOCATOR> threadPool;
extern PieMenu mainPieMenu;
extern List<DebugPoint> debugPoints;
//extern Action currentAction;
//extern NetAction currentNetAction;
extern bool playingSceneShiftAnimation;
extern Scene *previousScene;
extern f32 sceneShiftT;
extern f32 toolImageSize;
extern f32 pieMenuSize;
extern f32 colorMenuAnimValue;
extern v2f colorMenuPositionTL;
extern f32 colorMenuHue;

extern bool drawBounds;

inline u32 indexof(Scene const *scene) {
	return (u32)(scene - scenes);
}

inline f32 getDrawThickness(Scene const *scene) { return scene->windowDrawThickness * scene->cameraDistance; }
inline wchar *getFilename(std::wstring &path) {
	return path.data() + path.rfind(L'\\') + 1;
}
inline bool shouldRepaint(Scene const *scene) {
	return scene->drawColorDirty || scene->constantBufferDirty || scene->matrixSceneToNDCDirty || scene->needRepaint;
}
inline v2u getUv(Tool tool) {
	switch (tool) {
		case Tool_pencil: return {0, 1};
		case Tool_line: return {1, 1};
		case Tool_grid: return {0, 0};
		case Tool_dropper: return {1, 0};
		case Tool_circle: return {0, 2};
		case Tool_hand: return {2, 1};
		default:
			INVALID_CODE_PATH();
			return {};
	}
}
template <class T>
void setUv(T &quad, v2u uv, v2u uvSize) {
	quad.uvMin = (v2f)uv * INV_ATLAS_TILE_SIZE;
	quad.uvMax = quad.uvMin + INV_ATLAS_TILE_SIZE * (v2f)uvSize;
}

inline char const *toString(ActionType t) {
	switch (t)
	{
		case Action_none:      return "none";
		case Action_create:    return "create";
		case Action_translate: return "translate";
		case Action_rotate:    return "rotate";
		case Action_scale:     return "scale";
		default:               return "unknown";
	}
}
inline char const *toString(EntityType t) {
	switch (t)
	{
		case Entity_none:	return "none";
		case Entity_pencil:	return "pencil";
		case Entity_line:	return "line";
		case Entity_grid:	return "grid";
		case Entity_circle:	return "circle";
		case Entity_image:	return "image";
		default:			return "unknown";
	}
}

inline List<Line> getGridLines(GridEntity const &grid) {
	List<Line> lines;
	lines.reserve(grid.cellCount.x + grid.cellCount.y + 2);

	Line line;
	line.a.thickness = line.b.thickness = grid.thickness;

	line.a.position.y = 0;
	line.b.position.y = grid.size.y;
	for (u32 i = 0; i <= grid.cellCount.x; ++i) {
		line.a.position.x = line.b.position.x = (f32)i / grid.cellCount.x * grid.size.x;
		lines.push_back(line);
	}

	line.a.position.x = 0;
	line.b.position.x = grid.size.x;
	for (u32 i = 0; i <= grid.cellCount.y; ++i) {
		line.a.position.y = line.b.position.y = (f32)i / grid.cellCount.y * grid.size.y;
		lines.push_back(line);
	}

	return lines;
}

inline Array<Line, CircleEntity::LINE_COUNT> getCircleLines(CircleEntity const &circle) {
	v2f previousPosition = sincos(0) * circle.radius;
	Array<Line, CircleEntity::LINE_COUNT> result;
	for (u32 i = 0; i < CircleEntity::LINE_COUNT; ++i) {
		Line line;
		line.a.thickness = line.b.thickness = circle.thickness;

		v2f newPosition = sincos((f32)(i+1) / CircleEntity::LINE_COUNT * 2 * pi) * circle.radius;
		line.a.position = previousPosition;
		line.b.position = newPosition;
		result[i] = line;

		previousPosition = newPosition;
	}
	return result;
}
