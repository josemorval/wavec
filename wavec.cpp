// =============================================================================
// wavec.cpp
// Wavelet Image Codec with .WT file format
// 
// This application implements a wavelet-based image compression system that:
// 1. Loads 24-bit BMP images
// 2. Applies 2D Discrete Wavelet Transform (DWT) using Haar, Daubechies-4, or CDF 9/7
// 3. Discards small coefficients for compression
// 4. Quantizes remaining coefficients
// 5. Saves to a custom .WT format
// 6. Reconstructs images using Inverse DWT
//
// Compilation: cl /O2 wavec.cpp user32.lib gdi32.lib comdlg32.lib comctl32.lib
//
// References for DWT implementations:
// - Haar Wavelet: https://en.wikipedia.org/wiki/Haar_wavelet
// - Daubechies Wavelets: https://en.wikipedia.org/wiki/Daubechies_wavelet
// - CDF 9/7 (used in JPEG2000): https://en.wikipedia.org/wiki/Cohen%E2%80%93Daubechies%E2%80%93Feauveau_wavelet
// - Lifting Scheme: https://en.wikipedia.org/wiki/Lifting_scheme
// - "Wavelets and Subband Coding" by Vetterli & Kovačević
// =============================================================================

#define WIN32_LEAN_AND_MEAN  // Exclude rarely-used Windows headers for faster compilation
#define UNICODE              // Enable Unicode support
#define _UNICODE             // Enable Unicode support for C runtime

#include <windows.h>
#include <commctrl.h>        // Common controls (status bar, trackbar, etc.)
#include <commdlg.h>         // Common dialogs (Open/Save file dialogs)
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define IDI_APPICON 101      // Resource ID for application icon

#pragma comment(lib, "comctl32.lib")  // Link common controls library

// =============================================================================
// Menu and Control IDs
// =============================================================================
enum {
    ID_OPEN_BMP = 100,  // Open BMP file menu item
    ID_OPEN_WT,         // Open WT file menu item
    ID_SAVE_BMP,        // Save as BMP menu item
    ID_SAVE_WT,         // Save as WT menu item
    ID_EXIT,            // Exit application
    ID_TRANSFORM,       // Apply wavelet transform
    ID_RESET,           // Reset to original image
    ID_ABOUT            // About dialog
};

// =============================================================================
// Custom .WT File Format Header
// 
// The .WT format stores wavelet-transformed image data efficiently by:
// - Only storing non-zero coefficients (sparse storage)
// - Quantizing coefficients to reduce bit depth
// - Storing coefficient range for dequantization
// =============================================================================
#define WT_MAGIC 0x32545757  // Magic number "WWT2" to identify .WT files

#pragma pack(push, 1)  // Ensure struct is packed without padding
typedef struct {
    unsigned int magic;         // File identifier (WT_MAGIC)
    unsigned short width;       // Image width in pixels
    unsigned short height;      // Image height in pixels
    unsigned char waveletType;  // 0=Haar, 1=Daubechies-4, 2=CDF 9/7
    unsigned char levels;       // Number of decomposition levels
    unsigned char quantBits;    // Bits used for quantization (2-32)
    unsigned char reserved;     // Reserved for future use
    float coeffMin;             // Minimum coefficient value (for dequantization)
    float coeffMax;             // Maximum coefficient value (for dequantization)
    unsigned int nonZeroCount;  // Number of non-zero coefficients stored
    unsigned int originalSize;  // Original BMP file size (for ratio calculation)
} WTHeader;
#pragma pack(pop)

// =============================================================================
// Global Variables
// =============================================================================
static HINSTANCE g_inst;        // Application instance handle
static HWND g_hwnd;             // Main window handle
static HWND g_status;           // Status bar handle

// Image data buffers
static BYTE* g_original = NULL;   // Original image RGB data (R,G,B interleaved)
static BYTE* g_result = NULL;     // Reconstructed image RGB data
static BYTE* g_waveletVis = NULL; // Visualization of wavelet coefficients
static double* g_coeffs = NULL;   // Wavelet coefficients (all 3 channels)

// Image dimensions and state flags
static int g_w = 0, g_h = 0;           // Image width and height
static BOOL g_loaded = FALSE;          // True if any image is loaded
static BOOL g_transformed = FALSE;     // True if transform has been applied
static BOOL g_hasOriginal = FALSE;     // True if original image is available

// Transform parameters
static int g_waveletType = 0;    // 0=Haar, 1=Db4, 2=CDF 9/7
static int g_levels = 0;         // Decomposition levels (0=auto)
static double g_discard = 90.0;  // Percentage of smallest coefficients to discard
static int g_quantBits = 12;     // Quantization bit depth
static int g_usedLevels = 1;     // Actual levels used in transform
static int g_maxLevels = 1;      // Maximum possible levels for current image

// Statistics
static double g_coeffMin, g_coeffMax;  // Coefficient value range
static int g_totalCoeffs, g_nonZero;   // Total and non-zero coefficient counts
static double g_psnr = 0;              // Peak Signal-to-Noise Ratio
static int g_bmpSize = 0;              // Original BMP file size
static int g_wtSize = 0;               // Compressed WT file size

// =============================================================================
// Daubechies-4 Wavelet Filter Coefficients
// 
// These are the scaling function (low-pass) coefficients for the Db4 wavelet.
// The wavelet function (high-pass) coefficients are derived using the
// quadrature mirror filter relationship: g[k] = (-1)^k * h[N-1-k]
// 
// Reference: Daubechies, I. (1988). "Orthonormal bases of compactly supported wavelets"
// https://services.math.duke.edu/~ingrid/publications/cpam41-1988.pdf
// =============================================================================
static const double DB4[] = {
    0.4829629131445341,   // h0 - scaling coefficient
    0.8365163037378079,   // h1 - scaling coefficient
    0.2241438680420134,   // h2 - scaling coefficient
   -0.1294095225512604    // h3 - scaling coefficient
};

// =============================================================================
// CDF 9/7 Wavelet Lifting Coefficients
// 
// The Cohen-Daubechies-Feauveau 9/7 wavelet is a biorthogonal wavelet used in
// JPEG2000 for lossy compression. It's implemented using the lifting scheme,
// which is more efficient than convolution-based approaches.
// 
// The lifting scheme decomposes the wavelet transform into a sequence of
// simple filtering operations called "lifting steps":
// 1. Split: Separate even and odd samples
// 2. Predict: Use even samples to predict odd samples (creates detail coefficients)
// 3. Update: Use detail coefficients to update even samples (creates approximation)
// 
// Reference: 
// - Daubechies, I. & Sweldens, W. (1998). "Factoring wavelet transforms into lifting steps"
//   https://www.jstor.org/stable/119066
// - JPEG2000 Standard (ISO/IEC 15444-1)
// =============================================================================
static const double CDF_A = -1.586134342059924;   // First predict step coefficient (alpha)
static const double CDF_B = -0.052980118572961;   // First update step coefficient (beta)
static const double CDF_C =  0.882911075530934;   // Second predict step coefficient (gamma)
static const double CDF_D =  0.443506852043971;   // Second update step coefficient (delta)
static const double CDF_K =  1.149604398860241;   // Scaling factor (zeta)

// =============================================================================
// Forward Declarations
// =============================================================================
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void CreateMainMenu(HWND);
static void UpdateMenu(void);
static void Status(const wchar_t*);

static BOOL LoadBMP(const wchar_t*);
static BOOL SaveBMP(const wchar_t*);
static BOOL LoadWT(const wchar_t*);
static BOOL SaveWT(const wchar_t*);
static void FreeAll(void);

// 1D Wavelet transforms (forward and inverse)
static void HaarFwd(double*, int);
static void HaarInv(double*, int);
static void Db4Fwd(double*, int);
static void Db4Inv(double*, int);
static void Cdf97Fwd(double*, int);
static void Cdf97Inv(double*, int);

// 2D Wavelet transforms
static void Fwd2D(double*, int, int, int, int);
static void Inv2D(double*, int, int, int, int);

static void DoTransform(void);
static void DoReconstruct(void);
static void DoReset(void);
static void UpdateVis(void);
static double CalcPSNR(void);
static int CalcEstimatedSize(void);
static void ApplyQuantization(void);

static void Paint(HDC, RECT*);
static BOOL ShowDialog(HWND);

// =============================================================================
// Application Entry Point
// =============================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    g_inst = inst;
    
    // Initialize common controls (required for status bar, trackbar, etc.)
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_BAR_CLASSES};
    InitCommonControlsEx(&ic);
    
    // Register main window class
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_APPWORKSPACE+1);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APPICON));
    wc.lpszClassName = L"WaveletMain";
    RegisterClassW(&wc);
    
    // Create main window
    g_hwnd = CreateWindowW(L"WaveletMain", L"Wavec - another wavelet encoder",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 600,
        NULL, NULL, inst, NULL);
    
    ShowWindow(g_hwnd, show);
    
    // Main message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    FreeAll();  // Clean up allocated memory
    return 0;
}

// =============================================================================
// Main Window Procedure
// Handles all window messages (paint, resize, menu commands, keyboard, etc.)
// =============================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        // Create menu bar and status bar when window is created
        CreateMainMenu(hwnd);
        g_status = CreateWindowW(STATUSCLASSNAMEW, L"Open a BMP or WT file",
            WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, NULL, g_inst, NULL);
        return 0;
        
    case WM_SIZE:
        // Resize status bar and repaint when window size changes
        SendMessage(g_status, WM_SIZE, 0, 0);
        InvalidateRect(hwnd, NULL, TRUE);  // Force repaint
        return 0;
        
    case WM_PAINT: {
        // Handle window painting
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        RECT st; GetWindowRect(g_status, &st);
        rc.bottom -= (st.bottom - st.top);  // Exclude status bar area
        Paint(hdc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    
    case WM_COMMAND:
        // Handle menu commands
        switch (LOWORD(wp)) {
        case ID_OPEN_BMP: {
            // Show Open File dialog for BMP files
            wchar_t path[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"BMP Files\0*.bmp\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn) && LoadBMP(path)) {
                wchar_t s[64]; swprintf(s, 64, L"Loaded %dx%d", g_w, g_h);
                Status(s);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } return 0;
        
        case ID_OPEN_WT: {
            // Show Open File dialog for WT files
            wchar_t path[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"WT Files\0*.wt\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&ofn) && LoadWT(path)) {
                wchar_t s[128];
                const wchar_t* wn = (g_waveletType == 0) ? L"Haar" : (g_waveletType == 1) ? L"Db4" : L"CDF97";
                swprintf(s, 128, L"Loaded WT: %dx%d, %s, %d levels, %d bits", g_w, g_h, wn, g_usedLevels, g_quantBits);
                Status(s);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        } return 0;
        
        case ID_SAVE_BMP: {
            // Show Save File dialog for BMP files
            wchar_t path[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"BMP Files\0*.bmp\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT;
            ofn.lpstrDefExt = L"bmp";
            if (GetSaveFileNameW(&ofn) && SaveBMP(path))
                Status(L"BMP saved");
        } return 0;
        
        case ID_SAVE_WT: {
            // Show Save File dialog for WT files
            wchar_t path[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {sizeof(ofn)};
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"WT Files\0*.wt\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT;
            ofn.lpstrDefExt = L"wt";
            if (GetSaveFileNameW(&ofn) && SaveWT(path)) {
                wchar_t s[128];
                double ratio = g_wtSize > 0 ? (double)g_bmpSize / g_wtSize : 0;
                swprintf(s, 128, L"WT saved: %d KB (ratio %.1f:1)", g_wtSize/1024, ratio);
                Status(s);
            }
        } return 0;
        
        case ID_EXIT:
            DestroyWindow(hwnd);
            return 0;
            
        case ID_TRANSFORM:
            // Apply wavelet transform with user-selected parameters
            if (g_loaded && g_hasOriginal && ShowDialog(hwnd)) {
                SetCursor(LoadCursor(NULL, IDC_WAIT));  // Show wait cursor
                DoTransform();
                DoReconstruct();
                SetCursor(LoadCursor(NULL, IDC_ARROW));  // Restore cursor
                wchar_t s[128];
                int estSize = CalcEstimatedSize();
                double ratio = estSize > 0 ? (double)g_bmpSize / estSize : 0;
                swprintf(s, 128, L"PSNR: %.2f dB | Est. ratio: %.1f:1", g_psnr, ratio);
                Status(s);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
            
        case ID_RESET:
            // Reset to original image
            if (g_transformed && g_hasOriginal) {
                DoReset();
                Status(L"Reset");
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
            
        case ID_ABOUT: {
            // Show About dialog
            MSGBOXPARAMSW mbp = {0};
            mbp.cbSize = sizeof(mbp);
            mbp.hwndOwner = hwnd;
            mbp.hInstance = g_inst;
            mbp.lpszText = 
                L"Wavelet Image Codec\n\n"
                L"Wavelets: Haar, Daubechies-4, CDF 9/7\n\n"
                L"F5 = Transform\n"
                L"F7 = Reset\n\n"
                L"Quantization is applied to coefficients\n"
                L"before reconstruction (simulates save/load)";
            mbp.lpszCaption = L"About Wavelet Codec";
            mbp.dwStyle = MB_OK | MB_USERICON;
            mbp.lpszIcon = MAKEINTRESOURCEW(IDI_APPICON);
            MessageBoxIndirectW(&mbp);
        }
        return 0;
        }
        break;
        
    case WM_KEYDOWN:
        // Keyboard shortcuts
        if (wp == VK_F5) SendMessage(hwnd, WM_COMMAND, ID_TRANSFORM, 0);
        else if (wp == VK_F7) SendMessage(hwnd, WM_COMMAND, ID_RESET, 0);
        return 0;
        
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// =============================================================================
// Menu Creation and Management
// =============================================================================
static void CreateMainMenu(HWND hwnd)
{
    HMENU bar = ::CreateMenu();
    
    // File menu
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_OPEN_BMP, L"Open BMP...\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_OPEN_WT, L"Open WT...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_SAVE_BMP, L"Save BMP...\tCtrl+S");
    AppendMenuW(file, MF_STRING, ID_SAVE_WT, L"Save WT...");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"Exit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    
    // Process menu
    HMENU proc = CreatePopupMenu();
    AppendMenuW(proc, MF_STRING, ID_TRANSFORM, L"Transform...\tF5");
    AppendMenuW(proc, MF_STRING, ID_RESET, L"Reset\tF7");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)proc, L"&Process");
    
    // Help menu
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"About...");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    
    SetMenu(hwnd, bar);
    UpdateMenu();
}

// Update menu item enabled/disabled states based on current application state
static void UpdateMenu(void)
{
    HMENU m = GetMenu(g_hwnd);
    EnableMenuItem(m, ID_SAVE_BMP, g_loaded ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, ID_SAVE_WT, g_transformed ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, ID_TRANSFORM, (g_loaded && g_hasOriginal) ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, ID_RESET, (g_transformed && g_hasOriginal) ? MF_ENABLED : MF_GRAYED);
}

// Update status bar text
static void Status(const wchar_t* s) { SetWindowTextW(g_status, s); }

// =============================================================================
// BMP File I/O
// 
// BMP (Bitmap) is a simple uncompressed image format.
// We only support 24-bit (RGB) BMPs for simplicity.
// =============================================================================
static BOOL LoadBMP(const wchar_t* path)
{
    FILE* f = _wfopen(path, L"rb");
    if (!f) return FALSE;
    
    // Get file size for compression ratio calculation
    fseek(f, 0, SEEK_END);
    g_bmpSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Read BMP headers
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    fread(&bfh, sizeof(bfh), 1, f);
    fread(&bih, sizeof(bih), 1, f);
    
    // Validate: must be BMP signature and 24-bit color
    if (bfh.bfType != 0x4D42 || bih.biBitCount != 24) {
        fclose(f);
        MessageBoxW(g_hwnd, L"Only 24-bit BMP supported", L"Error", MB_ICONERROR);
        return FALSE;
    }
    
    FreeAll();  // Free any previously loaded image
    
    g_w = bih.biWidth;
    g_h = abs(bih.biHeight);
    // BMP can be stored top-down (negative height) or bottom-up (positive height)
    BOOL flip = (bih.biHeight > 0);
    
    // BMP rows are padded to 4-byte boundaries
    int row = ((g_w * 3 + 3) / 4) * 4;
    int sz = g_w * g_h * 3;
    
    // Allocate buffers
    g_original = (BYTE*)malloc(sz);
    g_result = (BYTE*)malloc(sz);
    BYTE* buf = (BYTE*)malloc(row);
    
    // Read pixel data, converting BGR to RGB and handling row order
    fseek(f, bfh.bfOffBits, SEEK_SET);
    for (int y = 0; y < g_h; y++) {
        int dy = flip ? (g_h - 1 - y) : y;  // Handle bottom-up storage
        fread(buf, 1, row, f);
        for (int x = 0; x < g_w; x++) {
            int si = x * 3, di = (dy * g_w + x) * 3;
            // Convert BGR (BMP format) to RGB (our internal format)
            g_original[di+0] = buf[si+2];  // R
            g_original[di+1] = buf[si+1];  // G
            g_original[di+2] = buf[si+0];  // B
        }
    }
    free(buf);
    fclose(f);
    
    // Initialize result with original
    memcpy(g_result, g_original, sz);
    g_loaded = TRUE;
    g_transformed = FALSE;
    g_hasOriginal = TRUE;
    
    // Calculate maximum decomposition levels based on image size
    // Each level halves the dimensions, minimum size depends on wavelet
    int m = (g_w < g_h) ? g_w : g_h;
    g_maxLevels = 0;
    while (m >= 4) { m /= 2; g_maxLevels++; }
    if (g_maxLevels < 1) g_maxLevels = 1;
    if (g_maxLevels > 8) g_maxLevels = 8;
    
    UpdateMenu();
    return TRUE;
}

static BOOL SaveBMP(const wchar_t* path)
{
    if (!g_result) return FALSE;
    
    FILE* f = _wfopen(path, L"wb");
    if (!f) return FALSE;
    
    // Calculate row size with padding
    int row = ((g_w * 3 + 3) / 4) * 4;
    int imgSz = row * g_h;
    
    // Prepare BMP file header
    BITMAPFILEHEADER bfh = {0};
    bfh.bfType = 0x4D42;           // "BM" signature
    bfh.bfSize = 54 + imgSz;       // Total file size
    bfh.bfOffBits = 54;            // Offset to pixel data
    
    // Prepare BMP info header
    BITMAPINFOHEADER bih = {0};
    bih.biSize = 40;               // Header size
    bih.biWidth = g_w;
    bih.biHeight = g_h;            // Positive = bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    
    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    
    // Write pixel data (bottom-up, BGR format)
    BYTE* buf = (BYTE*)calloc(row, 1);
    for (int y = g_h - 1; y >= 0; y--) {
        for (int x = 0; x < g_w; x++) {
            int si = (y * g_w + x) * 3, di = x * 3;
            // Convert RGB to BGR
            buf[di+0] = g_result[si+2];  // B
            buf[di+1] = g_result[si+1];  // G
            buf[di+2] = g_result[si+0];  // R
        }
        fwrite(buf, 1, row, f);
    }
    free(buf);
    fclose(f);
    return TRUE;
}

// =============================================================================
// WT (Wavelet Transform) File I/O
// 
// Custom sparse format that stores only non-zero coefficients.
// Each non-zero coefficient is stored as (index, quantized_value) pair.
// =============================================================================

// Calculate bytes needed to store a quantized value
static int CalcValBytes(int bits) {
    if (bits <= 8) return 1;
    if (bits <= 16) return 2;
    if (bits <= 24) return 3;
    return 4;
}

static BOOL LoadWT(const wchar_t* path)
{
    FILE* f = _wfopen(path, L"rb");
    if (!f) return FALSE;
    
    // Get file size
    fseek(f, 0, SEEK_END);
    g_wtSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Read and validate header
    WTHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != WT_MAGIC) {
        fclose(f);
        MessageBoxW(g_hwnd, L"Invalid WT file", L"Error", MB_ICONERROR);
        return FALSE;
    }
    
    FreeAll();
    
    // Extract parameters from header
    g_w = hdr.width;
    g_h = hdr.height;
    g_waveletType = hdr.waveletType;
    g_usedLevels = hdr.levels;
    g_quantBits = hdr.quantBits;
    g_coeffMin = hdr.coeffMin;
    g_coeffMax = hdr.coeffMax;
    g_nonZero = hdr.nonZeroCount;
    g_bmpSize = hdr.originalSize;
    
    int pixels = g_w * g_h;
    g_totalCoeffs = pixels * 3;  // 3 color channels
    
    // Allocate buffers (coefficients initialized to zero)
    g_coeffs = (double*)calloc(g_totalCoeffs, sizeof(double));
    g_result = (BYTE*)malloc(pixels * 3);
    g_waveletVis = (BYTE*)malloc(pixels * 3);
    
    // Dequantization parameters
    double range = g_coeffMax - g_coeffMin;
    if (range < 1e-10) range = 1;
    unsigned long long maxQ = ((unsigned long long)1 << g_quantBits) - 1;
    
    // Calculate bytes per index and value based on data size
    int idxBytes = (g_totalCoeffs <= 65536) ? 2 : (g_totalCoeffs <= 16777216) ? 3 : 4;
    int valBytes = CalcValBytes(g_quantBits);
    
    // Read non-zero coefficients and dequantize
    for (unsigned int i = 0; i < hdr.nonZeroCount; i++) {
        unsigned int idx = 0;
        unsigned long long qval = 0;
        
        fread(&idx, idxBytes, 1, f);
        fread(&qval, valBytes, 1, f);
        
        if (idx < (unsigned int)g_totalCoeffs) {
            // Dequantize: map from [0, maxQ] back to [coeffMin, coeffMax]
            g_coeffs[idx] = g_coeffMin + ((double)qval / maxQ) * range;
        }
    }
    fclose(f);
    
    // Update state
    g_loaded = TRUE;
    g_transformed = TRUE;
    g_hasOriginal = FALSE;  // No original when loading from WT
    g_original = NULL;
    
    // Calculate max levels for this image size
    int m = (g_w < g_h) ? g_w : g_h;
    g_maxLevels = 0;
    while (m >= 4) { m /= 2; g_maxLevels++; }
    if (g_maxLevels < 1) g_maxLevels = 1;
    if (g_maxLevels > 8) g_maxLevels = 8;
    
    UpdateVis();
    DoReconstruct();
    g_psnr = 0;  // Can't calculate PSNR without original
    
    UpdateMenu();
    return TRUE;
}

static BOOL SaveWT(const wchar_t* path)
{
    if (!g_coeffs || !g_transformed) return FALSE;
    
    FILE* f = _wfopen(path, L"wb");
    if (!f) return FALSE;
    
    // Find min/max of non-zero coefficients for quantization range
    double minV = 1e30, maxV = -1e30;
    int nz = 0;
    for (int i = 0; i < g_totalCoeffs; i++) {
        if (g_coeffs[i] != 0) {
            nz++;
            if (g_coeffs[i] < minV) minV = g_coeffs[i];
            if (g_coeffs[i] > maxV) maxV = g_coeffs[i];
        }
    }
    if (nz == 0) { minV = 0; maxV = 1; }
    
    // Prepare header
    WTHeader hdr;
    hdr.magic = WT_MAGIC;
    hdr.width = (unsigned short)g_w;
    hdr.height = (unsigned short)g_h;
    hdr.waveletType = (unsigned char)g_waveletType;
    hdr.levels = (unsigned char)g_usedLevels;
    hdr.quantBits = (unsigned char)g_quantBits;
    hdr.reserved = 0;
    hdr.coeffMin = (float)minV;
    hdr.coeffMax = (float)maxV;
    hdr.nonZeroCount = nz;
    hdr.originalSize = g_bmpSize;
    
    fwrite(&hdr, sizeof(hdr), 1, f);
    
    // Quantization parameters
    double range = maxV - minV;
    if (range < 1e-10) range = 1;
    unsigned long long maxQ = ((unsigned long long)1 << g_quantBits) - 1;
    
    // Calculate bytes per index and value
    int idxBytes = (g_totalCoeffs <= 65536) ? 2 : (g_totalCoeffs <= 16777216) ? 3 : 4;
    int valBytes = CalcValBytes(g_quantBits);
    
    // Write non-zero coefficients as (index, quantized_value) pairs
    for (int i = 0; i < g_totalCoeffs; i++) {
        if (g_coeffs[i] != 0) {
            unsigned int idx = i;
            // Quantize: map from [minV, maxV] to [0, maxQ]
            unsigned long long qval = (unsigned long long)(((g_coeffs[i] - minV) / range) * maxQ + 0.5);
            if (qval > maxQ) qval = maxQ;
            
            fwrite(&idx, idxBytes, 1, f);
            fwrite(&qval, valBytes, 1, f);
        }
    }
    fclose(f);
    
    // Update file size
    f = _wfopen(path, L"rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        g_wtSize = ftell(f);
        fclose(f);
    }
    
    return TRUE;
}

// Free all allocated memory
static void FreeAll(void)
{
    free(g_original); g_original = NULL;
    free(g_result); g_result = NULL;
    free(g_waveletVis); g_waveletVis = NULL;
    free(g_coeffs); g_coeffs = NULL;
    g_w = g_h = 0;
    g_loaded = g_transformed = g_hasOriginal = FALSE;
}

// =============================================================================
// 1D WAVELET TRANSFORMS
// 
// These implement the core wavelet transform algorithms.
// Each wavelet has forward (analysis) and inverse (synthesis) versions.
// =============================================================================

// =============================================================================
// HAAR WAVELET
// 
// The simplest wavelet, using averages and differences.
// Forward: For each pair (a, b), compute:
//   - Low-pass (average):  (a + b) / sqrt(2)
//   - High-pass (detail):  (a - b) / sqrt(2)
// 
// The sqrt(2) normalization ensures energy preservation (orthonormal basis).
// 
// Reference: https://en.wikipedia.org/wiki/Haar_wavelet
// =============================================================================
static void HaarFwd(double* d, int n)
{
    if (n < 2) return;
    
    double* t = (double*)malloc(n * sizeof(double));
    int h = n / 2;  // Half the length
    const double S = 0.7071067811865476;  // 1/sqrt(2) for normalization
    
    for (int i = 0; i < h; i++) {
        // Low-pass: average of pair (approximation coefficients)
        t[i] = (d[2*i] + d[2*i+1]) * S;
        // High-pass: difference of pair (detail coefficients)
        t[h+i] = (d[2*i] - d[2*i+1]) * S;
    }
    
    memcpy(d, t, n * sizeof(double));
    free(t);
}

// Haar inverse transform
static void HaarInv(double* d, int n)
{
    if (n < 2) return;
    
    double* t = (double*)malloc(n * sizeof(double));
    int h = n / 2;
    const double S = 0.7071067811865476;  // 1/sqrt(2)
    
    for (int i = 0; i < h; i++) {
        // Reconstruct original pair from low-pass and high-pass
        t[2*i] = (d[i] + d[h+i]) * S;      // First sample
        t[2*i+1] = (d[i] - d[h+i]) * S;    // Second sample
    }
    
    memcpy(d, t, n * sizeof(double));
    free(t);
}

// =============================================================================
// DAUBECHIES-4 WAVELET
// 
// A 4-tap orthogonal wavelet with better frequency localization than Haar.
// Uses overlapping filters that provide smoother approximations.
// 
// The filter coefficients (DB4 array) are derived from the requirement that
// the wavelet has 2 vanishing moments (can represent linear polynomials exactly).
// 
// Forward transform uses:
//   - Low-pass filter h[k] = DB4[k]
//   - High-pass filter g[k] = (-1)^k * h[3-k]  (quadrature mirror filter)
// 
// Reference: 
// - Daubechies, I. "Ten Lectures on Wavelets" (1992)
// - https://en.wikipedia.org/wiki/Daubechies_wavelet
// =============================================================================
static void Db4Fwd(double* d, int n)
{
    // Fall back to Haar for very short signals
    if (n < 4) { HaarFwd(d, n); return; }
    
    double* t = (double*)malloc(n * sizeof(double));
    int h = n / 2;
    
    // High-pass filter coefficients (derived from low-pass via QMF relationship)
    double g0 = DB4[3], g1 = -DB4[2], g2 = DB4[1], g3 = -DB4[0];
    
    for (int i = 0; i < h; i++) {
        int k = 2 * i;
        // Get 4 consecutive samples with periodic boundary (circular convolution)
        double d0 = d[k%n], d1 = d[(k+1)%n], d2 = d[(k+2)%n], d3 = d[(k+3)%n];
        
        // Low-pass: convolution with scaling filter
        t[i] = DB4[0]*d0 + DB4[1]*d1 + DB4[2]*d2 + DB4[3]*d3;
        // High-pass: convolution with wavelet filter
        t[h+i] = g0*d0 + g1*d1 + g2*d2 + g3*d3;
    }
    
    memcpy(d, t, n * sizeof(double));
    free(t);
}

// Daubechies-4 inverse transform
static void Db4Inv(double* d, int n)
{
    if (n < 4) { HaarInv(d, n); return; }
    
    double* t = (double*)calloc(n, sizeof(double));  // Zero-initialized
    int h = n / 2;
    
    // High-pass filter coefficients
    double g0 = DB4[3], g1 = -DB4[2], g2 = DB4[1], g3 = -DB4[0];
    
    // Reconstruction: each coefficient pair contributes to 4 output samples
    for (int i = 0; i < h; i++) {
        double lo = d[i], hi = d[h+i];
        int k = 2 * i;
        
        // Accumulate contributions (synthesis filter bank)
        t[k%n] += DB4[0]*lo + g0*hi;
        t[(k+1)%n] += DB4[1]*lo + g1*hi;
        t[(k+2)%n] += DB4[2]*lo + g2*hi;
        t[(k+3)%n] += DB4[3]*lo + g3*hi;
    }
    
    memcpy(d, t, n * sizeof(double));
    free(t);
}

// =============================================================================
// CDF 9/7 WAVELET (Cohen-Daubechies-Feauveau)
// 
// A biorthogonal wavelet used in JPEG2000 for lossy compression.
// Implemented using the LIFTING SCHEME, which is more efficient than
// direct convolution and allows in-place computation.
// 
// The lifting scheme works by:
// 1. Split signal into even and odd samples
// 2. Apply alternating predict and update steps
// 3. Scale the results
// 
// For CDF 9/7, the lifting steps are:
//   odd  += alpha * (even[i-1] + even[i])     // Predict 1
//   even += beta  * (odd[i-1]  + odd[i])      // Update 1
//   odd  += gamma * (even[i-1] + even[i])     // Predict 2
//   even += delta * (odd[i-1]  + odd[i])      // Update 2
//   even *= K                                  // Scale low-pass
//   odd  /= K                                  // Scale high-pass
// 
// References:
// - Sweldens, W. "The Lifting Scheme: A Construction of Second Generation Wavelets"
//   https://epubs.siam.org/doi/10.1137/S0036141095289051
// - Adams, M.D. "The JPEG-2000 Still Image Compression Standard"
// =============================================================================

// Mirror boundary extension: reflects indices at boundaries
// This handles edge cases where filter extends beyond signal bounds
static inline int Mir(int i, int n) {
    if (i < 0) return -i;           // Reflect at left boundary
    if (i >= n) return 2*n - 2 - i; // Reflect at right boundary
    return i;
}

static void Cdf97Fwd(double* d, int n)
{
    if (n < 4) { HaarFwd(d, n); return; }
    
    // Lifting steps (in-place modification)
    // Step 1: Predict - update odd samples using even neighbors
    for (int i = 1; i < n; i += 2)
        d[i] += CDF_A * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    // Step 2: Update - update even samples using odd neighbors
    for (int i = 0; i < n; i += 2)
        d[i] += CDF_B * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    // Step 3: Predict - second prediction step
    for (int i = 1; i < n; i += 2)
        d[i] += CDF_C * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    // Step 4: Update - second update step
    for (int i = 0; i < n; i += 2)
        d[i] += CDF_D * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    // Step 5: Scale - normalize coefficients
    for (int i = 0; i < n; i += 2) d[i] *= CDF_K;   // Scale low-pass up
    for (int i = 1; i < n; i += 2) d[i] /= CDF_K;   // Scale high-pass down
    
    // Reorder: separate even (low-pass) and odd (high-pass) samples
    double* t = (double*)malloc(n * sizeof(double));
    int j = 0;
    for (int i = 0; i < n; i += 2) t[j++] = d[i];  // Low-pass first
    for (int i = 1; i < n; i += 2) t[j++] = d[i];  // High-pass second
    memcpy(d, t, n * sizeof(double));
    free(t);
}

// CDF 9/7 inverse transform - reverses all lifting steps
static void Cdf97Inv(double* d, int n)
{
    if (n < 4) { HaarInv(d, n); return; }
    
    // Undo reordering: interleave low-pass and high-pass
    double* t = (double*)malloc(n * sizeof(double));
    int j = 0;
    for (int i = 0; i < n; i += 2) t[i] = d[j++];  // Even positions = low-pass
    for (int i = 1; i < n; i += 2) t[i] = d[j++];  // Odd positions = high-pass
    memcpy(d, t, n * sizeof(double));
    free(t);
    
    // Undo scaling
    for (int i = 0; i < n; i += 2) d[i] /= CDF_K;
    for (int i = 1; i < n; i += 2) d[i] *= CDF_K;
    
    // Undo lifting steps in reverse order
    for (int i = 0; i < n; i += 2)
        d[i] -= CDF_D * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    for (int i = 1; i < n; i += 2)
        d[i] -= CDF_C * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    for (int i = 0; i < n; i += 2)
        d[i] -= CDF_B * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
    
    for (int i = 1; i < n; i += 2)
        d[i] -= CDF_A * (d[Mir(i-1,n)] + d[Mir(i+1,n)]);
}

// =============================================================================
// 2D WAVELET TRANSFORMS
// 
// 2D transforms are implemented as separable 1D transforms:
// 1. Apply 1D transform to each row
// 2. Apply 1D transform to each column
// 
// This produces a 4-subband decomposition at each level:
//   LL (low-low)   | LH (low-high)     <- horizontal details
//   ----------------
//   HL (high-low)  | HH (high-high)    <- diagonal details
//        ^
//        vertical details
// 
// Multi-level decomposition recursively transforms the LL subband.
// =============================================================================
static void Fwd2D(double* d, int W, int H, int lvl, int type)
{
    double* row = (double*)malloc(W * sizeof(double));
    double* col = (double*)malloc(H * sizeof(double));
    int w = W, h = H;
    int minSz = (type == 0) ? 2 : 4;  // Minimum size depends on wavelet
    
    // Apply transform for each decomposition level
    for (int L = 0; L < lvl && w >= minSz && h >= minSz; L++) {
        // Transform rows (horizontal filtering)
        for (int y = 0; y < h; y++) {
            // Extract row
            for (int x = 0; x < w; x++) row[x] = d[y*W + x];
            
            // Apply 1D transform
            if (type == 0) HaarFwd(row, w);
            else if (type == 1) Db4Fwd(row, w);
            else Cdf97Fwd(row, w);
            
            // Store back
            for (int x = 0; x < w; x++) d[y*W + x] = row[x];
        }
        
        // Transform columns (vertical filtering)
        for (int x = 0; x < w; x++) {
            // Extract column
            for (int y = 0; y < h; y++) col[y] = d[y*W + x];
            
            // Apply 1D transform
            if (type == 0) HaarFwd(col, h);
            else if (type == 1) Db4Fwd(col, h);
            else Cdf97Fwd(col, h);
            
            // Store back
            for (int y = 0; y < h; y++) d[y*W + x] = col[y];
        }
        
        // Next level operates on LL subband (top-left quarter)
        w /= 2; h /= 2;
    }
    
    free(row); free(col);
}

// 2D inverse transform - processes levels in reverse order
static void Inv2D(double* d, int W, int H, int lvl, int type)
{
    double* row = (double*)malloc(W * sizeof(double));
    double* col = (double*)malloc(H * sizeof(double));
    
    // Calculate sizes at each level
    int sizes[16][2], actual = 0;
    int w = W, h = H;
    int minSz = (type == 0) ? 2 : 4;
    
    for (int i = 0; i < lvl && w >= minSz && h >= minSz; i++) {
        sizes[i][0] = w; sizes[i][1] = h;
        w /= 2; h /= 2; actual++;
    }
    
    // Reconstruct from coarsest to finest level
    for (int L = actual - 1; L >= 0; L--) {
        w = sizes[L][0]; h = sizes[L][1];
        
        // Inverse transform columns first (reverse of forward order)
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) col[y] = d[y*W + x];
            
            if (type == 0) HaarInv(col, h);
            else if (type == 1) Db4Inv(col, h);
            else Cdf97Inv(col, h);
            
            for (int y = 0; y < h; y++) d[y*W + x] = col[y];
        }
        
        // Then inverse transform rows
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) row[x] = d[y*W + x];
            
            if (type == 0) HaarInv(row, w);
            else if (type == 1) Db4Inv(row, w);
            else Cdf97Inv(row, w);
            
            for (int x = 0; x < w; x++) d[y*W + x] = row[x];
        }
    }
    
    free(row); free(col);
}

// =============================================================================
// QUANTIZATION
// 
// Quantization reduces the precision of coefficients to enable compression.
// This simulates what happens during save/load:
// 1. Find the range [min, max] of non-zero coefficients
// 2. Map each coefficient to an integer in [0, 2^bits - 1]
// 3. Map back to floating point (introduces quantization error)
// 
// Lower bit depths = more compression but more quality loss
// =============================================================================
static void ApplyQuantization(void)
{
    if (!g_coeffs || g_quantBits >= 32) return;
    
    // Find min/max of non-zero coefficients
    double minV = 1e30, maxV = -1e30;
    for (int i = 0; i < g_totalCoeffs; i++) {
        if (g_coeffs[i] != 0) {
            if (g_coeffs[i] < minV) minV = g_coeffs[i];
            if (g_coeffs[i] > maxV) maxV = g_coeffs[i];
        }
    }
    if (minV > maxV) return;  // All zeros
    
    double range = maxV - minV;
    if (range < 1e-10) return;
    
    unsigned long long maxQ = ((unsigned long long)1 << g_quantBits) - 1;
    
    // Quantize and dequantize each non-zero coefficient
    for (int i = 0; i < g_totalCoeffs; i++) {
        if (g_coeffs[i] != 0) {
            // Quantize: map to integer
            unsigned long long qval = (unsigned long long)(((g_coeffs[i] - minV) / range) * maxQ + 0.5);
            if (qval > maxQ) qval = maxQ;
            // Dequantize: map back to float (this introduces quantization error)
            g_coeffs[i] = minV + ((double)qval / maxQ) * range;
        }
    }
    
    // Update stored min/max for file saving
    g_coeffMin = minV;
    g_coeffMax = maxV;
}

// =============================================================================
// TRANSFORM OPERATIONS
// =============================================================================

// Structure for sorting coefficients by magnitude
typedef struct { double v; int i; } CoeffIdx;

// Comparison function for qsort
static int CmpCoeff(const void* a, const void* b) {
    double d = ((CoeffIdx*)a)->v - ((CoeffIdx*)b)->v;
    return (d < 0) ? -1 : (d > 0) ? 1 : 0;
}

// Calculate estimated file size based on current coefficients
static int CalcEstimatedSize(void)
{
    if (!g_coeffs || g_nonZero == 0) return 0;
    
    int headerSize = sizeof(WTHeader);
    int idxBytes = (g_totalCoeffs <= 65536) ? 2 : (g_totalCoeffs <= 16777216) ? 3 : 4;
    int valBytes = CalcValBytes(g_quantBits);
    int coeffSize = g_nonZero * (idxBytes + valBytes);
    
    return headerSize + coeffSize;
}

// Apply forward wavelet transform and coefficient thresholding
static void DoTransform(void)
{
    if (!g_original) return;
    
    int pixels = g_w * g_h;
    free(g_coeffs);
    g_coeffs = (double*)malloc(pixels * 3 * sizeof(double));
    g_totalCoeffs = pixels * 3;
    
    // Determine number of decomposition levels
    if (g_levels == 0) {
        // Auto: use maximum possible levels
        int m = (g_w < g_h) ? g_w : g_h;
        g_usedLevels = 0;
        int minSz = (g_waveletType == 0) ? 2 : 4;
        while (m >= minSz * 2 && g_usedLevels < 8) { m /= 2; g_usedLevels++; }
        if (g_usedLevels < 1) g_usedLevels = 1;
    } else {
        g_usedLevels = g_levels;
    }
    
    // Transform each color channel separately
    for (int c = 0; c < 3; c++) {
        double* ch = g_coeffs + c * pixels;
        // Copy pixel values to coefficient buffer
        for (int i = 0; i < pixels; i++) ch[i] = g_original[i*3 + c];
        // Apply 2D forward transform
        Fwd2D(ch, g_w, g_h, g_usedLevels, g_waveletType);
    }
    
    // Find coefficient range
    g_coeffMin = 1e30; g_coeffMax = -1e30;
    for (int i = 0; i < g_totalCoeffs; i++) {
        if (g_coeffs[i] < g_coeffMin) g_coeffMin = g_coeffs[i];
        if (g_coeffs[i] > g_coeffMax) g_coeffMax = g_coeffs[i];
    }
    
    // Discard smallest coefficients (thresholding for compression)
    if (g_discard > 0) {
        // Sort coefficients by absolute value
        CoeffIdx* s = (CoeffIdx*)malloc(g_totalCoeffs * sizeof(CoeffIdx));
        for (int i = 0; i < g_totalCoeffs; i++) {
            s[i].v = fabs(g_coeffs[i]);
            s[i].i = i;
        }
        qsort(s, g_totalCoeffs, sizeof(CoeffIdx), CmpCoeff);
        
        // Zero out the smallest coefficients
        long long nd = (long long)((g_discard / 100.0) * g_totalCoeffs);
        if (nd > g_totalCoeffs) nd = g_totalCoeffs;
        if (nd < 0) nd = 0;
        for (long long i = 0; i < nd; i++) g_coeffs[s[i].i] = 0;
        g_nonZero = g_totalCoeffs - (int)nd;
        free(s);
    } else {
        g_nonZero = g_totalCoeffs;
    }
    
    // Apply quantization to simulate what will be saved
    ApplyQuantization();
    
    g_transformed = TRUE;
    
    // Create visualization buffer
    free(g_waveletVis);
    g_waveletVis = (BYTE*)malloc(pixels * 3);
    UpdateVis();
    UpdateMenu();
}

// Reconstruct image from wavelet coefficients
static void DoReconstruct(void)
{
    if (!g_coeffs) return;
    
    int pixels = g_w * g_h;
    
    // Work on a copy to preserve original coefficients
    double* temp = (double*)malloc(g_totalCoeffs * sizeof(double));
    memcpy(temp, g_coeffs, g_totalCoeffs * sizeof(double));
    
    // Inverse transform each channel
    for (int c = 0; c < 3; c++) {
        double* ch = temp + c * pixels;
        Inv2D(ch, g_w, g_h, g_usedLevels, g_waveletType);
        
        // Convert back to bytes with clamping
        for (int i = 0; i < pixels; i++) {
            int v = (int)(ch[i] + 0.5);
            g_result[i*3 + c] = (BYTE)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    free(temp);
    
    // Calculate PSNR if original is available
    if (g_hasOriginal && g_original) {
        g_psnr = CalcPSNR();
    }
}

// Reset to original image
static void DoReset(void)
{
    if (!g_original) return;
    memcpy(g_result, g_original, g_w * g_h * 3);
    free(g_coeffs); g_coeffs = NULL;
    free(g_waveletVis); g_waveletVis = NULL;
    g_transformed = FALSE;
    g_psnr = 0;
    UpdateMenu();
}

// Update coefficient visualization
// Uses logarithmic scaling to show both large and small coefficients
static void UpdateVis(void)
{
    if (!g_coeffs || !g_waveletVis) return;
    int pixels = g_w * g_h;
    
    // Find maximum absolute value
    double maxAbs = 0;
    for (int i = 0; i < g_totalCoeffs; i++) {
        double a = fabs(g_coeffs[i]);
        if (a > maxAbs) maxAbs = a;
    }
    if (maxAbs < 1e-10) maxAbs = 1;
    
    // Logarithmic scale factor (higher = more contrast in details)
    double logScale = 2000.0;
    
    for (int c = 0; c < 3; c++) {
        double* ch = g_coeffs + c * pixels;
        for (int i = 0; i < pixels; i++) {
            double v = ch[i] / maxAbs;  // Normalize to [-1, 1]
            
            // Apply logarithmic scaling while preserving sign
            double sign = (v >= 0) ? 1.0 : -1.0;
            double logV = sign * log1p(fabs(v) * logScale) / log1p(logScale);
            
            // Map from [-1, 1] to [0, 255]
            g_waveletVis[i*3 + c] = (BYTE)((logV + 1.0) * 0.5 * 255);
        }
    }
}

// Calculate Peak Signal-to-Noise Ratio
// PSNR = 10 * log10(MAX^2 / MSE) where MAX=255 for 8-bit images
static double CalcPSNR(void)
{
    if (!g_original || !g_result) return 0;
    
    double mse = 0;
    int n = g_w * g_h * 3;
    
    // Calculate Mean Squared Error
    for (int i = 0; i < n; i++) {
        double d = (double)g_original[i] - (double)g_result[i];
        mse += d * d;
    }
    mse /= n;
    
    // PSNR formula (cap at 99.99 for perfect reconstruction)
    return (mse < 1e-10) ? 99.99 : 10.0 * log10(255.0 * 255.0 / mse);
}

// =============================================================================
// PAINTING / RENDERING
// =============================================================================

// Draw an RGB image to the device context
static void DrawImg(HDC hdc, BYTE* rgb, int x, int y, int tw, int th)
{
    if (!rgb) return;
    
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -g_h;  // Negative = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    
    // Convert RGB to BGR for Windows
    BYTE* bgr = (BYTE*)malloc(g_w * g_h * 3);
    for (int i = 0; i < g_w * g_h; i++) {
        bgr[i*3+0] = rgb[i*3+2];  // B
        bgr[i*3+1] = rgb[i*3+1];  // G
        bgr[i*3+2] = rgb[i*3+0];  // R
    }
    
    SetStretchBltMode(hdc, HALFTONE);  // High-quality scaling
    StretchDIBits(hdc, x, y, tw, th, 0, 0, g_w, g_h, bgr, &bi, DIB_RGB_COLORS, SRCCOPY);
    free(bgr);
}

// Main paint function - draws all images and labels
static void Paint(HDC hdc, RECT* rc)
{
    // Fill background
    HBRUSH bg = CreateSolidBrush(RGB(48, 48, 48));
    FillRect(hdc, rc, bg);
    DeleteObject(bg);
    
    // Show message if no image loaded
    if (!g_loaded) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(160, 160, 160));
        DrawTextW(hdc, L"Open a BMP or WT file", -1, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    
    // Determine how many images to display
    int numImages = 1;
    if (g_transformed && g_waveletVis) numImages = 2;
    if (g_hasOriginal && g_transformed) numImages = 3;
    
    // Calculate layout
    int clientW = rc->right - rc->left;
    int clientH = rc->bottom - rc->top;
    
    int margin = 10;
    int spacing = 15;
    int labelHeight = 40;  // Space for labels below images
    
    int totalSpacing = margin * 2 + spacing * (numImages - 1);
    int availableW = clientW - totalSpacing;
    int availableH = clientH - margin * 2 - labelHeight;
    
    if (availableW <= 0 || availableH <= 0) return;
    
    int maxImgW = availableW / numImages;
    int maxImgH = availableH;
    
    // Calculate scale to fit images
    double scaleX = (double)maxImgW / g_w;
    double scaleY = (double)maxImgH / g_h;
    double scale = (scaleX < scaleY) ? scaleX : scaleY;
    
    if (scale < 0.1) scale = 0.1;
    if (scale > 4.0) scale = 4.0;
    
    int drawW = (int)(g_w * scale);
    int drawH = (int)(g_h * scale);
    
    // Center images
    int totalW = drawW * numImages + spacing * (numImages - 1);
    int startX = (clientW - totalW) / 2;
    int startY = (clientH - drawH - labelHeight) / 2;
    
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_w;
    bmi.bmiHeader.biHeight = -g_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    // Setup text rendering
    SetBkMode(hdc, TRANSPARENT);
    HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    
    int currentX = startX;
    wchar_t label[256];
    
    // Draw original image (if available and transformed)
    if (g_hasOriginal && g_transformed && g_original) {
        // Convert RGB to BGR for Windows
        BYTE* tmp = (BYTE*)malloc(g_w * g_h * 3);
        for (int i = 0; i < g_w * g_h; i++) {
            tmp[i*3+0] = g_original[i*3+2];
            tmp[i*3+1] = g_original[i*3+1];
            tmp[i*3+2] = g_original[i*3+0];
        }
        StretchDIBits(hdc, currentX, startY, drawW, drawH,
            0, 0, g_w, g_h, tmp, &bmi, DIB_RGB_COLORS, SRCCOPY);
        free(tmp);
        
        // Draw labels
        SetTextColor(hdc, RGB(200, 200, 200));
        swprintf(label, 256, L"Original (%dx%d)", g_w, g_h);
        RECT labelRect = {currentX, startY + drawH + 5, currentX + drawW, startY + drawH + 20};
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_SINGLELINE);
        
        SetTextColor(hdc, RGB(140, 140, 140));
        swprintf(label, 256, L"Size: %d KB", g_bmpSize / 1024);
        RECT labelRect2 = {currentX, startY + drawH + 20, currentX + drawW, startY + drawH + 35};
        DrawTextW(hdc, label, -1, &labelRect2, DT_CENTER | DT_SINGLELINE);
        
        currentX += drawW + spacing;
    }
    
    // Draw wavelet coefficient visualization
    if (g_transformed && g_waveletVis) {
        BYTE* tmp = (BYTE*)malloc(g_w * g_h * 3);
        for (int i = 0; i < g_w * g_h; i++) {
            tmp[i*3+0] = g_waveletVis[i*3+2];
            tmp[i*3+1] = g_waveletVis[i*3+1];
            tmp[i*3+2] = g_waveletVis[i*3+0];
        }
        StretchDIBits(hdc, currentX, startY, drawW, drawH,
            0, 0, g_w, g_h, tmp, &bmi, DIB_RGB_COLORS, SRCCOPY);
        free(tmp);
        
        const wchar_t* wname = (g_waveletType == 0) ? L"Haar" : 
                               (g_waveletType == 1) ? L"Db4" : L"CDF 9/7";
        
        SetTextColor(hdc, RGB(200, 200, 200));
        swprintf(label, 256, L"Coefficients (%s, %d levels)", wname, g_usedLevels);
        RECT labelRect = {currentX, startY + drawH + 5, currentX + drawW, startY + drawH + 20};
        DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_SINGLELINE);
        
        SetTextColor(hdc, RGB(140, 140, 140));
        swprintf(label, 256, L"Non-zero: %d / %d (%.1f%%)", g_nonZero, g_totalCoeffs, 
            g_totalCoeffs > 0 ? (100.0 * g_nonZero / g_totalCoeffs) : 0);
        RECT labelRect2 = {currentX, startY + drawH + 20, currentX + drawW, startY + drawH + 35};
        DrawTextW(hdc, label, -1, &labelRect2, DT_CENTER | DT_SINGLELINE);
        
        currentX += drawW + spacing;
    }
    
    // Draw reconstructed/result image
    if (g_result) {
        BYTE* tmp = (BYTE*)malloc(g_w * g_h * 3);
        for (int i = 0; i < g_w * g_h; i++) {
            tmp[i*3+0] = g_result[i*3+2];
            tmp[i*3+1] = g_result[i*3+1];
            tmp[i*3+2] = g_result[i*3+0];
        }
        StretchDIBits(hdc, currentX, startY, drawW, drawH,
            0, 0, g_w, g_h, tmp, &bmi, DIB_RGB_COLORS, SRCCOPY);
        free(tmp);
        
        SetTextColor(hdc, RGB(200, 200, 200));
        if (g_transformed) {
            int estSize = CalcEstimatedSize();
            double ratio = estSize > 0 ? (double)g_bmpSize / estSize : 0;
            swprintf(label, 256, L"Reconstructed (%d-bit) - Est: %d KB", g_quantBits, estSize / 1024);
            RECT labelRect = {currentX, startY + drawH + 5, currentX + drawW, startY + drawH + 20};
            DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_SINGLELINE);
            
            SetTextColor(hdc, RGB(140, 140, 140));
            swprintf(label, 256, L"PSNR: %.2f dB | Ratio: %.1f:1", g_psnr, ratio);
            RECT labelRect2 = {currentX, startY + drawH + 20, currentX + drawW, startY + drawH + 35};
            DrawTextW(hdc, label, -1, &labelRect2, DT_CENTER | DT_SINGLELINE);
        } else {
            swprintf(label, 256, L"Image (%dx%d)", g_w, g_h);
            RECT labelRect = {currentX, startY + drawH + 5, currentX + drawW, startY + drawH + 20};
            DrawTextW(hdc, label, -1, &labelRect, DT_CENTER | DT_SINGLELINE);
        }
    }
    
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

// =============================================================================
// SETTINGS DIALOG
// Allows user to configure wavelet type, levels, discard percentage, and quantization
// =============================================================================
static HWND g_dlgWnd;
static HWND g_radioHaar, g_radioDb4, g_radioCdf;
static HWND g_comboLvl;
static HWND g_editDiscard;
static HWND g_sliderQuant, g_lblQuant;
static BOOL g_dlgOk;

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        // Wavelet type selection group
        CreateWindowW(L"BUTTON", L"Wavelet", WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            10, 5, 120, 90, hwnd, NULL, g_inst, NULL);
        g_radioHaar = CreateWindowW(L"BUTTON", L"Haar", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON|WS_GROUP,
            20, 22, 80, 18, hwnd, NULL, g_inst, NULL);
        g_radioDb4 = CreateWindowW(L"BUTTON", L"Daubechies-4", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
            20, 42, 100, 18, hwnd, NULL, g_inst, NULL);
        g_radioCdf = CreateWindowW(L"BUTTON", L"CDF 9/7", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
            20, 62, 80, 18, hwnd, NULL, g_inst, NULL);
        
        // Set current selection
        if (g_waveletType == 0) SendMessage(g_radioHaar, BM_SETCHECK, BST_CHECKED, 0);
        else if (g_waveletType == 1) SendMessage(g_radioDb4, BM_SETCHECK, BST_CHECKED, 0);
        else SendMessage(g_radioCdf, BM_SETCHECK, BST_CHECKED, 0);
        
        // Decomposition levels dropdown
        CreateWindowW(L"STATIC", L"Levels:", WS_CHILD|WS_VISIBLE, 145, 10, 50, 18, hwnd, NULL, g_inst, NULL);
        g_comboLvl = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            145, 28, 70, 200, hwnd, NULL, g_inst, NULL);
        SendMessageW(g_comboLvl, CB_ADDSTRING, 0, (LPARAM)L"Auto");
        for (int i = 1; i <= g_maxLevels; i++) {
            wchar_t b[8]; swprintf(b, 8, L"%d", i);
            SendMessageW(g_comboLvl, CB_ADDSTRING, 0, (LPARAM)b);
        }
        SendMessageW(g_comboLvl, CB_SETCURSEL, g_levels, 0);
        
        // Discard percentage input
        CreateWindowW(L"STATIC", L"Discard %:", WS_CHILD|WS_VISIBLE, 10, 105, 70, 18, hwnd, NULL, g_inst, NULL);
        g_editDiscard = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, 
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
            85, 102, 70, 22, hwnd, NULL, g_inst, NULL);
        wchar_t discardStr[32];
        swprintf(discardStr, 32, L"%.2f", g_discard);
        SetWindowTextW(g_editDiscard, discardStr);
        
        // Quantization bits slider
        wchar_t lb[48];
        swprintf(lb, 48, L"Quantization: %d bits", g_quantBits);
        g_lblQuant = CreateWindowW(L"STATIC", lb, WS_CHILD|WS_VISIBLE, 10, 135, 150, 18, hwnd, NULL, g_inst, NULL);
        g_sliderQuant = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD|WS_VISIBLE|TBS_HORZ,
            10, 153, 205, 25, hwnd, (HMENU)1002, g_inst, NULL);
        SendMessage(g_sliderQuant, TBM_SETRANGE, TRUE, MAKELPARAM(2, 32));
        SendMessage(g_sliderQuant, TBM_SETPOS, TRUE, g_quantBits);
        
        // OK and Cancel buttons
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,
            45, 190, 60, 26, hwnd, (HMENU)IDOK, g_inst, NULL);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD|WS_VISIBLE,
            115, 190, 60, 26, hwnd, (HMENU)IDCANCEL, g_inst, NULL);
        
        return 0;
    }
    
    case WM_HSCROLL: {
        // Update quantization label when slider moves
        if ((HWND)lp == g_sliderQuant) {
            int pos = (int)SendMessage(g_sliderQuant, TBM_GETPOS, 0, 0);
            wchar_t lb[48];
            swprintf(lb, 48, L"Quantization: %d bits", pos);
            SetWindowTextW(g_lblQuant, lb);
        }
        return 0;
    }
        
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            // Save settings
            if (SendMessage(g_radioHaar, BM_GETCHECK, 0, 0)) g_waveletType = 0;
            else if (SendMessage(g_radioDb4, BM_GETCHECK, 0, 0)) g_waveletType = 1;
            else g_waveletType = 2;
            
            g_levels = (int)SendMessage(g_comboLvl, CB_GETCURSEL, 0, 0);
            
            wchar_t discardStr[32];
            GetWindowTextW(g_editDiscard, discardStr, 32);
            double discardVal = _wtof(discardStr);
            if (discardVal < 0) discardVal = 0;
            if (discardVal > 99.999) discardVal = 99.999;
            g_discard = discardVal;
            
            g_quantBits = (int)SendMessage(g_sliderQuant, TBM_GETPOS, 0, 0);
            
            g_dlgOk = TRUE;
            DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDCANCEL) {
            g_dlgOk = FALSE;
            DestroyWindow(hwnd);
        }
        return 0;
        
    case WM_CLOSE:
        g_dlgOk = FALSE;
        DestroyWindow(hwnd);
        return 0;
        
    case WM_DESTROY:
        g_dlgWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Show the settings dialog and return TRUE if OK was clicked
static BOOL ShowDialog(HWND parent)
{
    // Register dialog window class (once)
    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.lpszClassName = L"WaveletDlg";
        RegisterClassW(&wc);
        reg = TRUE;
    }
    
    // Center dialog on parent window
    RECT pr; GetWindowRect(parent, &pr);
    int dw = 235, dh = 260;
    int x = pr.left + (pr.right - pr.left - dw) / 2;
    int y = pr.top + (pr.bottom - pr.top - dh) / 2;
    
    g_dlgOk = FALSE;
    g_dlgWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"WaveletDlg", L"Transform Settings",
        WS_POPUP|WS_CAPTION|WS_SYSMENU, x, y, dw, dh, parent, NULL, g_inst, NULL);
    
    ShowWindow(g_dlgWnd, SW_SHOW);
    EnableWindow(parent, FALSE);  // Disable parent (modal behavior)
    
    // Modal message loop
    MSG msg;
    while (g_dlgWnd && GetMessage(&msg, NULL, 0, 0)) {
        // Handle Tab key for focus navigation
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
            SetFocus(GetNextDlgTabItem(g_dlgWnd, GetFocus(), GetKeyState(VK_SHIFT) < 0));
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    EnableWindow(parent, TRUE);  // Re-enable parent
    SetForegroundWindow(parent);
    return g_dlgOk;
}
