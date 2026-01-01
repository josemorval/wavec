🌊 Wavec — Wavelet Image Codec

Wavec is an experimental wavelet-based image compression tool built as a vibe coding project — exploratory, educational, and made for fun.

🧪 This is not a production-ready codec. It’s a playground for learning and experimenting with wavelet transforms.

📸 Screenshot

✨ Features

Wavec is a Windows desktop application that can:

Load 24-bit BMP images

Apply 2D Discrete Wavelet Transform (DWT) using:

Haar

Daubechies-4 (Db4)

CDF 9/7

Discard small coefficients for lossy compression

Quantize remaining coefficients

Save to a custom .WT sparse format

Reconstruct images using the Inverse DWT

Export reconstructed images as BMP

⚠️ Image Requirements

Use BMP images with dimensions that are powers of two, for example:

256 × 256

512 × 512

1024 × 1024

512 × 256

This ensures optimal wavelet decomposition across all levels.
Non–power-of-two dimensions may work, but can produce unexpected results or artifacts.

🛠️ Building
Requirements

Windows 10 / 11

Visual Studio 2022 (Community edition works fine)

Windows SDK

Automatic Build

compile.bat

Manual Build

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
rc wavec.rc
cl wavec.cpp wavec.res user32.lib gdi32.lib comdlg32.lib comctl32.lib /Fe:wavec.exe /O2

▶️ Usage

Open BMP
File > Open BMP... or Ctrl + O

Transform
Process > Transform... or F5

Configure:

Wavelet type (Haar, Db4, CDF 9/7)

Decomposition levels (Auto recommended)

Discard percentage (higher = more compression, more loss)

Quantization bits (lower = smaller file, more artifacts)

Save:

File > Save WT... — save compressed format

File > Save BMP... — export reconstructed image

Reset:

F7 restores the original image

⌨️ Keyboard Shortcuts

Key Action
F5 Transform
F7 Reset
Ctrl+O Open BMP
Ctrl+S Save BMP

📦 The .WT Format

A custom sparse format that stores only non-zero wavelet coefficients as:

(index, quantized_value)

It includes all metadata required for reconstruction:

Image dimensions

Wavelet type and decomposition levels

Quantization parameters

Coefficient value range for dequantization

🌊 Wavelets Implemented

Wavelet Description
Haar Simplest wavelet, based on averages and differences
Daubechies-4 4-tap orthogonal wavelet with better frequency localization
CDF 9/7 Biorthogonal wavelet used in JPEG2000, implemented via lifting scheme

⚠️ Disclaimer

This is a vibe coding project — experimental, exploratory, and not rigorously tested.

It exists for learning, experimentation, and having fun with wavelet transforms.

Use at your own risk.

📚 References

Haar Wavelet
Daubechies Wavelets
CDF 9/7 Wavelet
Lifting Scheme

📜 License

Do whatever you want with it. 🤷
