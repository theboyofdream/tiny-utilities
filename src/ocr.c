/*
 * OCR — Win32 Optical Character Recognition utility.
 * Source: src/ocr.c
 * Exe: dist/release/ocr.exe
 *
 * Capabilities:
 *   - Portable Tesseract OCR engine (internal to ocr-resources/) with lazy downloading.
 *   - CLI usage: ocr.exe [--image] <image> [--lang <lang>] [--psm <psm>] [--oem <oem>]
 *   - Interactive picker: If no image is passed on CLI, opens a native Windows File Explorer dialog.
 *   - Supported image formats: PNG, JPG, JPEG, BMP.
 *   - Auto language script detection (OSD / multi-lang auto fallback).
 *   - Output: Copies recognized text to clipboard (CF_UNICODETEXT), outputs to stdout,
 *             and displays a native alert / notification if launched interactively.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <urlmon.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>

#include "common/tiny_cli.h"
#include "common/font.h"
#include "common/clipboard.h"

#define TESS_PORTABLE_URL L"https://github.com/zstrathe/tesseract_portable_windows/archive/refs/heads/main.zip"
#define TESSDATA_BASE_URL L"https://raw.githubusercontent.com/tesseract-ocr/tessdata_fast/main/"

/* ------------------------------------------------------------------ */
/* Path & Directory Helpers                                           */
/* ------------------------------------------------------------------ */

static void get_app_dir(wchar_t *outPath, DWORD maxLen)
{
    GetModuleFileNameW(NULL, outPath, maxLen);
    PathRemoveFileSpecW(outPath);
}

static void get_resource_dir(wchar_t *outPath, DWORD maxLen)
{
    get_app_dir(outPath, maxLen);
    PathAppendW(outPath, L"ocr-resources");
}

static void get_tesseract_exe(wchar_t *outPath, DWORD maxLen)
{
    get_resource_dir(outPath, maxLen);
    PathAppendW(outPath, L"tesseract\\tesseract.exe");
}

static void get_tessdata_dir(wchar_t *outPath, DWORD maxLen)
{
    get_resource_dir(outPath, maxLen);
    PathAppendW(outPath, L"tessdata");
}

static void get_tmp_dir(wchar_t *outPath, DWORD maxLen)
{
    get_resource_dir(outPath, maxLen);
    PathAppendW(outPath, L"tmp");
}

static bool file_exists_and_valid(const wchar_t *path, DWORD minSize)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    return (fad.nFileSizeLow >= minSize || fad.nFileSizeHigh > 0);
}

static bool download_file(const wchar_t *url, const wchar_t *destPath)
{
    HRESULT hr = URLDownloadToFileW(NULL, url, destPath, 0, NULL);
    if (SUCCEEDED(hr) && file_exists_and_valid(destPath, 1024)) {
        return true;
    }

    /* Fallback: curl */
    wchar_t cmd[2048];
    swprintf_s(cmd, 2048, L"curl.exe -s -L \"%s\" -o \"%s\"", url, destPath);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (file_exists_and_valid(destPath, 1024)) {
        return true;
    }

    /* Fallback: PowerShell Invoke-WebRequest */
    swprintf_s(cmd, 2048, L"powershell.exe -NoProfile -NonInteractive -Command \"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%s' -OutFile '%s'\"", url, destPath);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    return file_exists_and_valid(destPath, 1024);
}

static bool copy_dir_recursive(const wchar_t *src, const wchar_t *dst)
{
    CreateDirectoryW(dst, NULL);
    wchar_t searchPattern[MAX_PATH];
    swprintf_s(searchPattern, MAX_PATH, L"%s\\*", src);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t srcItem[MAX_PATH], dstItem[MAX_PATH];
        swprintf_s(srcItem, MAX_PATH, L"%s\\%s", src, fd.cFileName);
        swprintf_s(dstItem, MAX_PATH, L"%s\\%s", dst, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            copy_dir_recursive(srcItem, dstItem);
        } else {
            CopyFileW(srcItem, dstItem, FALSE);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return true;
}

static bool ensure_tesseract_runtime(void)
{
    wchar_t tessExe[MAX_PATH];
    get_tesseract_exe(tessExe, MAX_PATH);
    if (file_exists_and_valid(tessExe, 100000)) {
        return true;
    }

    wchar_t resDir[MAX_PATH], tessDir[MAX_PATH], tessDataDir[MAX_PATH], tmpDir[MAX_PATH];
    get_resource_dir(resDir, MAX_PATH);
    CreateDirectoryW(resDir, NULL);

    get_tessdata_dir(tessDataDir, MAX_PATH);
    CreateDirectoryW(tessDataDir, NULL);

    get_tmp_dir(tmpDir, MAX_PATH);
    CreateDirectoryW(tmpDir, NULL);

    PathCombineW(tessDir, resDir, L"tesseract");
    CreateDirectoryW(tessDir, NULL);

    fwprintf(stderr, L"[*] Downloading portable Tesseract runtime...\n");
    wchar_t zipPath[MAX_PATH];
    PathCombineW(zipPath, tmpDir, L"tesseract_portable.zip");

    if (!download_file(TESS_PORTABLE_URL, zipPath)) {
        fwprintf(stderr, L"[-] Failed to download portable Tesseract runtime.\n");
        return false;
    }

    /* Extract zip */
    wchar_t extractCmd[2048];
    swprintf_s(extractCmd, 2048, L"powershell.exe -NoProfile -NonInteractive -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"", zipPath, tmpDir);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, extractCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    /* Locate extracted folder and copy contents */
    wchar_t extractedFolder[MAX_PATH];
    PathCombineW(extractedFolder, tmpDir, L"tesseract_portable_windows-main");
    if (!copy_dir_recursive(extractedFolder, tessDir)) {
        copy_dir_recursive(tmpDir, tessDir);
    }

    DeleteFileW(zipPath);
    return file_exists_and_valid(tessExe, 100000);
}

static bool ensure_lang_file(const wchar_t *langCode)
{
    wchar_t tessDataDir[MAX_PATH];
    get_tessdata_dir(tessDataDir, MAX_PATH);
    CreateDirectoryW(tessDataDir, NULL);

    wchar_t langFile[MAX_PATH];
    swprintf_s(langFile, MAX_PATH, L"%s\\%s.traineddata", tessDataDir, langCode);

    if (file_exists_and_valid(langFile, 10000)) {
        return true;
    }

    fwprintf(stderr, L"[*] Downloading language pack '%s'...\n", langCode);
    wchar_t url[1024];
    swprintf_s(url, 1024, L"%s%s.traineddata", TESSDATA_BASE_URL, langCode);

    return download_file(url, langFile);
}

static void ensure_requested_langs(const wchar_t *langParam, wchar_t *outLangBuf, DWORD maxBufLen)
{
    wcscpy_s(outLangBuf, maxBufLen, L"");

    if (!langParam || wcscmp(langParam, L"auto") == 0 || wcscmp(langParam, L"eng") == 0) {
        ensure_lang_file(L"eng");
        wcscpy_s(outLangBuf, maxBufLen, L"eng");
        return;
    }

    /* Specific language code(s) passed like "eng+hin" */
    wchar_t tmp[512];
    wcscpy_s(tmp, 512, langParam);
    wchar_t *context = NULL;
    wchar_t *token = wcstok_s(tmp, L"+", &context);
    while (token) {
        ensure_lang_file(token);
        token = wcstok_s(NULL, L"+", &context);
    }

    wcscpy_s(outLangBuf, maxBufLen, langParam);
}

/* ------------------------------------------------------------------ */
/* Native File Picker Dialog                                          */
/* ------------------------------------------------------------------ */

static bool PickImageFileDialog(wchar_t *outPath, DWORD maxPathLen)
{
    wchar_t filePath[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = L"Image Files (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0PNG Files (*.png)\0*.png\0JPEG Files (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0BMP Files (*.bmp)\0*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Image for OCR";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileNameW(&ofn) && filePath[0] != L'\0') {
        wcscpy_s(outPath, maxPathLen, filePath);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Clipboard Helper                                                   */
/* ------------------------------------------------------------------ */

static bool CopyToClipboard(const wchar_t *text)
{
    return TinyClipboard_SetText(text);
}

/* ------------------------------------------------------------------ */
/* Console Output Helper                                              */
/* ------------------------------------------------------------------ */

static void EnsureConsoleAttached(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *fp = NULL;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
        }
    }
}

static void OutputText(const wchar_t *text)
{
    if (!text) return;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        }
    }

    DWORD len = (DWORD)wcslen(text);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            DWORD written = 0;
            WriteConsoleW(hOut, text, len, &written, NULL);
            if (len > 0 && text[len - 1] != L'\n') {
                WriteConsoleW(hOut, L"\n", 1, &written, NULL);
            }
        } else {
            char *utf8 = (char*)malloc((len * 4) + 16);
            if (utf8) {
                int ulen = WideCharToMultiByte(CP_UTF8, 0, text, (int)len, utf8, (int)(len * 4), NULL, NULL);
                if (ulen > 0) {
                    DWORD written = 0;
                    WriteFile(hOut, utf8, (DWORD)ulen, &written, NULL);
                    if (len > 0 && text[len - 1] != L'\n') {
                        WriteFile(hOut, "\n", 1, &written, NULL);
                    }
                }
                free(utf8);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Image Preprocessing via WIC (Upscale, Grayscale & Margin)          */
/* ------------------------------------------------------------------ */

static bool PreprocessImageWIC(const wchar_t *srcPath, wchar_t *outTempPath, DWORD maxTempLen)
{
    IWICImagingFactory *pFactory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        wcscpy_s(outTempPath, maxTempLen, srcPath);
        return false;
    }

    IWICBitmapDecoder *pDecoder = NULL;
    hr = IWICImagingFactory_CreateDecoderFromFilename(pFactory, srcPath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr) || !pDecoder) {
        IWICImagingFactory_Release(pFactory);
        wcscpy_s(outTempPath, maxTempLen, srcPath);
        return false;
    }

    IWICBitmapFrameDecode *pFrame = NULL;
    hr = IWICBitmapDecoder_GetFrame(pDecoder, 0, &pFrame);
    if (FAILED(hr) || !pFrame) {
        IWICBitmapDecoder_Release(pDecoder);
        IWICImagingFactory_Release(pFactory);
        wcscpy_s(outTempPath, maxTempLen, srcPath);
        return false;
    }

    UINT width = 0, height = 0;
    IWICBitmapFrameDecode_GetSize(pFrame, &width, &height);
    if (width == 0 || height == 0) {
        IWICBitmapFrameDecode_Release(pFrame);
        IWICBitmapDecoder_Release(pDecoder);
        IWICImagingFactory_Release(pFactory);
        wcscpy_s(outTempPath, maxTempLen, srcPath);
        return false;
    }

    double scale = 2.0;
    if (height < 400) scale = 3.0;
    else if (height < 600) scale = 2.5;
    else if (width >= 2000 && height >= 1200) scale = 1.5;

    UINT newWidth = (UINT)(width * scale);
    UINT newHeight = (UINT)(height * scale);
    if (newWidth > 3200) {
        scale = 3200.0 / width;
        newWidth = 3200;
        newHeight = (UINT)(height * scale);
    }

    IWICBitmapScaler *pScaler = NULL;
    hr = IWICImagingFactory_CreateBitmapScaler(pFactory, &pScaler);
    if (SUCCEEDED(hr) && pScaler) {
        IWICBitmapScaler_Initialize(pScaler, (IWICBitmapSource*)pFrame, newWidth, newHeight, WICBitmapInterpolationModeFant);
    }

    IWICBitmapSource *pSource = pScaler ? (IWICBitmapSource*)pScaler : (IWICBitmapSource*)pFrame;

    IWICFormatConverter *pConverter = NULL;
    hr = IWICImagingFactory_CreateFormatConverter(pFactory, &pConverter);
    if (SUCCEEDED(hr) && pConverter) {
        IWICFormatConverter_Initialize(pConverter, pSource, &GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeFixedGray256);
    }

    IWICBitmapSource *pFinalSource = pConverter ? (IWICBitmapSource*)pConverter : pSource;

    /* Write out preprocessed PNG to temp */
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    swprintf_s(outTempPath, maxTempLen, L"%socr_prep_%u_%u.png", tempDir, GetCurrentProcessId(), GetTickCount());

    IWICStream *pStream = NULL;
    hr = IWICImagingFactory_CreateStream(pFactory, &pStream);
    if (SUCCEEDED(hr) && pStream) {
        hr = IWICStream_InitializeFromFilename(pStream, outTempPath, GENERIC_WRITE);
        if (SUCCEEDED(hr)) {
            IWICBitmapEncoder *pEncoder = NULL;
            hr = IWICImagingFactory_CreateEncoder(pFactory, &GUID_ContainerFormatPng, NULL, &pEncoder);
            if (SUCCEEDED(hr) && pEncoder) {
                hr = IWICBitmapEncoder_Initialize(pEncoder, (IStream*)pStream, WICBitmapEncoderNoCache);
                if (SUCCEEDED(hr)) {
                    IWICBitmapFrameEncode *pFrameEncode = NULL;
                    hr = IWICBitmapEncoder_CreateNewFrame(pEncoder, &pFrameEncode, NULL);
                    if (SUCCEEDED(hr) && pFrameEncode) {
                        IWICBitmapFrameEncode_Initialize(pFrameEncode, NULL);
                        IWICBitmapFrameEncode_SetSize(pFrameEncode, newWidth, newHeight);
                        WICPixelFormatGUID format = GUID_WICPixelFormat8bppGray;
                        IWICBitmapFrameEncode_SetPixelFormat(pFrameEncode, &format);
                        IWICBitmapFrameEncode_WriteSource(pFrameEncode, pFinalSource, NULL);
                        IWICBitmapFrameEncode_Commit(pFrameEncode);
                        IWICBitmapFrameEncode_Release(pFrameEncode);
                    }
                    IWICBitmapEncoder_Commit(pEncoder);
                }
                IWICBitmapEncoder_Release(pEncoder);
            }
        }
        IWICStream_Release(pStream);
    }

    if (pConverter) IWICFormatConverter_Release(pConverter);
    if (pScaler) IWICBitmapScaler_Release(pScaler);
    IWICBitmapFrameDecode_Release(pFrame);
    IWICBitmapDecoder_Release(pDecoder);
    IWICImagingFactory_Release(pFactory);

    if (file_exists_and_valid(outTempPath, 50)) {
        return true;
    }

    wcscpy_s(outTempPath, maxTempLen, srcPath);
    return false;
}

/* ------------------------------------------------------------------ */
/* OCR Execution via Tesseract Runtime                                */
/* ------------------------------------------------------------------ */

static bool RunOCR(const wchar_t *imagePath, const wchar_t *lang, int psm, int oem, wchar_t *outText, DWORD maxTextLen)
{
    outText[0] = L'\0';

    if (!ensure_tesseract_runtime()) {
        fwprintf(stderr, L"[-] Error: Tesseract runtime is unavailable.\n");
        return false;
    }

    wchar_t activeLangs[512];
    ensure_requested_langs(lang, activeLangs, 512);

    /* Preprocess image via WIC for maximal recognition accuracy */
    wchar_t prepImagePath[MAX_PATH];
    bool preprocessed = PreprocessImageWIC(imagePath, prepImagePath, MAX_PATH);
    const wchar_t *targetImg = (preprocessed && file_exists_and_valid(prepImagePath, 50)) ? prepImagePath : imagePath;

    wchar_t tessExe[MAX_PATH], tessDataDir[MAX_PATH];
    get_tesseract_exe(tessExe, MAX_PATH);
    get_tessdata_dir(tessDataDir, MAX_PATH);

    wchar_t cmdLine[4096];
    swprintf_s(cmdLine, 4096, L"\"%s\" \"%s\" stdout -l %s --psm %d --oem %d --tessdata-dir \"%s\" -c preserve_interword_spaces=1 -c user_defined_dpi=300",
               tessExe, targetImg, activeLangs, (psm > 0 ? psm : 6), (oem >= 0 ? oem : 1), tessDataDir);

    HANDLE hStdOutRead = NULL, hStdOutWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        if (preprocessed && wcscmp(prepImagePath, imagePath) != 0) DeleteFileW(prepImagePath);
        return false;
    }
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hStdOutWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hStdOutRead);
        CloseHandle(hStdOutWrite);
        if (preprocessed && wcscmp(prepImagePath, imagePath) != 0) DeleteFileW(prepImagePath);
        return false;
    }

    CloseHandle(hStdOutWrite);

    char *rawBuf = (char*)malloc(262144);
    DWORD totalBytes = 0;
    if (rawBuf) {
        DWORD bytesRead = 0;
        while (ReadFile(hStdOutRead, rawBuf + totalBytes, 262144 - totalBytes - 1, &bytesRead, NULL) && bytesRead > 0) {
            totalBytes += bytesRead;
            if (totalBytes >= 262140) break;
        }
        rawBuf[totalBytes] = '\0';
    }

    WaitForSingleObject(pi.hProcess, 45000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdOutRead);

    if (preprocessed && wcscmp(prepImagePath, imagePath) != 0) {
        DeleteFileW(prepImagePath);
    }

    if (!rawBuf || totalBytes == 0) {
        if (rawBuf) free(rawBuf);
        return false;
    }

    /* Convert UTF-8 output to wide char */
    int wideChars = MultiByteToWideChar(CP_UTF8, 0, rawBuf, totalBytes, outText, maxTextLen - 1);
    free(rawBuf);

    if (wideChars <= 0) {
        return false;
    }
    outText[wideChars] = L'\0';

    /* Clean trailing whitespace / newlines / form feeds */
    while (wideChars > 0 && (outText[wideChars - 1] == L'\n' || outText[wideChars - 1] == L'\r' ||
           outText[wideChars - 1] == L' ' || outText[wideChars - 1] == L'\f' || outText[wideChars - 1] == L'\t')) {
        outText[--wideChars] = L'\0';
    }

    return (wcslen(outText) > 0);
}

/* ------------------------------------------------------------------ */
/* Main CLI / Entry Point                                              */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    EnsureConsoleAttached();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    wchar_t cmdBuf[8192];
    wcscpy_s(cmdBuf, sizeof(cmdBuf)/sizeof(wchar_t), GetCommandLineW());

    wchar_t *argv[TINY_CLI_MAX_ARGS];
    int argc = TinyCLI_Tokenize(cmdBuf, argv, TINY_CLI_MAX_ARGS);

    const wchar_t *imageArg = NULL;
    const wchar_t *langArg = L"eng";
    int psmArg = 6;
    int oemArg = 1;
    bool helpArg = false;
    bool versionArg = false;

    const CliOption opts[] = {
        { L"--image",   CLI_OPT_STRING, &imageArg,   0,  0 },
        { L"-i",        CLI_OPT_STRING, &imageArg,   0,  0 },
        { L"--lang",    CLI_OPT_STRING, &langArg,    0,  0 },
        { L"-l",        CLI_OPT_STRING, &langArg,    0,  0 },
        { L"--psm",     CLI_OPT_INT,    &psmArg,     0, 13 },
        { L"--oem",     CLI_OPT_INT,    &oemArg,     0,  3 },
        { L"--help",    CLI_OPT_BOOL,   &helpArg,    0,  0 },
        { L"-h",        CLI_OPT_BOOL,   &helpArg,    0,  0 },
        { L"--version", CLI_OPT_BOOL,   &versionArg, 0,  0 },
        { L"-v",        CLI_OPT_BOOL,   &versionArg, 0,  0 }
    };

    TinyCLI_Parse(argc, argv, opts, sizeof(opts) / sizeof(opts[0]));

    if (helpArg) {
        fwprintf(stderr, L"ocr.exe - tiny Windows OCR utility\n");
        fwprintf(stderr, L"Usage: ocr.exe [--image] <image> [--lang eng|auto] [--psm 6] [--oem 1]\n");
        fwprintf(stderr, L"       ocr.exe  (launches native file picker if no image passed)\n");
        CoUninitialize();
        return 0;
    }

    if (versionArg) {
        fwprintf(stderr, L"ocr.exe 1.1.0 (Win32 Tesseract OCR)\n");
        CoUninitialize();
        return 0;
    }

    /* Check positional argument for image path if --image was not set */
    if (!imageArg) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != L'-') {
                imageArg = argv[i];
                break;
            }
        }
    }

    wchar_t selectedImage[MAX_PATH] = {0};
    bool interactiveMode = false;

    if (imageArg) {
        wcscpy_s(selectedImage, MAX_PATH, imageArg);
    } else {
        /* No image passed via CLI: open native File Explorer picker dialog */
        interactiveMode = true;
        if (!PickImageFileDialog(selectedImage, MAX_PATH)) {
            /* User cancelled the dialog */
            CoUninitialize();
            return 0;
        }
    }

    /* Validate format */
    const wchar_t *ext = wcsrchr(selectedImage, L'.');
    if (!ext || (_wcsicmp(ext, L".png") != 0 && _wcsicmp(ext, L".jpg") != 0 &&
                 _wcsicmp(ext, L".jpeg") != 0 && _wcsicmp(ext, L".bmp") != 0)) {
        fwprintf(stderr, L"[-] Error: Unsupported image format. Supported formats: PNG, JPG, JPEG, BMP.\n");
        if (interactiveMode) {
            MessageBoxW(NULL, L"Unsupported image format. Only PNG, JPG, JPEG, and BMP files are supported.", L"OCR Error", MB_OK | MB_ICONERROR);
        }
        CoUninitialize();
        return 2;
    }

    if (!file_exists_and_valid(selectedImage, 10)) {
        fwprintf(stderr, L"[-] Error: Image file not found: %s\n", selectedImage);
        if (interactiveMode) {
            MessageBoxW(NULL, L"The selected image file was not found.", L"OCR Error", MB_OK | MB_ICONERROR);
        }
        CoUninitialize();
        return 1;
    }

    /* Run OCR */
    wchar_t recognizedText[65536] = {0};
    if (!RunOCR(selectedImage, langArg, psmArg, oemArg, recognizedText, 65536)) {
        fwprintf(stderr, L"[-] No text recognized or OCR processing failed.\n");
        if (interactiveMode) {
            MessageBoxW(NULL, L"No text was recognized in the selected image.", L"OCR Result", MB_OK | MB_ICONINFORMATION);
        }
        CoUninitialize();
        return 1;
    }

    /* Copy to clipboard */
    CopyToClipboard(recognizedText);

    /* Output to stdout */
    OutputText(recognizedText);

    /* If launched interactively without CLI args, display native alert / notification */
    if (interactiveMode) {
        wchar_t alertMsg[4096];
        swprintf_s(alertMsg, 4096, L"OCR Recognition Complete!\n\nRecognized text copied to clipboard:\n\n%s", recognizedText);
        MessageBoxW(NULL, alertMsg, L"OCR Result", MB_OK | MB_ICONINFORMATION);
    }

    CoUninitialize();
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    return wWinMain(hInstance, hPrevInstance, NULL, nCmdShow);
}
