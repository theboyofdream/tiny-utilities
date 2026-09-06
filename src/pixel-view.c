#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include "common/tiny_gui.h"
#include "common/tiny_dpi.h"

#define ID_FILE_OPEN          1001
#define ID_FILE_CLOSE         1002
#define ID_BG_PIXELATED       1010
#define ID_BG_LINEAR_GRADIENT 1011
#define ID_BG_SOLID           1012
#define ID_BG_TRANSPARENT     1013

#define ID_SCALE_1            1030
#define ID_SCALE_2            1031
#define ID_SCALE_3            1032
#define ID_SCALE_4            1033
#define ID_SCALE_8            1034

#define TIMER_GIF_ANIM        1

typedef enum {
    BG_PIXELATED_GRADIENT = 0,
    BG_LINEAR_GRADIENT,
    BG_SOLID,
    BG_TRANSPARENT
} BgMode;

typedef enum {
    GRAD_VERTICAL = 0,
    GRAD_HORIZONTAL,
    GRAD_DIAGONAL
} GradDir;

typedef struct {
    wchar_t filePath[MAX_PATH];
    BgMode bgMode;
    COLORREF bgSolidColor;
    COLORREF bgStartColor;
    COLORREF bgEndColor;
    GradDir gradDir;
    int pixelBlockSize;
    int initWidth;
    int initHeight;
    bool hasRatio;
    int pixelScale; // Pixel block size in screen pixels for game pixelation (default 3)
    int fixedGridW; // Optional fixed grid width
    int fixedGridH; // Optional fixed grid height
} AppConfig;

typedef struct {
    uint32_t* pixels; // BGRA canvas format
    UINT delayMs;
} ImageFrame;

typedef struct {
    ImageFrame* frames;
    UINT frameCount;
    UINT width;
    UINT height;
} ImageAnim;

static AppConfig g_config = {
    .filePath = L"",
    .bgMode = BG_PIXELATED_GRADIENT,
    .bgSolidColor = RGB(30, 30, 36),
    .bgStartColor = RGB(26, 26, 46),
    .bgEndColor = RGB(22, 33, 62),
    .gradDir = GRAD_VERTICAL,
    .pixelBlockSize = 16,
    .initWidth = 400,
    .initHeight = 400,
    .hasRatio = false,
    .pixelScale = 2,
    .fixedGridW = 0,
    .fixedGridH = 0
};

static ImageAnim g_anim = { 0 };
static UINT g_currentFrame = 0;
static HWND g_hWnd = NULL;
static IWICImagingFactory* g_pWicFactory = NULL;
static HANDLE g_hMutex = NULL;
static HANDLE g_hEvent = NULL;

// Helper color utilities
static inline uint32_t LerpColor(COLORREF c1, COLORREF c2, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint8_t r1 = GetRValue(c1), g1 = GetGValue(c1), b1 = GetBValue(c1);
    uint8_t r2 = GetRValue(c2), g2 = GetGValue(c2), b2 = GetBValue(c2);
    uint8_t r = (uint8_t)(r1 + t * (r2 - r1));
    uint8_t g = (uint8_t)(g1 + t * (g2 - g1));
    uint8_t b = (uint8_t)(b1 + t * (b2 - b1));
    return RGB(r, g, b);
}

static void FreeAnim(ImageAnim* anim) {
    if (anim->frames) {
        for (UINT i = 0; i < anim->frameCount; i++) {
            if (anim->frames[i].pixels) {
                HeapFree(GetProcessHeap(), 0, anim->frames[i].pixels);
            }
        }
        HeapFree(GetProcessHeap(), 0, anim->frames);
        anim->frames = NULL;
    }
    anim->frameCount = 0;
    anim->width = 0;
    anim->height = 0;
}

static BOOL InitWIC(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        return FALSE;
    }

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&g_pWicFactory);
    if (SUCCEEDED(hr)) return TRUE;

    hr = CoCreateInstance(&CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&g_pWicFactory);
    if (SUCCEEDED(hr)) return TRUE;

    hr = CoCreateInstance(&CLSID_WICImagingFactory1, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&g_pWicFactory);
    if (SUCCEEDED(hr)) return TRUE;

    return FALSE;
}

static BOOL LoadImageWithWIC(const wchar_t* filename, ImageAnim* anim) {
    if (!g_pWicFactory) return FALSE;

    IWICBitmapDecoder* pDecoder = NULL;
    HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(
        g_pWicFactory, filename, NULL, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) return FALSE;

    UINT frameCount = 0;
    IWICBitmapDecoder_GetFrameCount(pDecoder, &frameCount);
    if (frameCount == 0) {
        IWICBitmapDecoder_Release(pDecoder);
        return FALSE;
    }

    // Determine canvas dimensions
    UINT canvasW = 0, canvasH = 0;
    IWICMetadataQueryReader* pMetaReader = NULL;
    if (SUCCEEDED(IWICBitmapDecoder_GetMetadataQueryReader(pDecoder, &pMetaReader))) {
        PROPVARIANT varW, varH;
        PropVariantInit(&varW);
        PropVariantInit(&varH);
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(pMetaReader, L"/logscrdesc/Width", &varW)) &&
            varW.vt == VT_UI2) {
            canvasW = varW.uiVal;
        }
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(pMetaReader, L"/logscrdesc/Height", &varH)) &&
            varH.vt == VT_UI2) {
            canvasH = varH.uiVal;
        }
        PropVariantClear(&varW);
        PropVariantClear(&varH);
        IWICMetadataQueryReader_Release(pMetaReader);
    }

    if (canvasW == 0 || canvasH == 0) {
        IWICBitmapFrameDecode* pFrame0 = NULL;
        if (SUCCEEDED(IWICBitmapDecoder_GetFrame(pDecoder, 0, &pFrame0))) {
            IWICBitmapFrameDecode_GetSize(pFrame0, &canvasW, &canvasH);
            IWICBitmapFrameDecode_Release(pFrame0);
        }
    }

    if (canvasW == 0 || canvasH == 0) {
        IWICBitmapDecoder_Release(pDecoder);
        return FALSE;
    }

    ImageFrame* frames = (ImageFrame*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ImageFrame) * frameCount);
    if (!frames) {
        IWICBitmapDecoder_Release(pDecoder);
        return FALSE;
    }

    uint32_t* compositeCanvas = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, canvasW * canvasH * sizeof(uint32_t));
    if (!compositeCanvas) {
        HeapFree(GetProcessHeap(), 0, frames);
        IWICBitmapDecoder_Release(pDecoder);
        return FALSE;
    }

    uint8_t prevDisposal = 0;
    UINT prevLeft = 0, prevTop = 0, prevW = 0, prevH = 0;
    uint32_t* backupCanvas = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, canvasW * canvasH * sizeof(uint32_t));

    for (UINT i = 0; i < frameCount; i++) {
        IWICBitmapFrameDecode* pFrame = NULL;
        if (FAILED(IWICBitmapDecoder_GetFrame(pDecoder, i, &pFrame))) break;

        UINT fw = 0, fh = 0;
        IWICBitmapFrameDecode_GetSize(pFrame, &fw, &fh);

        UINT frameLeft = 0, frameTop = 0;
        UINT delayMs = 100;
        uint8_t disposal = 0;

        IWICMetadataQueryReader* pFrameMeta = NULL;
        if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(pFrame, &pFrameMeta))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(pFrameMeta, L"/grctlext/Delay", &var))) {
                if (var.vt == VT_UI2) delayMs = var.uiVal * 10;
                PropVariantClear(&var);
            }
            if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(pFrameMeta, L"/grctlext/Disposal", &var))) {
                if (var.vt == VT_UI1) disposal = var.bVal;
                PropVariantClear(&var);
            }
            if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(pFrameMeta, L"/imgdesc/Left", &var))) {
                if (var.vt == VT_UI2) frameLeft = var.uiVal;
                PropVariantClear(&var);
            }
            if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(pFrameMeta, L"/imgdesc/Top", &var))) {
                if (var.vt == VT_UI2) frameTop = var.uiVal;
                PropVariantClear(&var);
            }
            IWICMetadataQueryReader_Release(pFrameMeta);
        }

        if (delayMs < 20) delayMs = 100;

        // Handle disposal method of previous frame
        if (i > 0) {
            if (prevDisposal == 2) { // Restore to background
                for (UINT y = 0; y < prevH && (prevTop + y) < canvasH; y++) {
                    for (UINT x = 0; x < prevW && (prevLeft + x) < canvasW; x++) {
                        compositeCanvas[(prevTop + y) * canvasW + (prevLeft + x)] = 0;
                    }
                }
            } else if (prevDisposal == 3 && backupCanvas) { // Restore to previous
                CopyMemory(compositeCanvas, backupCanvas, canvasW * canvasH * sizeof(uint32_t));
            }
        }

        if (disposal == 3 && backupCanvas) {
            CopyMemory(backupCanvas, compositeCanvas, canvasW * canvasH * sizeof(uint32_t));
        }

        // Convert frame pixels to BGRA 32bpp
        IWICFormatConverter* pConverter = NULL;
        if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_pWicFactory, &pConverter))) {
            if (SUCCEEDED(IWICFormatConverter_Initialize(
                    pConverter, (IWICBitmapSource*)pFrame,
                    &GUID_WICPixelFormat32bppBGRA,
                    WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) {
                
                uint32_t* frameBuf = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, fw * fh * sizeof(uint32_t));
                if (frameBuf) {
                    if (SUCCEEDED(IWICFormatConverter_CopyPixels(pConverter, NULL, fw * sizeof(uint32_t), fw * fh * sizeof(uint32_t), (BYTE*)frameBuf))) {
                        // Composite current frame over compositeCanvas
                        for (UINT y = 0; y < fh && (frameTop + y) < canvasH; y++) {
                            for (UINT x = 0; x < fw && (frameLeft + x) < canvasW; x++) {
                                uint32_t srcPixel = frameBuf[y * fw + x];
                                uint8_t sa = (srcPixel >> 24) & 0xFF;
                                uint32_t dstIdx = (frameTop + y) * canvasW + (frameLeft + x);
                                if (sa == 255) {
                                    compositeCanvas[dstIdx] = srcPixel;
                                } else if (sa > 0) {
                                    uint32_t dstPixel = compositeCanvas[dstIdx];
                                    uint8_t da = (dstPixel >> 24) & 0xFF;
                                    uint8_t dr = (dstPixel >> 16) & 0xFF;
                                    uint8_t dg = (dstPixel >> 8) & 0xFF;
                                    uint8_t db = dstPixel & 0xFF;

                                    uint8_t sr = (srcPixel >> 16) & 0xFF;
                                    uint8_t sg = (srcPixel >> 8) & 0xFF;
                                    uint8_t sb = srcPixel & 0xFF;

                                    uint8_t outR = (sr * sa + dr * (255 - sa)) / 255;
                                    uint8_t outG = (sg * sa + dg * (255 - sa)) / 255;
                                    uint8_t outB = (sb * sa + db * (255 - sa)) / 255;
                                    uint8_t outA = sa + da * (255 - sa) / 255;

                                    compositeCanvas[dstIdx] = (outA << 24) | (outR << 16) | (outG << 8) | outB;
                                }
                            }
                        }
                    }
                    HeapFree(GetProcessHeap(), 0, frameBuf);
                }
            }
            IWICFormatConverter_Release(pConverter);
        }

        frames[i].delayMs = delayMs;
        frames[i].pixels = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, canvasW * canvasH * sizeof(uint32_t));
        if (frames[i].pixels) {
            CopyMemory(frames[i].pixels, compositeCanvas, canvasW * canvasH * sizeof(uint32_t));
        }

        prevDisposal = disposal;
        prevLeft = frameLeft;
        prevTop = frameTop;
        prevW = fw;
        prevH = fh;

        IWICBitmapFrameDecode_Release(pFrame);
    }

    if (backupCanvas) HeapFree(GetProcessHeap(), 0, backupCanvas);
    HeapFree(GetProcessHeap(), 0, compositeCanvas);
    IWICBitmapDecoder_Release(pDecoder);

    FreeAnim(anim);
    anim->frames = frames;
    anim->frameCount = frameCount;
    anim->width = canvasW;
    anim->height = canvasH;

    return TRUE;
}

static void RenderFrameToWindow(HWND hWnd) {
    if (g_anim.frameCount == 0 || !g_anim.frames) return;

    RECT rc;
    GetClientRect(hWnd, &rc);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    if (winW <= 0 || winH <= 0) return;

    int pixelSize = 1;
    int gridW = winW;
    int gridH = winH;

    if (g_config.fixedGridW > 0 && g_config.fixedGridH > 0) {
        gridW = g_config.fixedGridW;
        gridH = g_config.fixedGridH;
        pixelSize = winW / gridW;
        if (pixelSize < 1) pixelSize = 1;
    } else {
        pixelSize = g_config.pixelScale;
        if (pixelSize < 1) pixelSize = 1;

        gridW = winW / pixelSize;
        gridH = winH / pixelSize;
        if (gridW < 1) gridW = 1;
        if (gridH < 1) gridH = 1;
    }

    uint32_t* gridBuf = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, gridW * gridH * sizeof(uint32_t));
    if (!gridBuf) return;

    // 1. Render Background into low-res pixel grid (gridW x gridH)
    if (g_config.bgMode == BG_TRANSPARENT) {
        ZeroMemory(gridBuf, gridW * gridH * sizeof(uint32_t));
    } else if (g_config.bgMode == BG_SOLID) {
        uint8_t sr = GetRValue(g_config.bgSolidColor);
        uint8_t sg = GetGValue(g_config.bgSolidColor);
        uint8_t sb = GetBValue(g_config.bgSolidColor);
        uint32_t solidPixel = 0xFF000000 | (sr << 16) | (sg << 8) | sb;
        for (int i = 0; i < gridW * gridH; i++) {
            gridBuf[i] = solidPixel;
        }
    } else if (g_config.bgMode == BG_LINEAR_GRADIENT) {
        for (int y = 0; y < gridH; y++) {
            float t = (gridH > 1) ? ((float)y / (float)(gridH - 1)) : 0.0f;
            COLORREF col = LerpColor(g_config.bgStartColor, g_config.bgEndColor, t);
            uint8_t r = GetRValue(col), g = GetGValue(col), b = GetBValue(col);
            uint32_t gradPixel = 0xFF000000 | (r << 16) | (g << 8) | b;
            for (int x = 0; x < gridW; x++) {
                gridBuf[y * gridW + x] = gradPixel;
            }
        }
    } else if (g_config.bgMode == BG_PIXELATED_GRADIENT) {
        int blockSize = g_config.pixelBlockSize;
        if (blockSize < 2) blockSize = 16;
        int blocksH = (gridH + blockSize - 1) / blockSize;
        int blocksW = (gridW + blockSize - 1) / blockSize;

        for (int gy = 0; gy < blocksH; gy++) {
            float t = (blocksH > 1) ? ((float)gy / (float)(blocksH - 1)) : 0.0f;
            COLORREF col = LerpColor(g_config.bgStartColor, g_config.bgEndColor, t);
            uint8_t r = GetRValue(col), g = GetGValue(col), b = GetBValue(col);
            uint32_t blockPixel = 0xFF000000 | (r << 16) | (g << 8) | b;

            int startY = gy * blockSize;
            int endY = (startY + blockSize < gridH) ? (startY + blockSize) : gridH;

            for (int gx = 0; gx < blocksW; gx++) {
                int startX = gx * blockSize;
                int endX = (startX + blockSize < gridW) ? (startX + blockSize) : gridW;

                for (int y = startY; y < endY; y++) {
                    for (int x = startX; x < endX; x++) {
                        gridBuf[y * gridW + x] = blockPixel;
                    }
                }
            }
        }
    }

    // 2. Render Image onto low-res pixel grid (gridW x gridH) preserving exact aspect ratio
    UINT imgW = g_anim.width;
    UINT imgH = g_anim.height;
    if (imgW > 0 && imgH > 0 && g_currentFrame < g_anim.frameCount) {
        uint32_t* srcPixels = g_anim.frames[g_currentFrame].pixels;

        float gridAspect = (float)gridW / (float)gridH;
        float imgAspect = (float)imgW / (float)imgH;

        int dstW, dstH, dstX, dstY;
        if (gridAspect > imgAspect) {
            dstH = gridH;
            dstW = (int)(gridH * imgAspect);
            dstX = (gridW - dstW) / 2;
            dstY = 0;
        } else {
            dstW = gridW;
            dstH = (int)(gridW / imgAspect);
            dstX = 0;
            dstY = (gridH - dstH) / 2;
        }

        if (dstW > 0 && dstH > 0) {
            for (int dy = 0; dy < dstH; dy++) {
                int cy = dstY + dy;
                if (cy < 0 || cy >= gridH) continue;

                UINT sy = (UINT)((dy * imgH) / dstH);
                if (sy >= imgH) sy = imgH - 1;

                for (int dx = 0; dx < dstW; dx++) {
                    int cx = dstX + dx;
                    if (cx < 0 || cx >= gridW) continue;

                    UINT sx = (UINT)((dx * imgW) / dstW);
                    if (sx >= imgW) sx = imgW - 1;

                    uint32_t srcCol = srcPixels[sy * imgW + sx];
                    uint8_t sa = (srcCol >> 24) & 0xFF;
                    if (sa == 0) continue;

                    uint8_t sr = (srcCol >> 16) & 0xFF;
                    uint8_t sg = (srcCol >> 8) & 0xFF;
                    uint8_t sb = srcCol & 0xFF;

                    uint32_t dstIdx = cy * gridW + cx;

                    if (g_config.bgMode == BG_TRANSPARENT) {
                        uint8_t pr = (sr * sa) / 255;
                        uint8_t pg = (sg * sa) / 255;
                        uint8_t pb = (sb * sa) / 255;
                        gridBuf[dstIdx] = (sa << 24) | (pr << 16) | (pg << 8) | pb;
                    } else {
                        if (sa == 255) {
                            gridBuf[dstIdx] = 0xFF000000 | (sr << 16) | (sg << 8) | sb;
                        } else {
                            uint32_t bgPixel = gridBuf[dstIdx];
                            uint8_t bgr = (bgPixel >> 16) & 0xFF;
                            uint8_t bgg = (bgPixel >> 8) & 0xFF;
                            uint8_t bgb = bgPixel & 0xFF;

                            uint8_t outR = (sr * sa + bgr * (255 - sa)) / 255;
                            uint8_t outG = (sg * sa + bgg * (255 - sa)) / 255;
                            uint8_t outB = (sb * sa + bgb * (255 - sa)) / 255;
                            gridBuf[dstIdx] = 0xFF000000 | (outR << 16) | (outG << 8) | outB;
                        }
                    }
                }
            }
        }
    }

    // 3. Upscale low-res gridBuf (gridW x gridH) -> DIB section (winW x winH) via Square Pixel Mapping
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = winW;
    bmi.bmiHeader.biHeight = -winH; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* dibBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void**)&dibBits, NULL, 0);
    if (!dibBits) {
        HeapFree(GetProcessHeap(), 0, gridBuf);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    for (int winY = 0; winY < winH; winY++) {
        int gy = winY / pixelSize;
        if (gy >= gridH) gy = gridH - 1;

        for (int winX = 0; winX < winW; winX++) {
            int gx = winX / pixelSize;
            if (gx >= gridW) gx = gridW - 1;

            dibBits[winY * winW + winX] = gridBuf[gy * gridW + gx];
        }
    }

    RECT wRect;
    GetWindowRect(hWnd, &wRect);
    POINT ptDst = { wRect.left, wRect.top };
    SIZE sz = { winW, winH };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hWnd, hdcScreen, &ptDst, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    HeapFree(GetProcessHeap(), 0, gridBuf);
}

static void OpenFileCmd(HWND hWnd) {
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        KillTimer(hWnd, TIMER_GIF_ANIM);
        if (LoadImageWithWIC(szFile, &g_anim)) {
            wcscpy_s(g_config.filePath, MAX_PATH, szFile);
            g_currentFrame = 0;

            // Set Title
            wchar_t titleBuf[MAX_PATH + 64];
            wchar_t* fname = wcsrchr(szFile, L'\\');
            if (fname) fname++; else fname = szFile;
            wsprintfW(titleBuf, L"Pixel View — %s", fname);
            SetWindowTextW(hWnd, titleBuf);

            // Resize window to ratio/400px initial width preserving aspect ratio
            if (g_anim.width > 0 && g_anim.height > 0) {
                int winW, winH;
                if (g_config.hasRatio) {
                    winW = g_config.initWidth;
                    winH = g_config.initHeight;
                } else {
                    winW = 400;
                    winH = (int)(400.0f * ((float)g_anim.height / (float)g_anim.width));
                    if (winH < 100) winH = 100;
                }
                SetWindowPos(hWnd, NULL, 0, 0, winW, winH, SWP_NOMOVE | SWP_NOZORDER);
            }

            if (g_anim.frameCount > 1) {
                SetTimer(hWnd, TIMER_GIF_ANIM, g_anim.frames[0].delayMs, NULL);
            }
            RenderFrameToWindow(hWnd);
        } else {
            MessageBoxW(hWnd, L"Failed to load selected image file.", L"Error", MB_OK | MB_ICONERROR);
        }
    }
}

static void ShowContextMenu(HWND hWnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    HMENU hSubBg = CreatePopupMenu();
    HMENU hSubScale = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING, ID_FILE_OPEN, L"Open File...\tCtrl+O");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    UINT flagsP = (g_config.bgMode == BG_PIXELATED_GRADIENT) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsL = (g_config.bgMode == BG_LINEAR_GRADIENT) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsS = (g_config.bgMode == BG_SOLID) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsT = (g_config.bgMode == BG_TRANSPARENT) ? MF_CHECKED : MF_UNCHECKED;

    AppendMenuW(hSubBg, MF_STRING | flagsP, ID_BG_PIXELATED, L"Pixelated Gradient");
    AppendMenuW(hSubBg, MF_STRING | flagsL, ID_BG_LINEAR_GRADIENT, L"Linear Gradient");
    AppendMenuW(hSubBg, MF_STRING | flagsS, ID_BG_SOLID, L"Solid Color");
    AppendMenuW(hSubBg, MF_STRING | flagsT, ID_BG_TRANSPARENT, L"Transparent");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSubBg, L"Background");

    UINT flagsS1 = (g_config.pixelScale == 1 && g_config.fixedGridW == 0) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsS2 = (g_config.pixelScale == 2 && g_config.fixedGridW == 0) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsS3 = (g_config.pixelScale == 3 && g_config.fixedGridW == 0) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsS4 = (g_config.pixelScale == 4 && g_config.fixedGridW == 0) ? MF_CHECKED : MF_UNCHECKED;
    UINT flagsS8 = (g_config.pixelScale == 8 && g_config.fixedGridW == 0) ? MF_CHECKED : MF_UNCHECKED;

    AppendMenuW(hSubScale, MF_STRING | flagsS1, ID_SCALE_1, L"1px (Crisp Pixel Art - Max Detail)");
    AppendMenuW(hSubScale, MF_STRING | flagsS2, ID_SCALE_2, L"2px (Retro Game 2x - Default)");
    AppendMenuW(hSubScale, MF_STRING | flagsS3, ID_SCALE_3, L"3px (Retro Game 3x)");
    AppendMenuW(hSubScale, MF_STRING | flagsS4, ID_SCALE_4, L"4px (Chunky Pixel Art 4x)");
    AppendMenuW(hSubScale, MF_STRING | flagsS8, ID_SCALE_8, L"8px (Ultra Chunky 8x)");

    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSubScale, L"Pixel Scale / Detail");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_FILE_CLOSE, L"Close...\tAlt+F4");

    if (pt.x == -1 && pt.y == -1) {
        GetCursorPos(&pt);
    }

    SetForegroundWindow(hWnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hSubScale);
    DestroyMenu(hSubBg);
    DestroyMenu(hMenu);

    if (cmd == ID_FILE_OPEN) {
        OpenFileCmd(hWnd);
    } else if (cmd == ID_FILE_CLOSE) {
        DestroyWindow(hWnd);
    } else if (cmd >= ID_BG_PIXELATED && cmd <= ID_BG_TRANSPARENT) {
        if (cmd == ID_BG_PIXELATED) g_config.bgMode = BG_PIXELATED_GRADIENT;
        else if (cmd == ID_BG_LINEAR_GRADIENT) g_config.bgMode = BG_LINEAR_GRADIENT;
        else if (cmd == ID_BG_SOLID) g_config.bgMode = BG_SOLID;
        else if (cmd == ID_BG_TRANSPARENT) g_config.bgMode = BG_TRANSPARENT;
        RenderFrameToWindow(hWnd);
    } else if (cmd >= ID_SCALE_1 && cmd <= ID_SCALE_8) {
        g_config.fixedGridW = 0;
        g_config.fixedGridH = 0;
        if (cmd == ID_SCALE_1) g_config.pixelScale = 1;
        else if (cmd == ID_SCALE_2) g_config.pixelScale = 2;
        else if (cmd == ID_SCALE_3) g_config.pixelScale = 3;
        else if (cmd == ID_SCALE_4) g_config.pixelScale = 4;
        else if (cmd == ID_SCALE_8) g_config.pixelScale = 8;
        RenderFrameToWindow(hWnd);
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hWnd = hWnd;
        return 0;

    case WM_NCHITTEST: {
        LRESULT hit = TinyGUI_HitTestResizeBorders(hWnd, lParam, 8);
        if (hit == HTCLIENT) return HTCAPTION; // Left-click + drag moves frameless window
        return hit;
    }

    case WM_SIZE:
        RenderFrameToWindow(hWnd);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_GIF_ANIM && g_anim.frameCount > 1) {
            g_currentFrame = (g_currentFrame + 1) % g_anim.frameCount;
            SetTimer(hWnd, TIMER_GIF_ANIM, g_anim.frames[g_currentFrame].delayMs, NULL);
            RenderFrameToWindow(hWnd);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            OpenFileCmd(hWnd);
            return 0;
        }
        return 0;

    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ShowContextMenu(hWnd, pt);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_GIF_ANIM);
        FreeAnim(&g_anim);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ParseArgs(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--file") == 0 && i + 1 < argc) {
            wcscpy_s(g_config.filePath, MAX_PATH, argv[i + 1]);
            i++;
        } else if (wcscmp(argv[i], L"--bg") == 0 && i + 1 < argc) {
            if (wcscmp(argv[i + 1], L"transparent") == 0) g_config.bgMode = BG_TRANSPARENT;
            else if (wcscmp(argv[i + 1], L"solid") == 0) g_config.bgMode = BG_SOLID;
            else if (wcscmp(argv[i + 1], L"gradient") == 0) g_config.bgMode = BG_LINEAR_GRADIENT;
            else if (wcscmp(argv[i + 1], L"pixelated") == 0) g_config.bgMode = BG_PIXELATED_GRADIENT;
            i++;
        } else if (wcscmp(argv[i], L"--pixel-size") == 0 && i + 1 < argc) {
            int sz = _wtoi(argv[i + 1]);
            if (sz >= 1 && sz <= 64) {
                g_config.pixelScale = sz;
                g_config.fixedGridW = 0;
                g_config.fixedGridH = 0;
            }
            i++;
        } else if (wcscmp(argv[i], L"--ratio") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (swscanf_s(argv[i + 1], L"%dx%d", &w, &h) == 2 || swscanf_s(argv[i + 1], L"%dX%d", &w, &h) == 2) {
                if (w >= 50 && h >= 50) {
                    g_config.initWidth = w;
                    g_config.initHeight = h;
                    g_config.hasRatio = true;
                }
            } else if (swscanf_s(argv[i + 1], L"%d", &w) == 1 && w >= 50) {
                g_config.initWidth = w;
                g_config.initHeight = w;
                g_config.hasRatio = true;
            }
            i++;
        } else if (wcscmp(argv[i], L"--grid") == 0 && i + 1 < argc) {
            int gw = 0, gh = 0;
            if (swscanf_s(argv[i + 1], L"%dx%d", &gw, &gh) == 2 || swscanf_s(argv[i + 1], L"%dX%d", &gw, &gh) == 2) {
                if (gw >= 16 && gh >= 16 && gw <= 1024 && gh <= 1024) {
                    g_config.fixedGridW = gw;
                    g_config.fixedGridH = gh;
                }
            } else if (swscanf_s(argv[i + 1], L"%d", &gw) == 1 && gw >= 16 && gw <= 1024) {
                g_config.fixedGridW = gw;
                g_config.fixedGridH = gw;
            }
            i++;
        } else if (argv[i][0] != L'-' && g_config.filePath[0] == L'\0') {
            wcscpy_s(g_config.filePath, MAX_PATH, argv[i]);
        }
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // IPC Single-Instance Toggle check
    g_hMutex = CreateMutexW(NULL, FALSE, L"Global\\TinyPixelViewMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HANDLE hEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Global\\TinyPixelViewEvent");
        if (hEv) {
            SetEvent(hEv);
            CloseHandle(hEv);
        }
        if (g_hMutex) CloseHandle(g_hMutex);
        return 0;
    }
    g_hEvent = CreateEventW(NULL, FALSE, FALSE, L"Global\\TinyPixelViewEvent");

    TinyDPI_EnablePerMonitorAwareness();

    if (!InitWIC()) {
        MessageBoxW(NULL, L"Failed to initialize Windows Imaging Component (WIC).", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        ParseArgs(argc, argv);
        LocalFree(argv);
    }

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PixelViewWindowClass";
    RegisterClassExW(&wc);

    int winW = g_config.initWidth;
    int winH = g_config.initHeight;
    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"Pixel View",
        WS_POPUP | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) return 1;

    // Load initial image if specified, otherwise prompt user
    if (g_config.filePath[0] != L'\0') {
        if (LoadImageWithWIC(g_config.filePath, &g_anim)) {
            wchar_t titleBuf[MAX_PATH + 64];
            wchar_t* fname = wcsrchr(g_config.filePath, L'\\');
            if (fname) fname++; else fname = g_config.filePath;
            wsprintfW(titleBuf, L"Pixel View — %s", fname);
            SetWindowTextW(hWnd, titleBuf);

            if (g_anim.width > 0 && g_anim.height > 0) {
                if (g_config.hasRatio) {
                    winW = g_config.initWidth;
                    winH = g_config.initHeight;
                } else {
                    winW = 400;
                    winH = (int)(400.0f * ((float)g_anim.height / (float)g_anim.width));
                    if (winH < 100) winH = 100;
                }
            }
        }
    }

    // Center window on screen
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;
    SetWindowPos(hWnd, NULL, posX, posY, winW, winH, SWP_NOZORDER);

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    if (g_anim.frameCount == 0) {
        OpenFileCmd(hWnd);
        if (g_anim.frameCount == 0) {
            // User cancelled initial file selection
            DestroyWindow(hWnd);
            return 0;
        }
    } else {
        if (g_anim.frameCount > 1) {
            SetTimer(hWnd, TIMER_GIF_ANIM, g_anim.frames[0].delayMs, NULL);
        }
        RenderFrameToWindow(hWnd);
    }

    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) break;

        // Check for toggle event from second instance
        if (g_hEvent && WaitForSingleObject(g_hEvent, 0) == WAIT_OBJECT_0) {
            DestroyWindow(hWnd);
            break;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_pWicFactory) IWICImagingFactory_Release(g_pWicFactory);
    CoUninitialize();

    if (g_hEvent) CloseHandle(g_hEvent);
    if (g_hMutex) CloseHandle(g_hMutex);

    return 0;
}
