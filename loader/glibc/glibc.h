// glibc ABI on top of Darwin libc, for glibc-built ARM64 Linux guests. Plugs into
// Klepton's kl_shim_override, so it wins over Klepton's bionic table for any name
// it answers; everything else falls through to Klepton.
#ifndef MVR_GLIBC_H
#define MVR_GLIBC_H
void  mvr_glibc_init(void);
void *mvr_glibc_lookup(const char *name);
extern void *(*mvr_glibc_extra)(const char *name);   // consulted first, when set
void *mvr_glibc_ext_lookup(const char *name);
void  mvr_glibc_set_exe(const char *path);
int   mvr_errno_to_linux(int e);
int   mvr_errno_to_darwin(int e);
#endif
