#pragma once
#include "base.h"

#define R_initScene					R_DECORATE(void, initScene, (Scene *scene), (scene))
#define R_resize					R_DECORATE(void, resize, (), ())
#define R_repaint					R_DECORATE(void, repaint, (bool drawCursor, bool drawCursorCircle), (drawCursor, drawCursorCircle))
#define R_initConstantLineArray		R_DECORATE(void, initConstantLineArray, (void *renderData, TransformedLine const *data, umm count), (renderData, data, count))
#define R_initDynamicLineArray		R_DECORATE(void, initDynamicLineArray, (void *renderData, TransformedLine const *data, umm count), (renderData, data, count))
#define R_reinitDynamicLineArray	R_DECORATE(void, reinitDynamicLineArray, (void* renderData, TransformedLine const* data, umm count), (renderData, data, count))
#define R_releasePencil				R_DECORATE(void, releasePencil, (PencilEntity& action), (action))
#define R_releaseLine				R_DECORATE(void, releaseLine, (LineEntity& action), (action))
#define R_releaseGrid				R_DECORATE(void, releaseGrid, (GridEntity& action), (action))
#define R_releaseCircle				R_DECORATE(void, releaseCircle, (CircleEntity& action), (action))
#define R_releaseImageData			R_DECORATE(void, releaseImageData, (void *renderData), (renderData))
#define R_releaseEntity				R_DECORATE(void, releaseEntity, (Entity& action), (action))
#define R_switchRasterizer			R_DECORATE(void, switchRasterizer, (), ())
#define R_pickColor					R_DECORATE(void, pickColor, (), ())
#define R_resizeLineArray			R_DECORATE(void, resizeLineArray, (void *renderData, TransformedLine const *data, umm count), (renderData, data, count))
#define R_updateLineArray			R_DECORATE(void, updateLineArray, (void *renderData, TransformedLine const *data, umm count, umm firstElem), (renderData, data, count, firstElem))
#define R_updateLines				R_DECORATE(void, updateLines, (void* renderData, Line const* data, umm count, umm firstElem), (renderData, data, count, firstElem))
#define R_updateGridLines			R_DECORATE(void, updateGridLines, (GridEntity& grid), (grid))
#define R_updateCircleLines			R_DECORATE(void, updateCircleLines, (CircleEntity& circle), (circle))
#define R_freeze					R_DECORATE(void, freeze, (PencilEntity& pencil), (pencil))
#define R_resizePencilLineArray		R_DECORATE(void, resizePencilLineArray, (PencilEntity& pencil), (pencil))
#define R_updateLastElement			R_DECORATE(void, updateLastElement, (PencilEntity& pencil), (pencil))
#define R_setTexture				R_DECORATE(void, setTexture, (void *image, void const *data, u32 width, u32 height), (image, data, width, height))
#define R_updatePaintCursor			R_DECORATE(void, updatePaintCursor, (Scene* scene, v2f windowMousePos, v3f windowDrawColor, f32 windowDrawThickness), (scene, windowMousePos, windowDrawColor, windowDrawThickness))
#define R_initPencilEntity			R_DECORATE(void, initPencilEntity, (PencilEntity& pencil), (pencil))
#define R_initLineEntity			R_DECORATE(void, initLineEntity, (LineEntity& line), (line))
#define R_initGridEntity			R_DECORATE(void, initGridEntity, (GridEntity& grid), (grid))
#define R_initCircleEntity			R_DECORATE(void, initCircleEntity, (CircleEntity& circle), (circle))
#define R_createImageData			R_DECORATE(void *, createImageData, (), ())
#define R_initPencilEntityData		R_DECORATE(void, initPencilEntityData, (PencilEntity& pencil), (pencil))
#define R_initLineEntityData		R_DECORATE(void, initLineEntityData, (LineEntity& line), (line))
#define R_initGridEntityData		R_DECORATE(void, initGridEntityData, (GridEntity& grid), (grid))
#define R_initCircleEntityData		R_DECORATE(void, initCircleEntityData, (CircleEntity& circle), (circle))
#define R_isLoaded					R_DECORATE(bool, isLoaded, (ImageEntity const& image), (image))
#define R_setUnloadedTexture		R_DECORATE(void, setUnloadedTexture, (void *imageData), (imageData))
#define R_update					R_DECORATE(void, update, (Scene* scene), (scene))
#define R_onLinePushed				R_DECORATE(void, onLinePushed, (PencilEntity& pencil, Line line), (pencil, line))
#define R_onLinePopped				R_DECORATE(void, onLinePopped, (PencilEntity& pencil), (pencil))
#define R_onColorMenuOpen			R_DECORATE(void, onColorMenuOpen, (ColorMenuTarget target), (target))
#define R_setColorMenuColor			R_DECORATE(void, setColorMenuColor, (Scene* scene, v3f rgb), (scene, rgb))
#define R_getMutex					R_DECORATE(RecursiveMutex&, getMutex, (), ())
#define R_setMultisampleEnabled		R_DECORATE(void, setMultisampleEnabled, (bool enable), (enable))
#define R_isMultisampleEnabled		R_DECORATE(bool, isMultisampleEnabled, (), ())
#define R_setVSync					R_DECORATE(void, setVSync, (bool enable), (enable))
#define R_releaseScene				R_DECORATE(void, releaseScene, (Scene *scene), (scene))
#define R_shutdown					R_DECORATE(void, shutdown, (), ())
//#define R_debugSaveRenderTarget		R_DECORATE(void, debugSaveRenderTarget, (), ())

#define R_all			 \
R_initScene				 \
R_resize				 \
R_repaint				 \
R_initConstantLineArray	 \
R_initDynamicLineArray	 \
R_reinitDynamicLineArray \
R_releasePencil			 \
R_releaseLine			 \
R_releaseGrid			 \
R_releaseCircle			 \
R_releaseImageData		 \
R_releaseEntity			 \
R_switchRasterizer		 \
R_pickColor				 \
R_resizeLineArray		 \
R_updateLineArray		 \
R_updateLines			 \
R_updateGridLines		 \
R_updateCircleLines		 \
R_freeze				 \
R_resizePencilLineArray	 \
R_updateLastElement		 \
R_setTexture			 \
R_updatePaintCursor		 \
R_initPencilEntity		 \
R_initLineEntity		 \
R_initGridEntity		 \
R_initCircleEntity		 \
R_createImageData   	 \
R_initPencilEntityData	 \
R_initLineEntityData	 \
R_initGridEntityData	 \
R_initCircleEntityData	 \
R_isLoaded				 \
R_setUnloadedTexture	 \
R_update				 \
R_onLinePushed			 \
R_onLinePopped			 \
R_onColorMenuOpen		 \
R_setColorMenuColor		 \
R_getMutex				 \
R_setMultisampleEnabled  \
R_isMultisampleEnabled	 \
R_setVSync				 \
R_releaseScene			 \
R_shutdown				 \
//R_debugSaveRenderTarget

struct RendererImpl;

struct Renderer {

#define ADD_IMPL(...) (RendererImpl *impl, __VA_ARGS__)
#define R_DECORATE(ret, name, args, params) ret (*CONCAT(_, name)) ADD_IMPL args;
	R_all
#undef R_DECORATE
#undef ADD_IMPL
	
#define ADD_IMPL(...) ((RendererImpl *)this, __VA_ARGS__)
#define R_DECORATE(ret, name, args, params) FORCEINLINE ret name args { return CONCAT(_, name) ADD_IMPL params; }
	R_all
#undef R_DECORATE
#undef ADD_IMPL
	
	inline void initEntity(Entity& action) {
		switch (action.type) {
			case Entity_pencil: initPencilEntity(action.pencil); break;
			case Entity_line:   initLineEntity(action.line);   break;
			case Entity_grid:   initGridEntity(action.grid);   break;
			case Entity_circle: initCircleEntity(action.circle); break;
			case Entity_image: 
				break;
		}
	}
	inline void initEntityData(Scene *scene, Entity& action) {
		switch (action.type) {
			case Entity_pencil: initPencilEntityData(action.pencil); break;
			case Entity_line:   initLineEntityData(action.line);   break;
			case Entity_grid:   initGridEntityData(action.grid);   break;
			case Entity_circle: initCircleEntityData(action.circle); break;
			case Entity_image:
				break;
		}
	}
};

Renderer *createRenderer();
