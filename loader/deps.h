#ifndef MVR_DEPS_H
#define MVR_DEPS_H
void  mvr_deps_init(const char *exe_path);     // search dirs: exe dir, exe/lib, $MVR_LIBPATH
void *mvr_hostlib_lookup(const char *name);    // SDL2 / OpenGL served by macOS
void  mvr_steam_init(const char *appid);       // steam.c: Linux steam_api -> the macOS Steam client
#endif
