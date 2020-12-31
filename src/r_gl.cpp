#include <stdio.h>
#include "os_windows.h"
#include "renderer.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wingdi.h>
#include <gl/GL.h>
#pragma comment(lib, "opengl32")
#pragma comment(lib, "gdi32")

#define VERTS_PER_QUAD 6
#define VERTS_PER_LINE 96
#define PIE_SEL_VERT_COUNT 42
#define CIRCLE_VERTEX_COUNT (72*6)

#define CIRCLE_WIDTH 2
#define CIRCLE_OUTLINE_WIDTH 1

HDC clientDc;

struct RendererImpl : Renderer {
	RecursiveMutex mutex;
	RendererImpl();

#define R_DECORATE(ret, name, args, params) ret name args;
	R_all
#undef R_DECORATE
};

extern HWND mainWindow;

#define R_DECORATE(ret, name, args, params) ret RendererImpl::name args
R_initScene {
}
R_releaseScene {
}
R_resize {
	glViewport(0, 0, clientSize.x, clientSize.y);
}
R_initPencilEntity {
}
R_initLineEntity {
}
R_initGridEntity {
}
R_initCircleEntity {
}
R_createImageData {
	return 0;
}
R_initPencilEntityData{
}
R_initLineEntityData{
}
R_initGridEntityData{
}
R_initCircleEntityData{
}
R_onColorMenuOpen{
}
R_setColorMenuColor{
}
R_repaint{
	glClearColor(0.125f, 0.125f, 0.125f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!SwapBuffers(clientDc)) {
		INVALID_CODE_PATH("SwapBuffers failed");
	}
}
R_initConstantLineArray{
}
R_initDynamicLineArray{
}
R_reinitDynamicLineArray{
}
R_releasePencil { 
}
R_releaseLine { 
}
R_releaseGrid { 
}
R_releaseCircle { 
}
R_releaseImageData {
}
R_releaseEntity{
}
R_switchRasterizer{
}
R_pickColor{
}
R_resizeLineArray{
}
R_updateLineArray{
}
R_updateLines{
}
R_updateGridLines{
}
R_updateCircleLines{
}
R_freeze{
}
R_resizePencilLineArray{
}
R_updateLastElement{
}
R_setTexture{
}
R_updatePaintCursor{
} 
R_isLoaded {
	return true;
}
R_setUnloadedTexture{
}
R_update{
}
R_onLinePushed{
}
R_onLinePopped{
}
R_getMutex {
	return mutex;
}
R_setMultisampleEnabled {
}
R_isMultisampleEnabled {
	return false;
}
R_setVSync {
}
R_shutdown {
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
	clientDc = GetDC(mainWindow);

	PIXELFORMATDESCRIPTOR dp = {};
	dp.nSize = sizeof(PIXELFORMATDESCRIPTOR);
	dp.nVersion = 1;
	dp.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
	dp.cColorBits = 32;
	dp.cAlphaBits = 8;
	dp.iPixelType = PFD_TYPE_RGBA;
	dp.iLayerType = PFD_MAIN_PLANE;

	int index = ChoosePixelFormat(clientDc, &dp);

	PIXELFORMATDESCRIPTOR sp = {};
	DescribePixelFormat(clientDc, index, sizeof(sp), &sp);

	SetPixelFormat(clientDc, index, &sp);

	HGLRC rc = wglCreateContext(clientDc);
	if (!wglMakeCurrent(clientDc, rc)) {
		INVALID_CODE_PATH("wglMakeCurrent failed");
	}

#define ADD_IMPL(...) (RendererImpl *impl, __VA_ARGS__)
#define R_DECORATE(ret, name, args, params) CONCAT(_, name) = [] ADD_IMPL args -> ret { return impl->name params; };
	R_all
#undef R_DECORATE
#undef ADD_IMPL
}