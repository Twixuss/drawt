#define STBI_NO_STDIO
#include "../dep/stb/stb_image.h"

#define TL_IMPL
#include "../dep/tl/include/tl/math.h"
#include "../dep/tl/include/tl/file.h"
using namespace TL;

#include <stdio.h>
#include <Windows.h>

#define wcsequ(a, b) (wcscmp(a, b) == 0)

enum TransparencyMode {
	Transparency_keep,
	Transparency_black,
	Transparency_adjacent,
};

enum UvMode {
	Uv_wrap,
	Uv_clamp,
};

void printUsage() {
	puts(R"(
Usage: image2cpp <image> <header>
Options:
        What to do with fully transparent pixels
--transparent-keep (default)
    Leave fully transparent pixels as they are
--transparent-black
    Replace fully transparent pixels with black color
--transparent-adjacent
    Replace fully transparent pixels with average color of adjacent non-transparent pixels

        How to deal with out-of-bounds pixel fetches
--uv-wrap (default)
    Fetch from the other side
--uv-clamp
    Fetch closest pixel in bounds
)" + 1);
}
int wmain(int argc, wchar **argv) {
	if (argc < 3) {
		printUsage();
		return -1;
	}
	
	wchar *imagePath = 0;
	wchar *headerPath = 0;

	TransparencyMode transparencyMode = Transparency_keep;
	UvMode uvMode = Uv_wrap;
	bool flipY = false;

	for (int i = 1; i < argc; ++i) {
		wchar *arg = argv[i];
		if (wcsstr(arg, L"--")) {
			if (wcsequ(arg, L"--transparent-black")) {
				transparencyMode = Transparency_black;
			} else if (wcsequ(arg, L"--transparent-adjacent")) {
				transparencyMode = Transparency_adjacent;
			} else if (wcsequ(arg, L"--transparent-keep")) {
				transparencyMode = Transparency_keep;
			} else if (wcsequ(arg, L"--uv-wrap")) {
				uvMode = Uv_wrap;
			} else if (wcsequ(arg, L"--uv-clamp")) {
				uvMode = Uv_clamp;
			} else if (wcsequ(arg, L"--flip-y")) {
				flipY = true;
			} else {
				wprintf(L"Unknown parameter %s\n", arg);
				return -1;
			}
		} else {
			if (imagePath) {
				if (headerPath) {
					puts("Too much arguments");
					return -1;
				} else {
					headerPath = arg;
				}
			} else {
				imagePath = arg;
			}
		}
	}
	
	//_putws(imagePath);

	auto headerFile = _wfopen(headerPath, L"wb");
	if (!headerFile) {
		wprintf(L"Could not open %s\n", headerPath);
		return -1;
	} 
	DEFER { fclose(headerFile); };

	auto imageBuffer = readEntireFile(imagePath);
	if (!imageBuffer) {
		wprintf(L"Could not open %s\n", imagePath);
		return -1;
	} 

	int width, height;
	u32 *pixels = (u32 *)stbi_load_from_memory((stbi_uc *)imageBuffer.begin(), imageBuffer.size(), &width, &height, 0, 4);
	if (pixels) {
		DEFER { free(pixels); };
		int startY, endY, yInc;
		if (flipY) {
			startY = height - 1;
			endY = -1;
			yInc = -1;
		} else {
			startY = 0;
			endY = height;
			yInc = 1;
		}
		switch (transparencyMode) {
			case Transparency_adjacent: {
				int newWidth = width + 2;
				int newHeight = height + 2;
				u32 *newPixels = (u32 *)malloc(sizeof(u32) * newWidth * newHeight);
				switch (uvMode) {
					case Uv_wrap: {
						for (int dy = 0; dy < newHeight; ++dy) {
							for (int dx = 0; dx < newWidth; ++dx) {
								int sx = dx - 1;
								int sy = dy - 1;
								if (sx < 0) sx += width;
								if (sy < 0) sy += height;
								if (sx >= width)  sx -= width;
								if (sy >= height) sy -= height;
								newPixels[dy*newWidth + dx] = pixels[sy*width + sx];
							}
						}
					} break;
					case Uv_clamp: {
						for (int dy = 0; dy < newHeight; ++dy) {
							for (int dx = 0; dx < newWidth; ++dx) {
								int sx = dx - 1;
								int sy = dy - 1;
								if (sx < 0) sx = 0;
								if (sy < 0) sy = 0;
								if (sx >= width)  sx = width - 1;
								if (sy >= height) sy = height - 1;
								newPixels[dy*newWidth + dx] = pixels[sy*width + sx];
							}
						}
					} break;
				}
				free(pixels);
				pixels = newPixels;
				for (int y = startY; y != endY; y += yInc) {
					for (int x = 0; x < width; ++x) {
						int sx = x + 1;
						int sy = y + 1;
						u32 pixel = pixels[sy*newWidth + sx];
						if (!(pixel & 0xFF000000)) {
							u16 r = 0;
							u16 g = 0;
							u16 b = 0;
							u32 count = 0;
							auto addColor = [&] (u32 srcX, u32 srcY) {
								u32 srcPixel = pixels[srcY*newWidth + srcX];
								u32 srcA = srcPixel & 0xFF000000;
								if (srcA) {
									++count;
									r += (srcPixel >>  0) & 0xFF;
									g += (srcPixel >>  8) & 0xFF;
									b += (srcPixel >> 16) & 0xFF;
								}
							};
							addColor(sx - 1, sy - 1);
							addColor(sx,     sy - 1);
							addColor(sx + 1, sy - 1);
							addColor(sx - 1, sy    );
							addColor(sx + 1, sy    );
							addColor(sx - 1, sy + 1);
							addColor(sx,     sy + 1);
							addColor(sx + 1, sy + 1);
							if (count) {
								r /= count;
								g /= count;
								b /= count;
							}
							pixel = (r << 0) | (g << 8) | (b << 16);
						}
						fprintf(headerFile, "0x%08X, ", pixel);
					}
					fprintf(headerFile, "\n");
				}
			} break;
			case Transparency_black: {
				for (int y = startY; y != endY; y += yInc) {
					for (int x = 0; x < width; ++x) {
						u32 pixel = pixels[y*width + x];
						if (!(pixel & 0xFF000000)) {
							pixel = 0;
						}
						fprintf(headerFile, "0x%08X, ", pixel);
					}
					fprintf(headerFile, "\n");
				}
			} break;
			case Transparency_keep: {
				for (int y = startY; y != endY; y += yInc) {
					for (int x = 0; x < width; ++x) {
						fprintf(headerFile, "0x%08X, ", pixels[y*width + x]);
					}
					fprintf(headerFile, "\n");
				}
			} break;
		}
	} else {
		puts("stbi_load_from_file failed:");
		puts(stbi_failure_reason());
	}

}