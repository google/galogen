Galogen
=======

Galogen generates code to load OpenGL entry points  for the exact API version, 
profile and extensions that you specify.

Please visit http://galogen.gpfault.net/ for more info.

Usage:

  `galogen <path to GL registry XML file> [options]`

Options:

*  `--api` - should be "gl" for OpenGL, "gles1" for OpenGL ES 1, or "gles2" for OpenGL ES 2 or 3.
*  `--ver` - version of the API, e.g. 4.5 for OpenGL 4.5. Default is 4.0 for desktop OpenGL, 2.0 for OpenGL ES.
*  `--profile` - which API profile to use. Set to "core" for core profile, "compatibility" for compatibility profile. Default is "compatibility".
*  `--exts` - comma-separated list of extensions to add. Default is empty. 
*  `--filename` - name for generated file(s). Default is "gl".

Example:

`  ./galogen gl.xml --api gl --ver 4.5 --profile core --filename gl_core_45`

Disclaimer
==========

This is not an official Google product (experimental or otherwise), it is just
code that happens to be owned by Google.
