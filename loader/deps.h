#ifndef MVR_DEPS_H
#define MVR_DEPS_H
void  mvr_deps_init(const char *exe_path);     // search dirs: exe dir, exe/lib, $MVR_LIBPATH
void *mvr_hostlib_lookup(const char *name);    // SDL2 / OpenGL served by macOS
#endif
