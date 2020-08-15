#define TL_IMPL
#include "../dep/tl/include/tl/math.h"
using namespace TL;

#include <stdio.h>
#include <Windows.h>

#pragma pack(push, 1)
struct BmpHeader {
   u16 type;
   u32 _size;
   u16 reserved1;
   u16 reserved2;
   u32 offBits;
   u32 size;
   s32 width;
   s32 height;
   u16 planes;
   u16 bitCount;
   u32 compression;
   u32 sizeImage;
   s32 xPelsPerMeter;
   s32 yPelsPerMeter;
   u32 clrUsed;
   u32 clrImportant;
   u32 redMask;
   u32 greenMask;
   u32 blueMask;
};
#pragma pack(pop)

enum TransparencyMode {
	Transparency_default,
	Transparency_black,
	Transparency_closest,
};

int wmain(int argc, wchar **argv) {
	if (argc < 3) {
		puts("Usage: <image> <header>");
		return -1;
	}
	
	wchar *bmpPath = 0;
	wchar *headerPath = 0;

	bool quiet = false;
	TransparencyMode transparencyMode = Transparency_default;

	for (int i = 1; i < argc; ++i) {
		wchar *arg = argv[i];
		if (wcsstr(arg, L"--")) {
			if (wcscmp(arg, L"--quiet") == 0) {
				quiet = true;
			} else if (wcscmp(arg, L"--transparent-black") == 0) {
				transparencyMode = Transparency_black;
			} else if (wcscmp(arg, L"--transparent-closest") == 0) {
				transparencyMode = Transparency_closest;
			} else {
				wprintf(L"Unknown argument %s", arg);
				return -1;
			}
		} else {
			if (bmpPath) {
				if (headerPath) {
					puts("Too much arguments");
					return -1;
				} else {
					headerPath = arg;
				}
			} else {
				bmpPath = arg;
			}
		}
	}
	
	if (!quiet) {
		_putws(bmpPath);
	}

	auto bmpFile = _wfopen(bmpPath, L"r");
	if (!bmpFile) {
		wprintf(L"could not open %s\n", bmpPath);
		return -1;
	} 
	DEFER { fclose(bmpFile); };
	fseek(bmpFile, 0, SEEK_END);
	auto bmpFileSize = ftell(bmpFile);
	fseek(bmpFile, 0, SEEK_SET);
	auto bmpData = malloc(bmpFileSize);
	DEFER { free(bmpData); };
	fread(bmpData, bmpFileSize, 1, bmpFile);

	BmpHeader *bmpHeader = (BmpHeader *)bmpData;
	if (bmpHeader->type != 'MB') {
		wprintf(L"%s is not a .bmp\n", bmpPath);
		return -1;
	}
	
	if (bmpHeader->compression != BI_RGB && bmpHeader->compression != BI_BITFIELDS) {
		wprintf(L"%s is compressed .bmp (%u), which is not supported\n", bmpPath, bmpHeader->compression);
		return -1;
	}

	if (bmpHeader->bitCount != 32) {
		puts("Only 32 bit images are supported");
		return -1;
	}

	auto headerFile = _wfopen(headerPath, L"w");
	if (!headerFile) {
		wprintf(L"could not open %s\n", headerPath);
		return -1;
	} 
	DEFER { fclose(headerFile); };

	u32 *bmpPixels = (u32 *)((u8 *)bmpData + bmpHeader->offBits);
	if (bmpHeader->compression == BI_BITFIELDS) {
		constexpr u32 dstRedBit   =  0;
		constexpr u32 dstGreenBit =  8;
		constexpr u32 dstBlueBit  = 16;
		constexpr u32 dstAlphaBit = 24;
		u32 srcRedBit = findLowestOneBit(bmpHeader->redMask);
		u32 srcGreenBit = findLowestOneBit(bmpHeader->greenMask);
		u32 srcBlueBit = findLowestOneBit(bmpHeader->blueMask);
		u32 srcAlphaBit = findLowestOneBit(~(bmpHeader->redMask | bmpHeader->greenMask | bmpHeader->blueMask));
		for (u32 y = 0; y < bmpHeader->height; ++y) {
			for (u32 x = 0; x < bmpHeader->width; ++x) {
				u32 pixel = bmpPixels[y*bmpHeader->width + x];
				u32 r = (pixel >> srcRedBit  ) & 0xFF;
				u32 g = (pixel >> srcGreenBit) & 0xFF;
				u32 b = (pixel >> srcBlueBit ) & 0xFF;
				u32 a = (pixel >> srcAlphaBit) & 0xFF;
				switch (transparencyMode) {
					case Transparency_black:
						if (!a) {
							r = g = b = 0;
						}
						break;
					case Transparency_closest:
						if (!a) {
							r = g = b = 0;
							u32 count = 0;
							auto addColor = [&] (u32 srcX, u32 srcY) {
								u32 srcPixel = bmpPixels[srcY*bmpHeader->width + srcX];
								u32 srcA = (srcPixel >> srcAlphaBit) & 0xFF;
								if (srcA) {
									++count;
									r += (srcPixel >> srcRedBit  ) & 0xFF;
									g += (srcPixel >> srcGreenBit) & 0xFF;
									b += (srcPixel >> srcBlueBit ) & 0xFF;
								}
							};
							bool lx = x;
							bool ly = y;
							bool hx = x + 1 < bmpHeader->width;
							bool hy = y + 1 < bmpHeader->height;
							if (lx) {
								if (ly) { addColor(x - 1, y - 1); }
								          addColor(x - 1, y);
								if (hy) { addColor(x - 1, y + 1); }
							}
							if (ly) { addColor(x, y - 1); }
							if (hy) { addColor(x, y + 1); }
							if (hx) {
								if (ly) { addColor(x + 1, y - 1); }
								          addColor(x + 1, y);
								if (hy) { addColor(x + 1, y + 1); }
							}
							if (count) {
								r /= count;
								g /= count;
								b /= count;
							}
						}
						break;
				}
				pixel = (r << dstRedBit) | (g << dstGreenBit) | (b << dstBlueBit) | (a << dstAlphaBit);
				fprintf(headerFile, "0x%08X, ", pixel);
			}
			fprintf(headerFile, "\n");
		}
	} else {
		for (u32 y = 0; y < bmpHeader->height; ++y) {
			for (u32 x = 0; x < bmpHeader->width; ++x) {
				u32 pixel = bmpPixels[y*bmpHeader->width + x];
				fprintf(headerFile, "0x%08X, ", pixel);
			}
			fprintf(headerFile, "\n");
		}
	}

}