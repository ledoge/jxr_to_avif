## [Download latest release](https://github.com/ledoge/jxr_to_avif/releases/latest/download/release.zip)

# About
This is a simple command line tool for converting HDR JPEG-XR files, such as Windows HDR screenshots, to AVIF.

The output format is hardcoded to be 12 bit 4:4:4 for maximum quality. Unfortunately, these files cannot be decoded natively by Windows's AV1 extension, as it only seems to do 8 bit up to 4:4:4 or 10/12 bit up to 4:2:0. However, the files open fine in Chromium.

# Usage
```
jxr_to_avif input.jxr [output.avif]
```

# HDR metadata
The maxCLL value is calculated almost identically to [HDR + WCG Image Viewer](https://github.com/13thsymphony/HDRImageViewer) by taking the luminance of the 99.99 percentile brightest pixel. This is an underestimate of the "real" maxCLL value calculated according to H.274, so it technically causes some clipping when tone mapping. However, following the spec can lead to a much higher maxCLL value, which causes e.g. Chromium's tone mapping to significantly dim the entire image, so this trade-off seems to be worth it.
