// The arm64 ID registers a Linux kernel would let FEX read, rebuilt from Darwin's
// hw.optional.arm.FEAT_* (Darwin traps the mrs itself). Field layouts from the Arm ARM.
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>

static int feat( const char *name )
{
    char key[96]; int v = 0; size_t n = sizeof v;
    snprintf( key, sizeof key, "hw.optional.arm.%s", name );
    return sysctlbyname( key, &v, &n, NULL, 0 ) ? 0 : v;
}

#define F(reg, shift, val) (reg |= (uint64_t)(val) << (shift))

void inv_cpu_feature_registers( char *out, size_t cap )
{
    uint64_t isar0 = 0, isar1 = 0, isar2 = 0, pfr0 = 0x11, pfr1 = 0, mmfr1 = 0, mmfr2 = 0, dczid = 0;
    F( isar0, 4, feat( "FEAT_PMULL" ) ? 2 : feat( "FEAT_AES" ) );
    F( isar0, 8, feat( "FEAT_SHA1" ) );
    F( isar0, 12, feat( "FEAT_SHA512" ) ? 2 : feat( "FEAT_SHA256" ) );
    F( isar0, 16, feat( "FEAT_CRC32" ) );
    F( isar0, 20, feat( "FEAT_LSE" ) ? 2 : 0 );
    F( isar0, 28, feat( "FEAT_RDM" ) );
    F( isar0, 32, feat( "FEAT_SHA3" ) );
    F( isar0, 44, feat( "FEAT_DotProd" ) );
    F( isar0, 48, feat( "FEAT_FHM" ) );
    F( isar0, 52, feat( "FEAT_FlagM2" ) ? 2 : feat( "FEAT_FlagM" ) );
    F( isar1, 0, feat( "FEAT_DPB2" ) ? 2 : feat( "FEAT_DPB" ) );
    F( isar1, 12, feat( "FEAT_JSCVT" ) );
    F( isar1, 16, feat( "FEAT_FCMA" ) );
    F( isar1, 20, feat( "FEAT_LRCPC2" ) ? 2 : feat( "FEAT_LRCPC" ) );
    F( isar1, 32, feat( "FEAT_FRINTTS" ) );
    F( isar1, 36, feat( "FEAT_SB" ) );
    F( isar1, 40, feat( "FEAT_SPECRES" ) );
    F( isar1, 44, feat( "FEAT_EBF16" ) ? 2 : feat( "FEAT_BF16" ) );
    F( isar1, 52, feat( "FEAT_I8MM" ) );
    F( isar2, 0, feat( "FEAT_WFxT" ) ? 2 : 0 );
    F( isar2, 4, feat( "FEAT_RPRES" ) );
    F( isar2, 20, feat( "FEAT_HBC" ) );
    F( isar2, 52, feat( "FEAT_CSSC" ) );
    F( pfr0, 16, feat( "FEAT_FP16" ) );        // FP
    F( pfr0, 20, feat( "FEAT_FP16" ) );        // AdvSIMD
    F( pfr0, 48, feat( "FEAT_DIT" ) );
    F( pfr0, 56, feat( "FEAT_CSV2" ) );
    F( pfr0, 60, feat( "FEAT_CSV3" ) );
    F( pfr1, 0, feat( "FEAT_BTI" ) );
    F( pfr1, 4, feat( "FEAT_SSBS" ) ? 2 : 0 );
    // no SME: FEX would run streaming-mode code that Darwin hasn't set up for the thread
    F( mmfr1, 44, feat( "FEAT_AFP" ) );
    F( mmfr2, 32, feat( "FEAT_LSE2" ) );        // AT: unaligned atomics within 16 bytes
    __asm__ volatile( "mrs %0, dczid_el0" : "=r"(dczid) );   // the one Darwin lets EL0 read
    snprintf( out, cap, "isar0=%llx,isar1=%llx,isar2=%llx,pfr0=%llx,pfr1=%llx,mmfr1=%llx,mmfr2=%llx,dczid=%llx,midr=610f0000",
              (unsigned long long)isar0, (unsigned long long)isar1, (unsigned long long)isar2,
              (unsigned long long)pfr0, (unsigned long long)pfr1, (unsigned long long)mmfr1,
              (unsigned long long)mmfr2, (unsigned long long)dczid );
}
