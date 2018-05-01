Galogen
=======

Galogen generates code to load OpenGL entry points  for the exact API version, 
profile and extensions that you specify.

Usage:
  galogen <path to GL registry XML file> [options]

  --api - API name, such as gl or gles2. Default is gl.
Options:
  --ver - API version. Default is 4.0.
  --profile - Which API profile to generate the loader for. Allowed values are "core" and "compatibility". Default is "core".
  --exts - A comma-separated list of extensions. Default is empty. 
  --filename - Name for generated files (<api>_<ver>_<profile> by default). 
  --generator - Which generator to use. Default is "c_noload". 

Example:
  ./galogen gl.xml --api gl --ver 4.5 --profile core --filename gl

Disclaimer
==========

This is not an official Google product (experimental or otherwise), it is just
code that happens to be owned by Google.

