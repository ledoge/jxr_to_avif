## [Download latest release](https://github.com/ledoge/jxr_to_avif/releases/latest/download/release.zip)

# About
This is a simple command line tool for converting HDR JPEG-XR files, such as Windows HDR screenshots, to AVIF.

The output format is hardcoded to be 12 bit 4:4:4 for maximum quality. Unfortunately, these files cannot be decoded natively by Windows's AV1 extension, as it only seems to do 8 bit up to 4:4:4 or 10/12 bit up to 4:2:0. However, the files open fine in Chromium.

# Usage
```
jxr_to_avif input.jxr [output.avif]
```
