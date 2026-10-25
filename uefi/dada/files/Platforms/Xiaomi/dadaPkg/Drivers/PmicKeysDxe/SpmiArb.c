/** @file
  Minimal read/write access to the Qualcomm SPMI PMIC Arbiter (v1..v7).

  Sequencing follows Linux drivers/spmi/spmi-pmic-arb.c:
   - reads go through the per-APID "observer" channel (rd_base),
   - writes go through the per-APID read/write channel (wr_base).
  Only short (<=4 byte) accesses are implemented.
**/

#include <Base.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "SpmiArb.h"

#define SPMI_CACHE_ENTRIES 8
#define SPMI_APID_INVALID  (-1)

STATIC UINT32 mArbVersion;
STATIC UINT8  mArbClass;      /* 1, 2, 3, 5 or 7 */
STATIC UINT64 mRdBase;
STATIC UINT64 mWrBase;

STATIC struct {
  UINT16 Ppid;
  INT32  Apid;
} mApIdCache[SPMI_CACHE_ENTRIES];
STATIC UINTN mApIdCacheCount;

UINT32
SpmiArbGetVersion (
  VOID
  )
{
  return mArbVersion;
}

STATIC
UINT32
SpmiApIdMapOffset (
  IN UINT32  Index
  )
{
  switch (mArbClass) {
    case 2:
    case 3:
      return 0x800 + 4 * Index;
    case 5:
      return 0x900 + 4 * Index;
    case 7:
      return 0x2000 + 4 * Index;
    default:
      return 0;
  }
}

STATIC
UINT32
SpmiApIdOwnerOffset (
  IN UINT32  ApId
  )
{
  if (mArbClass == 7) {
    /* v7: cnfg + 4 * (apid - base_apid), base_apid == 0 for bus 0 */
    return 4 * ApId;
  }

  /* v2/v3/v5: cnfg + 0x700 + 4 * apid */
  return 0x700 + 4 * ApId;
}

/**
  Channel offset for a given APID.

  @param ApId   APID (ignored for arbiter v1).
  @param IsRead TRUE for the observer channel, FALSE for read/write.
**/
STATIC
UINT32
SpmiChannelOffset (
  IN INT32   ApId,
  IN BOOLEAN IsRead
  )
{
  if (mArbClass == 1) {
    return 0x800 + 0x80 * SPMI_BUS_CHANNEL;
  }

  switch (mArbClass) {
    case 2:
    case 3:
      return 0x1000 * SPMI_BUS_EE + 0x8000 * (UINT32)ApId;
    case 5:
      if (IsRead) {
        return 0x10000 * SPMI_BUS_EE + 0x80 * (UINT32)ApId;
      }
      return 0x10000 * (UINT32)ApId;
    case 7:
      if (IsRead) {
        return 0x8000 * SPMI_BUS_EE + 0x20 * (UINT32)ApId;
      }
      return 0x1000 * (UINT32)ApId;
    default:
      return 0;
  }
}

STATIC
UINT32
SpmiFmtReadCmd (
  IN UINT8   Sid,
  IN UINT16  Addr,
  IN UINT8   ByteCountMinusOne
  )
{
  UINT32 Cmd = ((UINT32)PMIC_ARB_OP_EXT_READL) << 27;

  if (mArbClass == 1) {
    Cmd |= ((UINT32)(Sid & 0xF)) << 20;
    Cmd |= ((UINT32)Addr) << 4;
  } else {
    Cmd |= ((UINT32)(Addr & 0xFF)) << 4;
  }

  Cmd |= ByteCountMinusOne & 0x7;
  return Cmd;
}

STATIC
UINT32
SpmiFmtWriteCmd (
  IN UINT8   Sid,
  IN UINT16  Addr,
  IN UINT8   ByteCountMinusOne
  )
{
  /* opcode 0 == PMIC_ARB_OP_EXT_WRITEL */
  UINT32 Cmd = 0U;

  if (mArbClass == 1) {
    Cmd |= ((UINT32)(Sid & 0xF)) << 20;
    Cmd |= ((UINT32)Addr) << 4;
  } else {
    Cmd |= ((UINT32)(Addr & 0xFF)) << 4;
  }

  Cmd |= ByteCountMinusOne & 0x7;
  return Cmd;
}

EFI_STATUS
SpmiArbInit (
  VOID
  )
{
  mArbVersion = MmioRead32 ((UINTN)SPMI_CORE_BASE);

  if (mArbVersion < PMIC_ARB_VERSION_V2_MIN) {
    mArbClass = 1;
    mRdBase   = SPMI_CORE_BASE;
    mWrBase   = SPMI_CORE_BASE;
  } else if (mArbVersion < PMIC_ARB_VERSION_V3_MIN) {
    mArbClass = 2;
    mRdBase   = SPMI_OBSRVR_BASE;
    mWrBase   = SPMI_CHNLS_BASE;
  } else if (mArbVersion < PMIC_ARB_VERSION_V5_MIN) {
    mArbClass = 3;
    mRdBase   = SPMI_OBSRVR_BASE;
    mWrBase   = SPMI_CHNLS_BASE;
  } else if (mArbVersion < PMIC_ARB_VERSION_V7_MIN) {
    mArbClass = 5;
    mRdBase   = SPMI_OBSRVR_BASE;
    mWrBase   = SPMI_CHNLS_BASE;
  } else {
    mArbClass = 7;
    mRdBase   = SPMI_OBSRVR_BASE;
    mWrBase   = SPMI_CHNLS_BASE;
  }

  mApIdCacheCount = 0;

  DEBUG ((
    DEBUG_ERROR,
    "SpmiArb: version 0x%x -> class v%u rd=0x%lx wr=0x%lx\n",
    mArbVersion,
    mArbClass,
    mRdBase,
    mWrBase
    ));

  if (mArbClass == 1) {
    return EFI_SUCCESS;
  }

  if (SpmiApIdMapOffset (0) == 0) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

INT32
SpmiPpidToApId (
  IN UINT8  Sid,
  IN UINT8  PeriphId
  )
{
  UINT16 Ppid = ((UINT16)Sid << 8) | PeriphId;
  UINTN  Index;

  if (mArbClass == 1) {
    return 0;
  }

  for (Index = 0; Index < mApIdCacheCount; Index++) {
    if (mApIdCache[Index].Ppid == Ppid) {
      return mApIdCache[Index].Apid;
    }
  }

  for (Index = 0; (SpmiApIdMapOffset ((UINT32)Index) + 4) <= SPMI_CORE_SIZE; Index++) {
    UINT32 Reg = MmioRead32 ((UINTN)(SPMI_CORE_BASE + SpmiApIdMapOffset ((UINT32)Index)));

    if (Reg == 0) {
      continue;
    }

    if (((Reg >> 8) & PMIC_ARB_APID_MASK) == Ppid) {
      INT32 Apid = (INT32)Index;

      if (mApIdCacheCount < SPMI_CACHE_ENTRIES) {
        mApIdCache[mApIdCacheCount].Ppid = Ppid;
        mApIdCache[mApIdCacheCount].Apid = Apid;
        mApIdCacheCount++;
      }

      return Apid;
    }
  }

  return SPMI_APID_INVALID;
}

STATIC
BOOLEAN
SpmiIsWriteOwner (
  IN INT32  ApId
  )
{
  UINT32 Reg;

  if (mArbClass < 5) {
    /* Hardware enforces ownership for older arbiters (STATUS.DENIED). */
    return TRUE;
  }

  Reg = MmioRead32 ((UINTN)(SPMI_CNFG_BASE + SpmiApIdOwnerOffset ((UINT32)ApId)));

  /* SPMI_OWNERSHIP_PERIPH2OWNER: bits 2:0 hold the owning EE */
  return ((Reg & 0x7) == SPMI_BUS_EE);
}

STATIC
EFI_STATUS
SpmiWaitForDone (
  IN UINT64  Base,
  IN UINT32  Offset
  )
{
  UINT32 Status = 0;
  UINTN  Retry;
  UINTN  Stall;

  for (Stall = 0; Stall < 200; Stall++) {
    for (Retry = 0; Retry < 512; Retry++) {
      Status = MmioRead32 ((UINTN)(Base + Offset + PMIC_ARB_STATUS));

      if ((Status & PMIC_ARB_STATUS_DONE) != 0) {
        if ((Status & PMIC_ARB_STATUS_DENIED) != 0) {
          return EFI_ACCESS_DENIED;
        }

        if ((Status & (PMIC_ARB_STATUS_FAILURE | PMIC_ARB_STATUS_DROPPED)) != 0) {
          return EFI_DEVICE_ERROR;
        }

        return EFI_SUCCESS;
      }
    }

    gBS->Stall (5);
  }

  DEBUG ((DEBUG_ERROR, "SpmiArb: timeout, status 0x%x at 0x%lx\n", Status, Base + Offset + PMIC_ARB_STATUS));
  return EFI_TIMEOUT;
}

EFI_STATUS
SpmiRead (
  IN  UINT8   Sid,
  IN  UINT16  Addr,
  OUT UINT8   *Buffer,
  IN  UINTN   Len
  )
{
  INT32      Apid;
  UINT32     Offset;
  UINT32     Data = 0;
  EFI_STATUS Status;

  if ((Buffer == NULL) || (Len == 0) || (Len > 4)) {
    return EFI_INVALID_PARAMETER;
  }

  Apid = SpmiPpidToApId (Sid, (UINT8)(Addr >> 8));
  if (Apid < 0) {
    return EFI_NOT_FOUND;
  }

  Offset = SpmiChannelOffset (Apid, TRUE);

  MmioWrite32 (
    (UINTN)(mRdBase + Offset + PMIC_ARB_CMD),
    SpmiFmtReadCmd (Sid, Addr, (UINT8)(Len - 1))
    );

  Status = SpmiWaitForDone (mRdBase, Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Data = MmioRead32 ((UINTN)(mRdBase + Offset + PMIC_ARB_RDATA0));
  CopyMem (Buffer, &Data, Len);
  return EFI_SUCCESS;
}

EFI_STATUS
SpmiWrite (
  IN UINT8   Sid,
  IN UINT16  Addr,
  IN UINT8   *Buffer,
  IN UINTN   Len
  )
{
  INT32      Apid;
  UINT32     Offset;
  UINT32     Data = 0;

  if ((Buffer == NULL) || (Len == 0) || (Len > 4)) {
    return EFI_INVALID_PARAMETER;
  }

  Apid = SpmiPpidToApId (Sid, (UINT8)(Addr >> 8));
  if (Apid < 0) {
    return EFI_NOT_FOUND;
  }

  if (!SpmiIsWriteOwner (Apid)) {
    return EFI_ACCESS_DENIED;
  }

  Offset = SpmiChannelOffset (Apid, FALSE);

  CopyMem (&Data, Buffer, Len);
  MmioWrite32 ((UINTN)(mWrBase + Offset + PMIC_ARB_WDATA0), Data);
  MmioWrite32 (
    (UINTN)(mWrBase + Offset + PMIC_ARB_CMD),
    SpmiFmtWriteCmd (Sid, Addr, (UINT8)(Len - 1))
    );

  return SpmiWaitForDone (mWrBase, Offset);
}

EFI_STATUS
SpmiRead8 (
  IN  UINT8   Sid,
  IN  UINT16  Addr,
  OUT UINT8   *Value
  )
{
  return SpmiRead (Sid, Addr, Value, 1);
}

EFI_STATUS
SpmiWrite8 (
  IN UINT8  Sid,
  IN UINT16 Addr,
  IN UINT8  Value
  )
{
  return SpmiWrite (Sid, Addr, &Value, 1);
}
