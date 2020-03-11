![C/C++ CI](https://github.com/x1b6e6/qdl/workflows/C/C++%20CI/badge.svg?branch=master)

Qualcomm Download
=================

This tool communicates with USB devices of id 05c6:9008 to upload a flash
loader and use this to flash images.

Usage:
```bash
qdl <prog.mbn> [<program> <patch> ...]
```

Building
========
In order to build the project you need `libxml2` headers and libraries, found in
e.g. the `libxml2-dev` package.

With this installed run:
```
make
```
