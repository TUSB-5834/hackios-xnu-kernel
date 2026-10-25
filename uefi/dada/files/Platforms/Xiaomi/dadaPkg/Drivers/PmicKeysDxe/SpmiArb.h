/** @file
  Minimal read/write access to the Qualcomm SPMI PMIC Arbiter.

  Register layout follows Linux drivers/spmi/spmi-pmic-arb.c (v1..v7).
**/

#ifndef __SPMI_ARB_H__
#define __SPMI_ARB_H__

#include <Uefi.h>

#define SPMI_CORE_BASE    0x0C400000ULL  /* "core"    (DT reg 0xc400000 0x3000) */
#define SPMI_CORE_SIZE    0x3000
#define SPMI_CNFG_BASE    0x0C42D000ULL  /* "cnfg"    (DT reg 0xc42d000 0x4000) */
#define SPMI_OBSRVR_BASE  0x0C440000ULL  /* "obsrvr"  (DT reg 0xc440000 0x80000) */
#define SPMI_INTR_BASE    0x0C4C0000ULL  /* "intr"    (DT reg 0xc4c0000 0x10000) */
#define SPMI_CHNLS_BASE   0x0C500000ULL  /* "chnls"   (DT reg 0xc500000 0x400000) */

#define SPMI_BUS_EE       0              /* DT qcom,ee */
#define SPMI_BUS_CHANNEL  0              /* DT qcom,channel */

/* Channel (per transaction) registers */
#define PMIC_ARB_CMD      0x00
#define PMIC_ARB_CONFIG   0x04
#define PMIC_ARB_STATUS   0x08
#define PMIC_ARB_WDATA0   0x10
#define PMIC_ARB_WDATA1   0x14
#define PMIC_ARB_RDATA0   0x18
#define PMIC_ARB_RDATA1   0x1C

#define PMIC_ARB_STATUS_DONE    BIT0
#define PMIC_ARB_STATUS_FAILURE BIT1
#define PMIC_ARB_STATUS_DENIED  BIT2
#define PMIC_ARB_STATUS_DROPPED BIT3

#define PMIC_ARB_OP_EXT_READL   1

#define PMIC_ARB_VERSION_V2_MIN 0x20010000U
#define PMIC_ARB_VERSION_V3_MIN 0x30000000U
#define PMIC_ARB_VERSION_V5_MIN 0x50000000U
#define PMIC_ARB_VERSION_V7_MIN 0x70000000U

#define PMIC_ARB_APID_MASK      0xFFFU

/**
  Probe the arbiter version and compute channel bases.

  @retval EFI_SUCCESS    Version recognised (v1..v7).
  @retval EFI_UNSUPPORTED Unexpected version register value.
**/
EFI_STATUS
SpmiArbInit (
  VOID
  );

UINT32
SpmiArbGetVersion (
  VOID
  );

/**
  Translate a peripheral (SID, PERIPH_ID) pair into its APID.

  @return APID (>=0) or -1 when the peripheral is not mapped.
**/
INT32
SpmiPpidToApId (
  IN UINT8  Sid,
  IN UINT8  PeriphId
  );

/**
  Read Len (1..4) bytes from SPMI address Addr of slave Sid.
**/
EFI_STATUS
SpmiRead (
  IN  UINT8   Sid,
  IN  UINT16  Addr,
  OUT UINT8   *Buffer,
  IN  UINTN   Len
  );

/**
  Write Len (1..4) bytes to SPMI address Addr of slave Sid.
**/
EFI_STATUS
SpmiWrite (
  IN UINT8   Sid,
  IN UINT16  Addr,
  IN UINT8   *Buffer,
  IN UINTN   Len
  );

EFI_STATUS
SpmiRead8 (
  IN  UINT8   Sid,
  IN  UINT16  Addr,
  OUT UINT8   *Value
  );

EFI_STATUS
SpmiWrite8 (
  IN UINT8  Sid,
  IN UINT16 Addr,
  IN UINT8  Value
  );

#endif
