#include <stdio.h>
#include "os_windows.h"
#include "renderer.h"
#include "../dep/tl/include/tl/d3d11.h"

#define VERTS_PER_QUAD 6
#define VERTS_PER_LINE 96
#define PIE_SEL_VERT_COUNT 42
#define CIRCLE_VERTEX_COUNT (72*6)

#define CIRCLE_WIDTH 2
#define CIRCLE_OUTLINE_WIDTH 1

struct Shader {
	ID3D11VertexShader *vs = 0;
	ID3D11PixelShader *ps = 0;
};

#define DECLARE_CBUFFER(index, name) struct name

#define DECLARE_SCENE_CBUFFER				  \
DECLARE_CBUFFER(0, SceneConstantBufferData) { \
	v2f scenePosition;						  \
	v2f sceneScale;							  \
											  \
	v3f sceneDrawColor;						  \
	f32 sceneDrawThickness;					  \
											  \
	v2f sceneMousePos;						  \
	v2f pad_;								  \
											  \
	v3f colorMenuColor;						  \
}
#define DECLARE_GLOBAL_CBUFFER				   \
DECLARE_CBUFFER(1, GlobalConstantBufferData) { \
	m4 matrixWindowToNDC;					   \
											   \
	v3f windowDrawColor;					   \
	f32 windowAspect;						   \
											   \
	v2f clientSize;							   \
	v2f windowMousePos;						   \
											   \
	f32 windowDrawThickness;				   \
}
#define DECLARE_PIE_CBUFFER				    \
DECLARE_CBUFFER(2, PieConstantBufferData) { \
	v2f piePos;								\
	f32 pieAngle;							\
	f32 pieSize;							\
	f32 pieAlpha;							\
}
#define DECLARE_COLOR_CBUFFER				  \
DECLARE_CBUFFER(3, ColorConstantBufferData) { \
	v2f colorMenuPosition;					  \
	f32 colorMenuSize;						  \
	f32 colorMenuAlpha;						  \
	v3f colorMenuHueColor;					  \
	f32 colorMenuHue;						  \
}
#define DECLARE_ENTITY_CBUFFER				   \
DECLARE_CBUFFER(4, EntityConstantBufferData) { \
	v2f entityPosition;                        \
	v2f imageSize;							   \
											   \
	v3f entityColor;						   \
	f32 thicknessMult;						   \
											   \
	v2f boundsMin;                             \
	v2f boundsMax;                             \
											   \
	m4 entityRotation;						   \
}

DECLARE_SCENE_CBUFFER;
DECLARE_GLOBAL_CBUFFER;
DECLARE_PIE_CBUFFER;
DECLARE_COLOR_CBUFFER;
DECLARE_ENTITY_CBUFFER;

#undef DECLARE_CBUFFER

struct LineData {
	D3D11::StructuredBuffer buffer;
	List<TransformedLine> transformedLines;
};
#define LINE_DATA(x) (*(LineData *)x)

struct ImageData {
	D3D11::Texture texture;
};
#define IMAGE_DATA(x) (*(ImageData *)x)

struct SceneData {
	D3D11::TypedConstantBuffer<SceneConstantBufferData> constantBuffer;
	SceneConstantBufferData constantBufferData;
	D3D11::RenderTexture canvasRT;
	D3D11::RenderTexture canvasRTMS;
};
#define SCENE_DATA(x) (*(SceneData *)x)

struct PieData {
	D3D11::TypedConstantBuffer<PieConstantBufferData> constantBuffer;
};
#define PIE_DATA(x) (*(PieData *)x)

struct RendererImpl : Renderer, D3D11::State {
	Shader lineShader, quadShader, blitShader, blitShaderMS, circleShader, pieSelShader, colorMenuShader, imageShader, imageOutlineShader, boundsShader;
	D3D11::StructuredBuffer uiSBuffer;
	D3D11::Texture toolAtlas, unloadedTexture;
	D3D11::Blend alphaBlend;
	D3D11::TypedConstantBuffer<GlobalConstantBufferData> globalConstantBuffer;
	D3D11::TypedConstantBuffer<ColorConstantBufferData> colorConstantBuffer;
	D3D11::TypedConstantBuffer<EntityConstantBufferData> entityConstantBuffer;
	GlobalConstantBufferData globalConstantBufferData;
	ColorConstantBufferData colorConstantBufferData;
	
	D3D11::Rasterizer defaultRasterizerNoMs;
	D3D11::Rasterizer wireframeRasterizerNoMs;
	D3D11::Rasterizer doubleRasterizerNoMs;
	D3D11::Rasterizer defaultRasterizerMs;
	D3D11::Rasterizer wireframeRasterizerMs;
	D3D11::Rasterizer doubleRasterizerMs;
	D3D11::Rasterizer defaultRasterizer;
	D3D11::Rasterizer wireframeRasterizer;
	D3D11::Rasterizer doubleRasterizer;
	u32 msaaSampleCount = 0;
	bool globalConstantBufferDirty = false;
	bool wireframe = false;
	bool vSync = true;

	RendererImpl();

	void drawEntity(Entity const &action);
	void repaintScene(Scene *scene);
	void updatePieBuffer(PieMenu &menu);
	
	ID3D11VertexShader *createVertexShader(char const *src, umm srcSize, char const *name);
	ID3D11PixelShader *createPixelShader(char const *src, umm srcSize, char const *name);

#define R_DECORATE(ret, name, args, params) ret name args;
	R_all
#undef R_DECORATE
};

ID3D11VertexShader *RendererImpl::createVertexShader(char const *src, umm srcSize, char const *name) {
	ID3DBlob *bc;
	ID3DBlob *errors;
	HRESULT compileResult = D3DCompile(src, srcSize, name, 0, 0, "main", "vs_5_0", 0, 0, &bc, &errors);
	if (errors) {
		LOG("%", (char *)errors->GetBufferPointer());
		errors->Release();
	}
	if (FAILED(compileResult)) {
		LOG("VS compilation failed");
		DHR(compileResult);
	}
	ID3D11VertexShader *result;
	DHR(device->CreateVertexShader(bc->GetBufferPointer(), bc->GetBufferSize(), 0, &result));
	bc->Release();
	return result;
}
ID3D11PixelShader *RendererImpl::createPixelShader(char const *src, umm srcSize, char const *name) {
	ID3DBlob *bc;
	ID3DBlob *errors;
	HRESULT compileResult = D3DCompile(src, srcSize, name, 0, 0, "main", "ps_5_0", 0, 0, &bc, &errors);
	if (errors) {
		LOG("%", (char *)errors->GetBufferPointer());
		errors->Release();
	}
	if (FAILED(compileResult)) {
		LOG("VS compilation failed");
		DHR(compileResult);
	}
	ID3D11PixelShader *result;
	DHR(device->CreatePixelShader(bc->GetBufferPointer(), bc->GetBufferSize(), 0, &result));
	bc->Release();
	return result;
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

List<TransformedLine> getTransformedGridLines(GridEntity const &grid) {
	List<Line> lines = getGridLines(grid);
	List<TransformedLine> result;
	result.reserve(lines.size());
	for (auto l : lines) {
		result.push_back(transform(l));
	}
	return result;
}

Array<TransformedLine, CircleEntity::LINE_COUNT> getTransformedCircleLines(CircleEntity const &circle) {
	auto lines = getCircleLines(circle);
	Array<TransformedLine, CircleEntity::LINE_COUNT> result;
	for (u32 i = 0; i < countof(lines); ++i) {
		result[i] = transform(lines[i]);
	}
	return result;
}

#define SHADER_COMMON_SOURCE R"(
#define DECLARE_CBUFFER(index, name) cbuffer _ : register(b##index)
#define f32 float
#define v2f float2
#define v3f float3
#define v4f float4
#define m4 float4x4
)" \
STRINGIZE(DECLARE_SCENE_CBUFFER) \
STRINGIZE(DECLARE_GLOBAL_CBUFFER) \
STRINGIZE(DECLARE_PIE_CBUFFER) \
STRINGIZE(DECLARE_COLOR_CBUFFER) \
STRINGIZE(DECLARE_ENTITY_CBUFFER) \
R"(
float2 sceneToNDC(float2 p) { return (p - scenePosition) * sceneScale; }
float2 sceneToNDC(float2 p, float2 offset) { return (p - scenePosition + offset) * sceneScale; }
float2 windowToNDC(float2 p) { return mul(matrixWindowToNDC, float4(p, 0, 1)).xy; }
float4 getImagePosition(float2 v) {
	return float4(sceneToNDC(entityPosition, mul(entityRotation, float4((v - 0.5f) * imageSize, 0, 1)).xy), 0, 1);
}
#undef DECLARE_CBUFFER
)"

extern HWND mainWindow;


#define R_DECORATE(ret, name, args, params) ret RendererImpl::name args
R_initScene {
	auto data = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, SceneData, 1, 0));
	data->constantBuffer = createConstantBuffer<SceneConstantBufferData>(D3D11_USAGE_DEFAULT);
	data->constantBufferData.sceneDrawThickness = scene->windowDrawThickness;
	scene->renderData = data;
}
R_releaseScene {
	release(SCENE_DATA(scene->renderData).canvasRT); 
	release(SCENE_DATA(scene->renderData).canvasRTMS); 
	release(SCENE_DATA(scene->renderData).constantBuffer); 
	DEALLOCATE(TL_DEFAULT_ALLOCATOR, scene->renderData);
	scene->renderData = 0;
}
R_resize {
	resizeBackBuffer(clientSize.x, clientSize.y);
}
R_initPencilEntity {
	pencil.renderData = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, LineData, 1, 0));
}
R_initLineEntity {
	line.renderData = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, LineData, 1, 0));
}
R_initGridEntity {
	grid.renderData = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, LineData, 1, 0));
}
R_initCircleEntity {
	circle.renderData = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, LineData, 1, 0));
}
R_createImageData {
	auto data = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, ImageData, 1, 0));
	data->texture = unloadedTexture;
	return data;
}
R_initPencilEntityData{
	List<TransformedLine> transformedLines;
	transformedLines.reserve(pencil.lines.size());
	for (auto line : pencil.lines) {
		transformedLines.push_back(transform(line));
	}
	initConstantLineArray(pencil.renderData, transformedLines.data(), transformedLines.size());
}
R_initLineEntityData{
	TransformedLine transformedLine = transform(line.line);
	initConstantLineArray(line.renderData, &transformedLine, 1);
}
R_initGridEntityData{
	auto lines = getTransformedGridLines(grid);
	initConstantLineArray(grid.renderData, lines.data(), lines.size());
}
R_initCircleEntityData{
	initConstantLineArray(circle.renderData, getTransformedCircleLines(circle).data(), CircleEntity::LINE_COUNT);
}
R_onColorMenuOpen{
	switch (target) {
		case ColorMenuTarget_draw:
			SCENE_DATA(currentScene->renderData).constantBufferData.colorMenuColor = currentScene->drawColor;
			break;
		case ColorMenuTarget_canvas:
			SCENE_DATA(currentScene->renderData).constantBufferData.colorMenuColor = currentScene->canvasColor;
			break;
	}
}
R_setColorMenuColor{
	SCENE_DATA(scene->renderData).constantBufferData.colorMenuColor = rgb;
}
R_repaint{
	SCOPED_LOCK(immediateContextMutex);

	if (windowSizeDirty) {
		windowSizeDirty = false;
		globalConstantBufferDirty = true;
		globalConstantBufferData.matrixWindowToNDC = m4::translation(-1, -1, 0) * m4::scaling(2.0f / (v2f)clientSize, 1);
		globalConstantBufferData.clientSize = (v2f)clientSize;
		globalConstantBufferData.windowAspect = (f32)clientSize.x / clientSize.y;
	}
	if (globalConstantBufferDirty) {
		updateConstantBuffer(globalConstantBuffer, &globalConstantBufferData);
	}

	setViewport(0, 0, clientSize.x, clientSize.y);

	if (shouldRepaint(currentScene)) {
		repaintScene(currentScene);
	}
	if (playingSceneShiftAnimation) {
		if (shouldRepaint(previousScene)) {
			repaintScene(previousScene);
		}
	}

	if (needRepaint) {
		needRepaint = false;

		setRenderTarget(backBuffer);
		setShader(blitShader.vs);
		setShader(blitShader.ps);
		setRasterizer(defaultRasterizerNoMs);
		setBlend();

		if (playingSceneShiftAnimation) {
			if (currentScene > previousScene)
				setViewport(lerp(0.0f, -(f32)clientSize.x, sceneShiftT), 0, clientSize.x, clientSize.y);
			else
				setViewport(lerp(0.0f, clientSize.x, sceneShiftT), 0, clientSize.x, clientSize.y);

			setShaderResource(SCENE_DATA(previousScene->renderData).canvasRT, 'P', 0);
			draw(3);

			if (currentScene > previousScene)
				setViewport(lerp(clientSize.x, 0.0f, sceneShiftT), 0, clientSize.x, clientSize.y);
			else
				setViewport(lerp(-(f32)clientSize.x, 0.0f, sceneShiftT), 0, clientSize.x, clientSize.y);
		}
		else {
			setViewport(clientSize.x, clientSize.y);
		}

		setShaderResource(SCENE_DATA(currentScene->renderData).canvasRT, 'P', 0);
		draw(3);
		setBlend(alphaBlend);

		StaticList<Quad, 256> quads;

		auto makeQuad = [](v2f position, v2f size, v2u uv, v2u uvSize, v4f color) {
			Quad q;
			q.position = position;
			q.position.y -= size.y;
			q.size = size;
			q.color = color;
			setUv(q, uv, uvSize);
			return q;
		};
		auto pushShadowedQuad = [&](v2f position, v2f size, v2u uv, v2u uvSize, v4f color, f32 shadowSize = 2.0f) {
			auto quad = makeQuad(position, size, uv, uvSize, color);
			auto shadow = quad;
			shadow.position += v2f{ 1,-2 };
			shadow.color.xyz *= 0;
			quads.push_back(shadow);
			quads.push_back(quad);
		};
		auto pushQuad = [&](v2f position, v2f size, v2u uv, v2u uvSize, v4f color) {
			quads.push_back(makeQuad(position, size, uv, uvSize, color));
		};
		auto drawPieMenu = [&](PieMenu& menu) {
			updatePieBuffer(menu);
			if (menu.animValue) {
				setConstantBuffer(PIE_DATA(menu.renderData).constantBuffer, 'V', 2);
				setShader(pieSelShader.vs);
				setShader(pieSelShader.ps);

				draw(PIE_SEL_VERT_COUNT);

				for (u32 i = 0; i < menu.items.size(); ++i) {
					v2f offset = m2::rotation(map(i, 0.0f, menu.items.size(), 0.0f, pi * 2)) * V2f(-pieMenuSize, 0);
					auto& item = menu.items[i];
					v2f size = V2f(toolImageSize);
					v2f position = round(menu.position + offset * menu.animValue + v2f{ -1, 1 } *size * 0.5f);
					v4f color = V4f(1, 1, 1, menu.animValue);

					pushQuad(position - size * 0.5f * v2f{ 1,-1 }, size * 2, { 2, 2 }, { 2, 2 }, V4f(V3f(0.5f), menu.animValue));
					pushShadowedQuad(position, size, item.uv, { 1,1 }, color);

					//position.x += size.x * 0.5f;
					//position.y -= size.y * 0.5f;
					//size *= 0.5f;
					if (item.type == PieMenuItem_toolColor) {
						pushShadowedQuad(position, size, { 2, 0 }, { 1,1 }, color);
					}
					else if (item.type == PieMenuItem_canvasColor) {
						pushShadowedQuad(position, size, { 3, 1 }, { 1,1 }, color);
					}
				}
			}
		};

		drawPieMenu(mainPieMenu);
		//drawPieMenu(toolPieMenu);
		//drawPieMenu(canvasPieMenu);

		if (drawCursorCircle) {
			setShader(circleShader.vs);
			setShader(circleShader.ps);
			setRasterizer(defaultRasterizer);
			draw(CIRCLE_VERTEX_COUNT);
		}
		if (drawCursor) {
			pushShadowedQuad((v2f)mousePosBL, V2f(toolImageSize), getUv(currentScene->tool), { 1,1 }, V4f(1));
		}
		updateStructuredBuffer(uiSBuffer, quads.size(), sizeof(Quad), quads.data());
		setShader(quadShader.vs);
		setShader(quadShader.ps);
		setShaderResource(uiSBuffer, 'V', 0);
		setShaderResource(toolAtlas, 'P', 0);
		setRasterizer(defaultRasterizerNoMs);
		draw(quads.size() * VERTS_PER_QUAD);

		if (colorMenuAnimValue) {
			colorConstantBufferData.colorMenuSize = minWindowDim * 0.2f;
			colorConstantBufferData.colorMenuPosition = v2f{ colorMenuPositionTL.x, clientSize.y - colorMenuPositionTL.y };
			colorConstantBufferData.colorMenuAlpha = colorMenuAnimValue;
			colorConstantBufferData.colorMenuHueColor = hsvToRgb(colorMenuHue, 1, 1);
			colorConstantBufferData.colorMenuHue = colorMenuHue;
			updateConstantBuffer(colorConstantBuffer, &colorConstantBufferData);

			setShader(colorMenuShader.vs);
			setShader(colorMenuShader.ps);
			setRasterizer(defaultRasterizer);
			draw(9);
		}

		swapChain->Present(vSync, 0);
	}
	else {
		Sleep(16);
	}
	debugPoints.clear();
}
R_initConstantLineArray{
	LINE_DATA(renderData).buffer = createStructuredBuffer(D3D11_USAGE_IMMUTABLE, count, sizeof(TransformedLine), data);
}
R_initDynamicLineArray{
	LINE_DATA(renderData).buffer = createStructuredBuffer(D3D11_USAGE_DEFAULT, count, sizeof(TransformedLine), data);
}
R_reinitDynamicLineArray{
	D3D11::release(LINE_DATA(renderData).buffer);
	initDynamicLineArray(renderData, data, count);
}
R_releasePencil { 
	if (action.renderData) {
		release(LINE_DATA(action.renderData).buffer); 
		DEALLOCATE(TL_DEFAULT_ALLOCATOR, action.renderData);
		action.renderData = 0;
	}
}
R_releaseLine { 
	if (action.renderData) {
		release(LINE_DATA(action.renderData).buffer); 
		DEALLOCATE(TL_DEFAULT_ALLOCATOR, action.renderData);
		action.renderData = 0;
	}
}
R_releaseGrid { 
	if (action.renderData) {
		release(LINE_DATA(action.renderData).buffer); 
		DEALLOCATE(TL_DEFAULT_ALLOCATOR, action.renderData);
		action.renderData = 0;
	}
}
R_releaseCircle { 
	if (action.renderData) {
		release(LINE_DATA(action.renderData).buffer); 
		DEALLOCATE(TL_DEFAULT_ALLOCATOR, action.renderData);
		action.renderData = 0;
	}
}
R_releaseImageData {
	auto &data = IMAGE_DATA(renderData);
	if (data.texture.srv != unloadedTexture.srv) {
		release(data.texture); 
		DEALLOCATE(TL_DEFAULT_ALLOCATOR, renderData);
	}
}
R_releaseEntity{
	switch (action.type) {
		case Entity_pencil: releasePencil(action.pencil); break;
		case Entity_line:   releaseLine(action.line); break;
		case Entity_grid:   releaseGrid(action.grid); break;
		case Entity_circle: releaseCircle(action.circle); break;
		case Entity_image:
			break;

		default: INVALID_CODE_PATH();
	}
}
R_switchRasterizer{
	wireframe = !wireframe;
}
R_pickColor{
	if (device3) {
		SCOPED_LOCK(immediateContextMutex);

		DHR(immediateContext->Map(SCENE_DATA(currentScene->renderData).canvasRT.tex, 0, D3D11_MAP_READ, 0, 0));
		D3D11_BOX box = {};
		box.left = mousePosTL.x;
		box.top = mousePosTL.y;
		box.right = box.left + 1;
		box.bottom = box.top + 1;
		box.back = 1;
		struct {
			u8 r, g, b, _;
		} pixel;
		device3->ReadFromSubresource(&pixel, 1, 1, SCENE_DATA(currentScene->renderData).canvasRT.tex, 0, &box);
		immediateContext->Unmap(SCENE_DATA(currentScene->renderData).canvasRT.tex, 0);
		currentScene->drawColor.x = pixel.r / 255.0f;
		currentScene->drawColor.y = pixel.g / 255.0f;
		currentScene->drawColor.z = pixel.b / 255.0f;
		currentScene->drawColorDirty = true;
	} else {
		LOG("How to read from texture on device1?");
	}
}
R_resizeLineArray{
	u32 bufferElemCount = LINE_DATA(renderData).buffer.size / sizeof(TransformedLine);
	if (count > bufferElemCount) {
		release(LINE_DATA(renderData).buffer);
		LINE_DATA(renderData).buffer = createStructuredBuffer(D3D11_USAGE_DEFAULT, bufferElemCount + pencilLineBufferDefElemCount, sizeof(TransformedLine), data);
	}
}
R_updateLineArray{
	updateStructuredBuffer(LINE_DATA(renderData).buffer, count, sizeof(TransformedLine), data, firstElem);
}
R_updateLines{
	List<TransformedLine> transformedLines;
	while (count--) {
		transformedLines.push_back(transform(*data++));
	}
	updateLineArray(renderData, transformedLines.data(), transformedLines.size(), firstElem);
}
R_updateGridLines{
	List<TransformedLine> lines = getTransformedGridLines(grid);
	updateLineArray(grid.renderData, lines.data(), lines.size(), 0);
}
R_updateCircleLines{
	updateLineArray(circle.renderData, getTransformedCircleLines(circle).data(), CircleEntity::LINE_COUNT, 0);
}
R_freeze{
	LINE_DATA(pencil.renderData).transformedLines = {};
}
R_resizePencilLineArray{
	resizeLineArray(pencil.renderData, LINE_DATA(pencil.renderData).transformedLines.data(), LINE_DATA(pencil.renderData).transformedLines.size());
}
R_updateLastElement{
	updateLineArray(pencil.renderData, LINE_DATA(pencil.renderData).transformedLines.end() - 1, 1, LINE_DATA(pencil.renderData).transformedLines.size() - 1);
}
R_setTexture{
	SCOPED_LOCK(immediateContextMutex);
	IMAGE_DATA(image).texture = State::createTexture(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, data, true);
}
R_updatePaintCursor{
	globalConstantBufferData.windowMousePos = windowMousePos;
	globalConstantBufferData.windowDrawColor = windowDrawColor;
	globalConstantBufferData.windowDrawThickness = TL::round(windowDrawThickness);
	globalConstantBufferDirty = true;
} 
R_isLoaded {
	return IMAGE_DATA(image.renderData).texture.srv != unloadedTexture.srv;
}
R_setUnloadedTexture{
	IMAGE_DATA(imageData).texture = unloadedTexture;
}
R_update{
	SCENE_DATA(scene->renderData).constantBufferData.sceneDrawThickness = getDrawThickness(scene);
}
R_onLinePushed{
	LINE_DATA(pencil.renderData).transformedLines.push_back(transform(line));
}
R_onLinePopped{
	LINE_DATA(pencil.renderData).transformedLines.pop_back();
}
R_getMutex {
	return immediateContextMutex;
}
R_setMultisampleEnabled {
	if (enable) {
		defaultRasterizer = defaultRasterizerMs;
		wireframeRasterizer = wireframeRasterizerMs;
		doubleRasterizer = doubleRasterizerMs;
	} else {
		defaultRasterizer = defaultRasterizerNoMs;
		wireframeRasterizer = wireframeRasterizerNoMs;
		doubleRasterizer = doubleRasterizerNoMs;
	}
}
R_isMultisampleEnabled {
	return defaultRasterizer.raster == defaultRasterizerMs.raster;
}
R_setVSync {
	vSync = enable;
}
R_shutdown {
	release(PIE_DATA(mainPieMenu.renderData).constantBuffer);
	DEALLOCATE(TL_DEFAULT_ALLOCATOR, mainPieMenu.renderData);
	DEALLOCATE(TL_DEFAULT_ALLOCATOR, this);
}
//
//#include "../dep/stb/stb_image_write.h"
//
//R_debugSaveRenderTarget {
//	auto &canvas = SCENE_DATA(currentScene->renderData).canvasRT;
//
//	D3D11_TEXTURE2D_DESC d{};
//	d.ArraySize = 1;
//	d.BindFlags = 0;
//	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
//	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
//	d.Width = clientSize.x;
//	d.Height = clientSize.y;
//	d.MipLevels = 1;
//	d.Usage = D3D11_USAGE_STAGING;
//	d.SampleDesc = {1, 0};
//
//	ID3D11Texture2D *t;
//	DHR(device->CreateTexture2D(&d, 0, &t));
//	immediateContext->CopyResource(t, canvas.tex);
//
//	D3D11_MAPPED_SUBRESOURCE m;
//	DHR(immediateContext->Map(t, 0, D3D11_MAP_READ, 0, &m));
//	if (!stbi_write_png("render_target.png", clientSize.x, clientSize.y, 4, m.pData, clientSize.x * 4)) {
//		DEBUG_BREAK;
//	}
//	immediateContext->Unmap(t, 0);
//
//	t->Release();
//}
#undef R_DECORATE

Renderer *createRenderer() {
	return construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, RendererImpl, 1, 0));
}

RendererImpl::RendererImpl() {
	UINT deviceFlags = 0;
#if BUILD_DEBUG
	deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	D3D11::initState(*this, mainWindow, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, true, -1, deviceFlags);
	
	msaaSampleCount = getMaxMsaaSampleCount(DXGI_FORMAT_R8G8B8A8_UNORM);
	
	auto work = makeWorkQueue(&threadPool);

	setTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	
	globalConstantBuffer = createConstantBuffer<GlobalConstantBufferData>(D3D11_USAGE_DEFAULT);
	setConstantBuffer(globalConstantBuffer, 'V', 1);
	setConstantBuffer(globalConstantBuffer, 'P', 1);
	
	colorConstantBuffer = createConstantBuffer<ColorConstantBufferData>(D3D11_USAGE_DEFAULT);
	setConstantBuffer(colorConstantBuffer, 'V', 3);
	setConstantBuffer(colorConstantBuffer, 'P', 3);

	entityConstantBuffer = createConstantBuffer<EntityConstantBufferData>(D3D11_USAGE_DEFAULT);
	setConstantBuffer(entityConstantBuffer, 'V', 4);
	setConstantBuffer(entityConstantBuffer, 'P', 4);

	mainPieMenu.renderData = construct(ALLOCATE_T(TL_DEFAULT_ALLOCATOR, PieData, 1, 0));
	PIE_DATA(mainPieMenu.renderData).constantBuffer = createConstantBuffer<PieConstantBufferData>(D3D11_USAGE_DEFAULT);

	
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
#define VERTS_PER_LINE )" STRINGIZE(VERTS_PER_LINE) R"(

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

#define HALF(x, a, b)								      \
	{CS(x+270), a}, {CS(x+90), a}, {CS(x+270), b},        \
															\
	{CS(x+180), a}, {CS(x+90), a}, {CS(x+270), a},	      \
															\
	{CS(x+135), a}, {CS(x+ 90), a}, {CS(x+180), a},	      \
	{CS(x+225), a}, {CS(x+180), a}, {CS(x+270), a},	      \
															\
	{CS(x+112.5), a}, {CS(x+ 90), a}, {CS(x+135), a},     \
	{CS(x+157.5), a}, {CS(x+135), a}, {CS(x+180), a},     \
	{CS(x+202.5), a}, {CS(x+180), a}, {CS(x+225), a},     \
	{CS(x+247.5), a}, {CS(x+225), a}, {CS(x+270), a},     \
															\
	{CS(x+101.25), a}, {CS(x+90   ), a}, {CS(x+112.5), a},\
	{CS(x+123.75), a}, {CS(x+112.5), a}, {CS(x+135  ), a},\
	{CS(x+146.25), a}, {CS(x+135  ), a}, {CS(x+157.5), a},\
	{CS(x+168.75), a}, {CS(x+157.5), a}, {CS(x+180  ), a},\
	{CS(x+191.25), a}, {CS(x+180  ), a}, {CS(x+202.5), a},\
	{CS(x+213.75), a}, {CS(x+202.5), a}, {CS(x+225  ), a},\
	{CS(x+236.25), a}, {CS(x+225  ), a}, {CS(x+247.5), a},\
	{CS(x+258.75), a}, {CS(x+247.5), a}, {CS(x+270  ), a}

static const Vertex vertexBuffer[] = {
	HALF(  0, 0, 1),
	HALF(180, 1, 0),
};
float4 main(in In i) : SV_Position {
	Out o;
	Vertex vertex = vertexBuffer[i.id % VERTS_PER_LINE];
	Line l = lines[i.id / VERTS_PER_LINE];
	float2x2 transform = lerp(l.a.transform, l.b.transform, vertex.t);
	float2 position = lerp(l.a.position, l.b.position, vertex.t);
	return float4(sceneToNDC(mul(entityRotation, float4(mul(transform, vertex.position * thicknessMult) + position, 0, 1)).xy + entityPosition), 0, 1);
}
)";

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
float4 main() : SV_Target {
	return float4(entityColor, 1.0f);
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);

		lineShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "line_vs");
		lineShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "line_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
struct Out {
	float2 uv : UV;
	float4 hueColor : COLOR;
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
	float4 hueColor;
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
	o.hueColor = quad.hueColor;
	return o;
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
Texture2D srcTex : register(t0);
SamplerState samplerState;
float4 main(in float2 uv : UV, in float4 hueColor : COLOR) : SV_Target {
	return srcTex.Sample(samplerState, uv) * hueColor;
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		quadShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "quad_vs");
		quadShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "quad_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
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
	o.position = float4(vertexData[id], 0, 1);
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
Texture2D srcTex : register(t0);
SamplerState smp : register(s0);
float4 main(in float2 uv : UV) : SV_Target {
	return srcTex.Sample(smp, uv);
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		blitShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "blit_vs");
		blitShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "blit_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
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
	o.uv *= clientSize;
	o.position = float4(vertexData[id], 0, 1);
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

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
	float4 hueColor = 0;
	for (uint i = 0; i < MSAA_SAMPLE_COUNT; ++i)
		hueColor += srcTex.Load(uv, i);
	return hueColor / MSAA_SAMPLE_COUNT;// + float4(uv / 1000, 0, 1);
}
#endif
)", msaaSampleCount);
		blitShaderMS.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "blitms_vs");
		blitShaderMS.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "blitms_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
#define CIRCLE_WIDTH )" STRINGIZE(CIRCLE_WIDTH) R"(
#define CIRCLE_OUTLINE_WIDTH )" STRINGIZE(CIRCLE_OUTLINE_WIDTH) R"(
#define CIRCLE_VERTEX_COUNT )" STRINGIZE(CIRCLE_VERTEX_COUNT) R"(
#define DEG(x) ((x) / 180.0f * 3.1415926535897932384626433832795f)
#define STEP 5
#define LINE(x) \
	{cos(DEG(x)) * 0.5f, sin(DEG(x)) * 0.5f, 0}, \
	{cos(DEG(x)) * 0.5f, sin(DEG(x)) * 0.5f, 1}, \
	{cos(DEG(x+STEP)) * 0.5f, sin(DEG(x+STEP)) * 0.5f, 1}, \
	{cos(DEG(x+STEP)) * 0.5f, sin(DEG(x+STEP)) * 0.5f, 0}, \
	{cos(DEG(x)) * 0.5f, sin(DEG(x)) * 0.5f, 0}, \
	{cos(DEG(x+STEP)) * 0.5f, sin(DEG(x+STEP)) * 0.5f, 1}

struct Vertex {
	float2 position;
	float inner;
};
static const Vertex vertexData[] = {
	LINE(  0), LINE(  5),
	LINE( 10), LINE( 15),
	LINE( 20), LINE( 25),
	LINE( 30), LINE( 35),
	LINE( 40), LINE( 45),
	LINE( 50), LINE( 55),
	LINE( 60), LINE( 65),
	LINE( 70), LINE( 75),
	LINE( 80), LINE( 85),
	LINE( 90), LINE( 95),
	LINE(100), LINE(105),
	LINE(110), LINE(115),
	LINE(120), LINE(125),
	LINE(130), LINE(135),
	LINE(140), LINE(145),
	LINE(150), LINE(155),
	LINE(160), LINE(165),
	LINE(170), LINE(175),
	LINE(180), LINE(185),
	LINE(190), LINE(195),
	LINE(200), LINE(205),
	LINE(210), LINE(215),
	LINE(220), LINE(225),
	LINE(230), LINE(235),
	LINE(240), LINE(245),
	LINE(250), LINE(255),
	LINE(260), LINE(265),
	LINE(270), LINE(275),
	LINE(280), LINE(285),
	LINE(290), LINE(295),
	LINE(300), LINE(305),
	LINE(310), LINE(315),
	LINE(320), LINE(325),
	LINE(330), LINE(335),
	LINE(340), LINE(345),
	LINE(350), LINE(355),
};

void main(in uint id : SV_VertexId, out float inner : INNER, out float4 position : SV_Position) {
	Vertex v = vertexData[id];
	position = float4(windowToNDC(windowMousePos + v.position * max(windowDrawThickness - (1<<(CIRCLE_WIDTH+1)) * v.inner, 0)), 0, 1);
	inner = v.inner;
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
#define CIRCLE_WIDTH )" STRINGIZE(CIRCLE_WIDTH) R"(
#define CIRCLE_OUTLINE_WIDTH )" STRINGIZE(CIRCLE_OUTLINE_WIDTH) R"(
#define MULT (1<<(CIRCLE_WIDTH-CIRCLE_OUTLINE_WIDTH+1))
#define OFFS (-((1<<(CIRCLE_WIDTH-CIRCLE_OUTLINE_WIDTH))-1))

float4 main(in float inner : INNER) : SV_Target {
	return float4(lerp(windowDrawColor, inner, max(0, abs(inner - 0.5f) * MULT + OFFS)), 1);
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		circleShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "circle_vs");
		circleShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "circle_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
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
	{{0, 0}, 0.5f}, {{-MCOS( 0  ), MSIN( 0  )}, 0.0f}, {{-MCOS(33.7), MSIN(33.7)}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(33.7), MSIN(33.7)}, 0.0f}, {{-MCOS(67.5), MSIN(67.5)}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(67.5), MSIN(67.5)}, 0.0f}, {{  COS(22  ),  SIN(22  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(22  ),  SIN(22  )}, 0.0f}, {{  COS(11  ),  SIN(11  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(11  ),  SIN(11  )}, 0.0f}, {{  COS( 0  ),  SIN( 0  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS( 0  ),  SIN( 0  )}, 0.0f}, {{  COS(11  ), -SIN(11  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(11  ), -SIN(11  )}, 0.0f}, {{  COS(22  ), -SIN(22  )}, 0.0f},
	{{0, 0}, 0.5f}, {{  COS(22  ), -SIN(22  )}, 0.0f}, {{-MCOS(67.5),-MSIN(67.5)}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(67.5),-MSIN(67.5)}, 0.0f}, {{-MCOS(33.7),-MSIN(33.7)}, 0.0f},
	{{0, 0}, 0.5f}, {{-MCOS(33.7),-MSIN(33.7)}, 0.0f}, {{-MCOS( 0  ),-MSIN( 0  )}, 0.0f},
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

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
float4 main(in float alpha : ALPHA) : SV_Target {
	return float4(1, 1, 1, alpha);
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		pieSelShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "pieSel_vs");
		pieSelShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "pieSel_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = 
	SHADER_COMMON_SOURCE R"(
#define SV_TRIANGLE_SIZE )" STRINGIZE(SV_TRIANGLE_SIZE) R"(
struct Vertex {
	float2 position;
	float3 hueColor;
	float type;
};
static const Vertex vertices[] = {
	{{ 0,                 1  }, {1, 0, 0}, 1},
	{{ cos(radians(30)), -0.5}, {0, 1, 0}, 1},
	{{-cos(radians(30)), -0.5}, {0, 0, 1}, 1},
	{float2( 0,                 1  ) * 0.8, {1,1,0}, 2},
	{float2( cos(radians(30)), -0.5) * 0.8, {1,1,0}, 2},
	{float2(-cos(radians(30)), -0.5) * 0.8, {1,1,0}, 2},
	{float2( 0,                 1  ) * SV_TRIANGLE_SIZE, {1,    1, 0}, 0},
	{float2( cos(radians(30)), -0.5) * SV_TRIANGLE_SIZE, {0,    1, 0}, 0},
	{float2(-cos(radians(30)), -0.5) * SV_TRIANGLE_SIZE, {0.5F, 0, 1}, 0},
};
void main(out float3 hueColor : COLOR, out float type : TYPE, out float4 position : SV_Position, in uint id : SV_VertexId) {
	Vertex v = vertices[id];
	type = v.type;
	hueColor = v.hueColor;
	position = mul(matrixWindowToNDC, float4(colorMenuPosition + v.position * colorMenuSize, 0, 1));
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
#define PI 3.1415926535897932384626433832795f
float map(float v, float sn, float sx, float dn, float dx) { return (v - sn) / (sx - sn) * (dx - dn) + dn; }
float4 main(in float3 hueColor : COLOR, in float type : TYPE) : SV_Target {
	float4 result = 1;
	if (type > 1.5f) {
		result.rgb = colorMenuColor;
	} else if (type > 0.5f) {
		result.rgb = hueColor / max(hueColor.r, max(hueColor.g, hueColor.b));
	} else {
		result.rgb = lerp(1, colorMenuHueColor, hueColor.x) * hueColor.y;
	}
	result.a = colorMenuAlpha;
	return result;
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		colorMenuShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "colorMenu_vs");
		colorMenuShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "colorMenu_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
static const float2 vertexData[] = {
	{0, 0},
	{0, 1},
	{1, 0},
	{0, 1},
	{1, 1},
	{1, 0},
};
void main(out float2 uv : UV, out float4 position : SV_Position, in uint id : SV_VertexId) {
	float2 v = vertexData[id];
	uv = v;
	uv.y = 1 - uv.y;
	position = getImagePosition(v);
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
Texture2D image : register(t0);
SamplerState samplerState;
float4 main(in float2 uv : UV) : SV_Target {
	return image.Sample(samplerState, uv);
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		imageShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "image_vs");
		imageShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "image_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
static const float2 vertexData[] = {
	{0.0, 0.0},
	{0.0, 1.0},
	{0.1, 0.0},
	{0.1, 1.0},
	{0.9, 0.0},
	{0.9, 1.0},
	{1.0, 0.0},
	{1.0, 1.0},

	{0.0, 0.0},
	{1.0, 0.0},
	{0.0, 0.1},
	{1.0, 0.1},
	{0.0, 0.9},
	{1.0, 0.9},
	{0.0, 1.0},
	{1.0, 1.0},
};
void main(out float4 position : SV_Position, in uint id : SV_VertexId) {
	float2 v = vertexData[id];
	position = getImagePosition(v);
}
)";
	u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

	char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
float4 main() : SV_Target {
	return 1;
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		imageOutlineShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "imageOutline_vs");
		imageOutlineShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "imageOutline_ps");
	});
	work.push([&]  {
		char vertexShaderSourceData[] = SHADER_COMMON_SOURCE R"(
static const float2 vertexData[] = {
	{0, 0}, {0, 1},
	{1, 0}, {1, 1},
	{0, 0}, {1, 0},
	{0, 1}, {1, 1},
};
void main(out float4 position : SV_Position, in uint id : SV_VertexId) {
	float2 v = vertexData[id];
	position = float4(sceneToNDC(lerp(boundsMin, boundsMax, v)), 0, 1);
}
)";
		u32 const vertexShaderSourceSize = sizeof(vertexShaderSourceData);

		char pixelShaderSourceData[] = SHADER_COMMON_SOURCE R"(
float4 main() : SV_Target {
	return 1;
}
)";
		u32 const pixelShaderSourceSize = sizeof(pixelShaderSourceData);
		boundsShader.vs = createVertexShader(vertexShaderSourceData, vertexShaderSourceSize, "bounds_vs");
		boundsShader.ps = createPixelShader(pixelShaderSourceData, pixelShaderSourceSize, "bounds_ps");
	});

	work.push([&] {
		auto sampler = createSampler(D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_FILTER_ANISOTROPIC);
		useContext([&] { 
			setSampler(sampler, 'P', 0);
		});
	});

	work.push([&] {
		alphaBlend = createBlend(
			D3D11_BLEND_OP_ADD, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA
		   //,D3D11_BLEND_OP_MAX, D3D11_BLEND_ONE, D3D11_BLEND_ONE
		);
	});
	
	{
		static constexpr u32 atlasData[] {
#include "atlas.h"
		};
		static constexpr u32 atlasSize = 256;
		static_assert(countof(atlasData) == atlasSize * atlasSize);
		work.push([&] {
			toolAtlas = State::createTexture(atlasSize, atlasSize, DXGI_FORMAT_R8G8B8A8_UNORM, atlasData, true);
		});
	
		u32 pixel = 0xFF000000;
		unloadedTexture = State::createTexture(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, &pixel);
	}

	uiSBuffer = createStructuredBuffer(D3D11_USAGE_DYNAMIC, 256, sizeof(Quad));
	
	work.push([&] {
		defaultRasterizerNoMs   = createRasterizer(D3D11_FILL_SOLID, D3D11_CULL_BACK);
		defaultRasterizerMs     = createRasterizer(D3D11_FILL_SOLID, D3D11_CULL_BACK, true, true);
		wireframeRasterizerNoMs = createRasterizer(D3D11_FILL_WIREFRAME, D3D11_CULL_BACK);
		wireframeRasterizerMs   = createRasterizer(D3D11_FILL_WIREFRAME, D3D11_CULL_BACK, true, true);
		doubleRasterizerNoMs    = createRasterizer(D3D11_FILL_SOLID, D3D11_CULL_NONE);
		doubleRasterizerMs      = createRasterizer(D3D11_FILL_SOLID, D3D11_CULL_NONE, true, true);
		setMultisampleEnabled(true);
	});
	
	work.waitForCompletion();


#define ADD_IMPL(...) (RendererImpl *impl, __VA_ARGS__)
#define R_DECORATE(ret, name, args, params) CONCAT(_, name) = [] ADD_IMPL args -> ret { return impl->name params; };
	R_all
#undef R_DECORATE
#undef ADD_IMPL
}
void RendererImpl::drawEntity(Entity const &e) {
	SCOPED_LOCK(immediateContextMutex);
	
	setTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	switch (e.type) {
		case Entity_none:
			INVALID_CODE_PATH();
			break;
		case Entity_circle:
		case Entity_grid:
		case Entity_line:
		case Entity_pencil: {
			setShader(lineShader.vs);
			setShader(lineShader.ps);
			setRasterizer(wireframe ? wireframeRasterizer : defaultRasterizer);
		} break;
		case Entity_image: {
			setShader(imageShader.vs);
			setShader(imageShader.ps);
		} break;
	}

	EntityConstantBufferData data;
	data.entityRotation = m4::rotationZ(-e.rotation);
	data.entityPosition = e.position;
	data.thicknessMult = 1.0f;
	data.boundsMin = e.bounds.min;
	data.boundsMax = e.bounds.max;

	if (e.hovered && e.type != Entity_image) {
		data.entityColor = V3f(1);
		updateConstantBuffer(entityConstantBuffer, &data);

		switch (e.type) {
			case Entity_pencil: {
				setShaderResource(LINE_DATA(e.pencil.renderData).buffer, 'V', 0);
				draw(e.pencil.lines.size() * VERTS_PER_LINE);
			} break;
			case Entity_line: {
				setShaderResource(LINE_DATA(e.line.renderData).buffer, 'V', 0);
				draw(VERTS_PER_LINE);
			} break;
			case Entity_grid: {
				u32 lineCount = e.grid.cellCount.x + e.grid.cellCount.y + 2;
				setShaderResource(LINE_DATA(e.grid.renderData).buffer, 'V', 0);
				draw(lineCount * VERTS_PER_LINE);
			} break;
			case Entity_circle: {
				setShaderResource(LINE_DATA(e.circle.renderData).buffer, 'V', 0);
				draw(e.circle.LINE_COUNT * VERTS_PER_LINE);
			} break;
		}
		data.thicknessMult = 0.8f;
	}
	
	switch (e.type) {
		case Entity_pencil: data.entityColor = e.pencil.color; break;
		case Entity_line:   data.entityColor = e.line.color;   break;
		case Entity_grid:   data.entityColor = e.grid.color;   break;
		case Entity_circle: data.entityColor = e.circle.color; break;
		case Entity_image: {
			auto &image = e.image;
			data.imageSize = image.size;
			break;
		}
	}
	updateConstantBuffer(entityConstantBuffer, &data);

	switch (e.type) {
		case Entity_pencil: {
			setShaderResource(LINE_DATA(e.pencil.renderData).buffer, 'V', 0);
			draw(e.pencil.lines.size() * VERTS_PER_LINE);
		} break;
		case Entity_line: {
			setShaderResource(LINE_DATA(e.line.renderData).buffer, 'V', 0);
			draw(VERTS_PER_LINE);
		} break;
		case Entity_grid: {
			u32 lineCount = e.grid.cellCount.x + e.grid.cellCount.y + 2;
			setShaderResource(LINE_DATA(e.grid.renderData).buffer, 'V', 0);
			draw(lineCount * VERTS_PER_LINE);
		} break;
		case Entity_circle: {
			setShaderResource(LINE_DATA(e.circle.renderData).buffer, 'V', 0);
			draw(e.circle.LINE_COUNT * VERTS_PER_LINE);
		} break;
		case Entity_image: {
			auto &image = e.image;
			setShaderResource(IMAGE_DATA(image.renderData).texture, 'P', 0);
			setRasterizer(doubleRasterizer);
			draw(6);
			if (image.hovered) {
				setTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
				setShader(imageOutlineShader.vs);
				setShader(imageOutlineShader.ps);
				setRasterizer(defaultRasterizer);
				draw(16);
			}
		} break;
		default: INVALID_CODE_PATH();
	}

	if (drawBounds) {
		setTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
		setShader(boundsShader.vs);
		setShader(boundsShader.ps);
		setRasterizer(defaultRasterizer);
		draw(8);
	}
}
void RendererImpl::updatePieBuffer(PieMenu &menu) {
	if (menu.animValue) {
		PieConstantBufferData data;
		data.piePos = menu.position;
		data.pieSize = pieMenuSize;
		data.pieAlpha = menu.animValue * menu.handAlphaValue;
		data.pieAngle = menu.angle;
		updateConstantBuffer(PIE_DATA(menu.renderData).constantBuffer, &data);
	}
}
void RendererImpl::repaintScene(Scene *scene) {
	scene->needRepaint = false;
	needRepaint = true;
	
	auto &sceneData = SCENE_DATA(scene->renderData);

	if (scene->needResize) {
		scene->needResize = false;
		D3D11::release(sceneData.canvasRT);
		D3D11::release(sceneData.canvasRTMS);
		sceneData.canvasRT = createRenderTexture(clientSize.x, clientSize.y, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_CPU_ACCESS_READ);
		sceneData.canvasRTMS = createRenderTexture(clientSize.x, clientSize.y, msaaSampleCount, DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	if (scene->matrixSceneToNDCDirty) {
		scene->matrixSceneToNDCDirty = false;
		scene->constantBufferDirty = true;
		sceneData.constantBufferData.scenePosition = scene->cameraPosition;
		sceneData.constantBufferData.sceneScale = reciprocal(scene->cameraDistance) / (v2f)clientSize * 2.0f;
		//scene->constantBufferData.matrixSceneToNDC = m4::scaling(reciprocal(scene->cameraDistance)) * m4::scaling(2.0f / (v2f)clientSize, 1) * m4::translation(-scene->cameraPosition, 0);
	}
	if (scene->drawColorDirty) {
		scene->drawColorDirty = false;
		sceneData.constantBufferData.sceneDrawColor = scene->drawColor;
		scene->constantBufferDirty = true;
	}
	if (scene->constantBufferDirty) {
		scene->constantBufferDirty = false;
		sceneData.constantBufferData.sceneDrawThickness = getDrawThickness(currentScene);
		updateConstantBuffer(sceneData.constantBuffer, &sceneData.constantBufferData);
	} 

	{
		auto &rt = isMultisampleEnabled() ? sceneData.canvasRTMS : sceneData.canvasRT;
		clearRenderTarget(rt, V4f(scene->canvasColor, 1.0f).data());
		setRenderTarget(rt);
	}
	setConstantBuffer(sceneData.constantBuffer, 'V', 0);
	setConstantBuffer(sceneData.constantBuffer, 'P', 0);
	
	setBlend(alphaBlend);

	for (auto &[id, entity] : scene->entities) {
		if (entity.visible)
			drawEntity(entity);
	}
	//{
	//	SCOPED_LOCK(currentNetEntity.mutex);
	//	drawEntity(currentNetEntity.action);
	//}
	//for (Entity *action : scene->extraEntitysToDraw) {
	//	drawEntity(*action);
	//}
	
	setBlend();
	setTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	
#if 0
	setRasterizer(defaultRasterizer);
	auto oldCbData = sceneData.constantBufferData;
	for (auto p : debugPoints) {
		sceneData.constantBufferData.sceneMousePos = p.position;
		sceneData.constantBufferData.sceneDrawColor = p.color;
		sceneData.constantBufferData.sceneDrawThickness = scene->cameraDistance * 16;
		immediateContext->UpdateSubresource(sceneData.constantBuffer, 0, 0, &sceneData.constantBufferData, 0, 0);

		immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

		immediateContext->VSSetShader(circleShader.vs, 0, 0);
		immediateContext->PSSetShader(circleShader.ps, 0, 0);
		draw(CIRCLE_VERTEX_COUNT);

		immediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}
	sceneData.constantBufferData = oldCbData;
	immediateContext->UpdateSubresource(sceneData.constantBuffer, 0, 0, &sceneData.constantBufferData, 0, 0);
#endif
	if (isMultisampleEnabled()) {
		setRenderTarget(sceneData.canvasRT);
		setShaderResource(sceneData.canvasRTMS, 'P', 0);
		setShader(blitShaderMS.vs);
		setShader(blitShaderMS.ps);
		setRasterizer(defaultRasterizerNoMs);
		draw(3);
	}
}
