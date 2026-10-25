/** @file
  SM8750 volume/power key driver for Mu-Silicium (Xiaomi 15 / dada).

  The keys are monitored by the PMIC (not the SoC TLMM):
    - power  : PON peripheral, IRQ KPDPWR_N  (RT_STS bit 7)
    - volume-: PON peripheral, IRQ RESIN_N   (RT_STS bit 6)
    - volume+: PM8550 SPMI GPIO 6            (RT_STS bit 0, active low)

  Key state is polled over the SPMI PMIC Arbiter and reported through
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL / EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL:

    volume+ -> SCAN_UP    (Boot Manager entry + menu navigation)
    volume- -> SCAN_DOWN  (menu navigation)
    power   -> Enter      (selection)
    volume- held + power  -> SCAN_ESC (back)

  A vendor device path (EFI_KEYPAD_DEVICE_GUID) is installed on the same
  handle so ConPlatform/ConSplitter (ConIn) and BootManagerLib's
  LocateDevicePath (KeyCallback) both find this driver.

  Ported from edk2-msm Silicon/Qualcomm/sm8750/Drivers/PmicKeysDxe.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#include "SpmiArb.h"

#define PMIC_KEYS_TRACE 0

/* Polling: 10 ms tick */
#define KEY_POLL_INTERVAL_100NS  (10 * 10000)
#define KEY_DEBOUNCE_TICKS       3       /* 30 ms */
#define KEY_REPEAT_DELAY_TICKS   40      /* 400 ms */
#define KEY_REPEAT_RATE_TICKS    10      /* 100 ms */

/* PMK8550/PM8010 PON peripheral (DT pon_hlos@1300), SID candidates: the
 * upstream pmk8550.dtsi puts it on SID 0 while this board's DT nests it
 * under qcom,pm8010@c (SID 0x0c); the SID whose PPID maps wins. */
#define PON_PERIPH_BASE          0x1300
#define PON_IRQ_RT_STS           0x10    /* QPNPINT_REG_RT_STS */
#define PON_IRQ_KPDPWR           7
#define PON_IRQ_RESIN            6
#define PON_PERIPH_ID            0x13

STATIC CONST UINT8 mPonSidCandidates[] = { 0x00, 0x0C };

/* PM8550 gpio@8800 (SID 1). volume_up uses "gpio6": index = 6 - 1 = 5,
 * each pad occupies PMIC_GPIO_ADDRESS_RANGE (0x100). */
#define SPMI_SID_PM8550          0x01
#define PMIC_GPIO_PERIPH_BASE    0x8800
#define PMIC_GPIO_VOLUP_INDEX    5
#define PMIC_GPIO_ADDRESS_RANGE  0x100
#define PMIC_GPIO_REG_TYPE       0x04
#define PMIC_GPIO_REG_RT_STS     0x10
#define PMIC_GPIO_REG_MODE_CTL   0x40
#define PMIC_GPIO_REG_EN_CTL     0x46
#define PMIC_GPIO_TYPE           0x10
#define PMIC_GPIO_MODE_DIR_SHIFT 4
#define PMIC_GPIO_MODE_DIR_MASK  0x7
#define PMIC_GPIO_DIR_INPUT      0
#define PMIC_GPIO_MASTER_EN_BIT  0x80

#define PON_RT_STS_ADDR          (PON_PERIPH_BASE + PON_IRQ_RT_STS)
#define VOLUP_BASE               (PMIC_GPIO_PERIPH_BASE + PMIC_GPIO_VOLUP_INDEX * PMIC_GPIO_ADDRESS_RANGE)
#define VOLUP_RT_STS_ADDR        (VOLUP_BASE + PMIC_GPIO_REG_RT_STS)
#define VOLUP_TYPE_ADDR          (VOLUP_BASE + PMIC_GPIO_REG_TYPE)
#define VOLUP_MODE_CTL_ADDR      (VOLUP_BASE + PMIC_GPIO_REG_MODE_CTL)
#define VOLUP_EN_CTL_ADDR        (VOLUP_BASE + PMIC_GPIO_REG_EN_CTL)

#define KEY_COUNT                3
#define KEY_PWR                  0
#define KEY_VOLM                 1
#define KEY_VOLP                 2
#define KEY_BIT(Index)           (1U << (Index))

#define KEY_QUEUE_MAX            8
#define NOTIFY_QUEUE_MAX         8

/* EFI_KEYPAD_DEVICE_GUID - same as SiliciumPkg Include/Configuration/BootDevices.h */
#define EFI_KEYPAD_DEVICE_GUID                                       \
  {                                                                  \
    0xD7F58A0E, 0xBED2, 0x4B5A,                                      \
    { 0xBB, 0x43, 0x8A, 0xB2, 0x3D, 0xD0, 0xE2, 0xB0 }               \
  }

typedef struct {
  VENDOR_DEVICE_PATH       VendorDevicePath;
  EFI_DEVICE_PATH_PROTOCOL EndDevicePath;
} PMIC_KEYS_DEVICE_PATH;

STATIC CONST PMIC_KEYS_DEVICE_PATH mKeypadDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)(sizeof (VENDOR_DEVICE_PATH)), (UINT8)((sizeof (VENDOR_DEVICE_PATH)) >> 8) }
    },
    EFI_KEYPAD_DEVICE_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)(END_DEVICE_PATH_LENGTH), (UINT8)((END_DEVICE_PATH_LENGTH) >> 8) }
  }
};

typedef struct {
  UINT32                    Signature;
  LIST_ENTRY                NotifyEntry;
  EFI_KEY_DATA              KeyData;
  EFI_KEY_NOTIFY_FUNCTION   KeyNotificationFn;
} PMIC_KEYS_NOTIFY;

#define PMIC_KEYS_NOTIFY_SIGNATURE  SIGNATURE_32('p', 'm', 'k', 'n')

STATIC EFI_EVENT                       mTimerEvent;
STATIC EFI_EVENT                       mWaitForKey;
STATIC EFI_EVENT                       mWaitForKeyEx;
STATIC EFI_EVENT                       mNotifyEvent;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mSimpleTextIn;
STATIC EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL mSimpleTextInEx;

STATIC EFI_KEY_DATA mKeyQueue[KEY_QUEUE_MAX];
STATIC UINTN        mQueueHead;
STATIC UINTN        mQueueCount;

STATIC EFI_KEY_DATA mNotifyQueue[NOTIFY_QUEUE_MAX];
STATIC UINTN        mNotifyHead;
STATIC UINTN        mNotifyCount;

STATIC LIST_ENTRY mNotifyList;

STATIC UINT8  mPonSid;
STATIC UINT8  mRawPrevious;
STATIC UINT8  mStableState;
STATIC UINT8  mStableCount;
STATIC UINT16 mHoldTicks[KEY_COUNT];
STATIC BOOLEAN mVolumeGpioInput;

/**
  Push a key onto the queue and signal consumers.
**/
STATIC
VOID
PmicKeysEmit (
  IN UINT16  ScanCode,
  IN CHAR16  UnicodeChar
  )
{
  UINTN Index;

  if (mQueueCount >= KEY_QUEUE_MAX) {
    return;
  }

  Index = (mQueueHead + mQueueCount) % KEY_QUEUE_MAX;
  ZeroMem (&mKeyQueue[Index], sizeof (EFI_KEY_DATA));
  mKeyQueue[Index].Key.ScanCode    = ScanCode;
  mKeyQueue[Index].Key.UnicodeChar = UnicodeChar;
  mQueueCount++;

#if PMIC_KEYS_TRACE
  DEBUG ((DEBUG_ERROR, "PmicKeys: emit scan=0x%x char=0x%x\n", ScanCode, UnicodeChar));
#endif

  /* Mirror into the notify queue for SimpleTextInputEx callbacks. */
  if (mNotifyCount < NOTIFY_QUEUE_MAX) {
    Index = (mNotifyHead + mNotifyCount) % NOTIFY_QUEUE_MAX;
    CopyMem (&mNotifyQueue[Index], &mKeyQueue[(mQueueHead + mQueueCount - 1) % KEY_QUEUE_MAX], sizeof (EFI_KEY_DATA));
    mNotifyCount++;
  }

  if (mWaitForKey != NULL) {
    gBS->SignalEvent (mWaitForKey);
  }

  if (mWaitForKeyEx != NULL) {
    gBS->SignalEvent (mWaitForKeyEx);
  }

  if (mNotifyEvent != NULL) {
    gBS->SignalEvent (mNotifyEvent);
  }
}

STATIC
BOOLEAN
PmicKeysQueueIsEmpty (
  VOID
  )
{
  return (BOOLEAN)(mQueueCount == 0);
}

STATIC
EFI_STATUS
PmicKeysPopKey (
  OUT EFI_KEY_DATA  *KeyData
  )
{
  if (mQueueCount == 0) {
    return EFI_NOT_READY;
  }

  CopyMem (KeyData, &mKeyQueue[mQueueHead], sizeof (EFI_KEY_DATA));
  mQueueHead = (mQueueHead + 1) % KEY_QUEUE_MAX;
  mQueueCount--;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
PmicKeysPopNotify (
  OUT EFI_KEY_DATA  *KeyData
  )
{
  if (mNotifyCount == 0) {
    return EFI_NOT_READY;
  }

  CopyMem (KeyData, &mNotifyQueue[mNotifyHead], sizeof (EFI_KEY_DATA));
  mNotifyHead = (mNotifyHead + 1) % NOTIFY_QUEUE_MAX;
  mNotifyCount--;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
PmicKeysIsKeyRegistered (
  IN EFI_KEY_DATA  *RegisteredData,
  IN EFI_KEY_DATA  *InputData
  )
{
  if ((RegisteredData->Key.ScanCode != InputData->Key.ScanCode) ||
      (RegisteredData->Key.UnicodeChar != InputData->Key.UnicodeChar)) {
    return FALSE;
  }

  if (RegisteredData->KeyState.KeyShiftState != 0 &&
      RegisteredData->KeyState.KeyShiftState != InputData->KeyState.KeyShiftState) {
    return FALSE;
  } else if (RegisteredData->KeyState.KeyToggleState != 0 &&
             RegisteredData->KeyState.KeyToggleState != InputData->KeyState.KeyToggleState) {
    return FALSE;
  }

  return TRUE;
}

/* ----------------------------------------------------------------------- */
/* EFI_SIMPLE_TEXT_INPUT_PROTOCOL                                          */
/* ----------------------------------------------------------------------- */

STATIC
EFIAPI
EFI_STATUS
PmicKeysReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  (VOID)This;
  (VOID)ExtendedVerification;

  mQueueHead  = 0;
  mQueueCount = 0;
  return EFI_SUCCESS;
}

STATIC
EFIAPI
EFI_STATUS
PmicKeysReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_KEY_DATA KeyData;
  EFI_STATUS   Status;

  (VOID)This;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = PmicKeysPopKey (&KeyData);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (Key, &KeyData.Key, sizeof (EFI_INPUT_KEY));
  return EFI_SUCCESS;
}

STATIC
EFIAPI
VOID
PmicKeysWaitForKeyNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (!PmicKeysQueueIsEmpty ()) {
    gBS->SignalEvent (Event);
  }
}

/* ----------------------------------------------------------------------- */
/* EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL                                       */
/* ----------------------------------------------------------------------- */

STATIC
EFIAPI
EFI_STATUS
PmicKeysResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  return PmicKeysReset (&mSimpleTextIn, ExtendedVerification);
}

STATIC
EFIAPI
EFI_STATUS
PmicKeysReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  (VOID)This;

  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return PmicKeysPopKey (KeyData);
}

STATIC
EFIAPI
EFI_STATUS
PmicKeysSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE               *KeyToggleState
  )
{
  (VOID)This;

  if (KeyToggleState == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*KeyToggleState & EFI_TOGGLE_STATE_VALID) != EFI_TOGGLE_STATE_VALID) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFIAPI
EFI_STATUS
PmicKeysRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  PMIC_KEYS_NOTIFY  *NewNotify;
  LIST_ENTRY        *Link;
  PMIC_KEYS_NOTIFY  *CurrentNotify;

  (VOID)This;

  if (KeyData == NULL || NotifyHandle == NULL || KeyNotificationFunction == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    CurrentNotify = CR (Link, PMIC_KEYS_NOTIFY, NotifyEntry, PMIC_KEYS_NOTIFY_SIGNATURE);
    if (PmicKeysIsKeyRegistered (&CurrentNotify->KeyData, KeyData)) {
      if (CurrentNotify->KeyNotificationFn == KeyNotificationFunction) {
        *NotifyHandle = CurrentNotify;
        return EFI_SUCCESS;
      }
    }
  }

  NewNotify = AllocateZeroPool (sizeof (PMIC_KEYS_NOTIFY));
  if (NewNotify == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  NewNotify->Signature         = PMIC_KEYS_NOTIFY_SIGNATURE;
  NewNotify->KeyNotificationFn = KeyNotificationFunction;
  CopyMem (&NewNotify->KeyData, KeyData, sizeof (EFI_KEY_DATA));

  InsertTailList (&mNotifyList, &NewNotify->NotifyEntry);

  *NotifyHandle = NewNotify;
  return EFI_SUCCESS;
}

STATIC
EFIAPI
EFI_STATUS
PmicKeysUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  LIST_ENTRY        *Link;
  PMIC_KEYS_NOTIFY  *CurrentNotify;

  (VOID)This;

  if (NotificationHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
    CurrentNotify = CR (Link, PMIC_KEYS_NOTIFY, NotifyEntry, PMIC_KEYS_NOTIFY_SIGNATURE);
    if (CurrentNotify == NotificationHandle) {
      RemoveEntryList (&CurrentNotify->NotifyEntry);
      FreePool (CurrentNotify);
      return EFI_SUCCESS;
    }
  }

  return EFI_INVALID_PARAMETER;
}

STATIC
EFIAPI
VOID
PmicKeysWaitForKeyExNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  PmicKeysWaitForKeyNotify (Event, Context);
}

/**
  Process SimpleTextInputEx key notifications.
**/
STATIC
EFIAPI
VOID
PmicKeysNotifyProcess (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_KEY_DATA       KeyData;
  LIST_ENTRY         *Link;
  PMIC_KEYS_NOTIFY   *CurrentNotify;

  (VOID)Event;
  (VOID)Context;

  while (TRUE) {
    if (EFI_ERROR (PmicKeysPopNotify (&KeyData))) {
      break;
    }

    for (Link = GetFirstNode (&mNotifyList); !IsNull (&mNotifyList, Link); Link = GetNextNode (&mNotifyList, Link)) {
      CurrentNotify = CR (Link, PMIC_KEYS_NOTIFY, NotifyEntry, PMIC_KEYS_NOTIFY_SIGNATURE);
      if (PmicKeysIsKeyRegistered (&CurrentNotify->KeyData, &KeyData)) {
        CurrentNotify->KeyNotificationFn (&KeyData);
      }
    }
  }
}

/* ----------------------------------------------------------------------- */
/* PMIC key polling                                                         */
/* ----------------------------------------------------------------------- */

/**
  Read the three key states, normalised so that 1 == pressed.
**/
STATIC
UINT8
PmicKeysReadRaw (
  VOID
  )
{
  UINT8       PonStatus   = 0;
  UINT8       VolUpStatus = 0;
  UINT8       State       = 0;
  EFI_STATUS  Status;

  Status = SpmiRead8 (mPonSid, PON_RT_STS_ADDR, &PonStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PmicKeys: PON read failed (0x%x)\n", Status));
    return mStableState;
  }

  if (((PonStatus >> PON_IRQ_KPDPWR) & 1) != 0) {
    State |= KEY_BIT (KEY_PWR);
  }

  if (((PonStatus >> PON_IRQ_RESIN) & 1) != 0) {
    State |= KEY_BIT (KEY_VOLM);
  }

  Status = SpmiRead8 (SPMI_SID_PM8550, VOLUP_RT_STS_ADDR, &VolUpStatus);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PmicKeys: GPIO read failed (0x%x)\n", Status));
    return State;
  }

  /* gpio-keys declares this line GPIO_ACTIVE_LOW */
  if ((VolUpStatus & 1) == 0) {
    State |= KEY_BIT (KEY_VOLP);
  }

  return State;
}

STATIC
EFIAPI
VOID
PmicKeysPoll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8   Raw;
  UINT8   Stable;
  UINT8   Pressed;
  UINT8   WasPressed;
  UINTN   Index;

  (VOID)Event;
  (VOID)Context;

  Raw = PmicKeysReadRaw ();

  if (Raw != mRawPrevious) {
    mRawPrevious = Raw;
    mStableCount = 0;
  } else if (mStableCount < KEY_DEBOUNCE_TICKS) {
    mStableCount++;
  }

  if (mStableCount < KEY_DEBOUNCE_TICKS) {
    return;
  }

  Stable = mRawPrevious;

#if PMIC_KEYS_TRACE
  if (Stable != mStableState) {
    DEBUG ((
      DEBUG_ERROR,
      "PmicKeys: state pwr=%u volm=%u volp=%u\n",
      (Stable & KEY_BIT (KEY_PWR)) ? 1 : 0,
      (Stable & KEY_BIT (KEY_VOLM)) ? 1 : 0,
      (Stable & KEY_BIT (KEY_VOLP)) ? 1 : 0
      ));
  }
#endif

  if (Stable != mStableState) {
    for (Index = 0; Index < KEY_COUNT; Index++) {
      Pressed    = (UINT8)(Stable & KEY_BIT (Index));
      WasPressed = (UINT8)(mStableState & KEY_BIT (Index));

      if (Pressed == 0) {
        mHoldTicks[Index] = 0;
        continue;
      }

      if (WasPressed != 0) {
        continue;
      }

      mHoldTicks[Index] = 1;

      if ((Index == KEY_PWR) && ((Stable & KEY_BIT (KEY_VOLM)) != 0)) {
        /* volume- held + power press -> "back" */
        PmicKeysEmit (SCAN_ESC, 0);
      } else if (Index == KEY_VOLP) {
        PmicKeysEmit (SCAN_UP, 0);
      } else if (Index == KEY_VOLM) {
        PmicKeysEmit (SCAN_DOWN, 0);
      } else {
        /* power -> Enter */
        PmicKeysEmit (0, CHAR_CARRIAGE_RETURN);
      }
    }

    mStableState = Stable;
    return;
  }

  for (Index = 0; Index < KEY_COUNT; Index++) {
    if ((Stable & KEY_BIT (Index)) == 0) {
      continue;
    }

    if (mHoldTicks[Index] < 0xFFFF) {
      mHoldTicks[Index]++;
    }

    if (mHoldTicks[Index] <= KEY_REPEAT_DELAY_TICKS) {
      continue;
    }

    if (((mHoldTicks[Index] - KEY_REPEAT_DELAY_TICKS) % KEY_REPEAT_RATE_TICKS) == 0) {
      if ((Index == KEY_PWR) && ((Stable & KEY_BIT (KEY_VOLM)) != 0)) {
        PmicKeysEmit (SCAN_ESC, 0);
      } else if (Index == KEY_VOLP) {
        PmicKeysEmit (SCAN_UP, 0);
      } else if (Index == KEY_VOLM) {
        PmicKeysEmit (SCAN_DOWN, 0);
      } else {
        PmicKeysEmit (0, CHAR_CARRIAGE_RETURN);
      }
    }
  }
}

/**
  The volume_up PMIC GPIO may still be in its power-on default state on a
  cold boot; make sure it is enabled and configured as a digital input.
**/
STATIC
VOID
PmicKeysConfigureVolumeGpio (
  VOID
  )
{
  UINT8        Type    = 0;
  UINT8        Mode    = 0;
  UINT8        Enabled = 0;
  EFI_STATUS   Status;
  UINT8        Dir;

  Status = SpmiRead8 (SPMI_SID_PM8550, VOLUP_TYPE_ADDR, &Type);
  if (EFI_ERROR (Status) || (Type != PMIC_GPIO_TYPE)) {
    DEBUG ((
      DEBUG_ERROR,
      "PmicKeys: vol+ GPIO type 0x%x (expected 0x%x), status 0x%x - not configuring\n",
      Type,
      PMIC_GPIO_TYPE,
      Status
      ));
    return;
  }

  SpmiRead8 (SPMI_SID_PM8550, VOLUP_MODE_CTL_ADDR, &Mode);
  SpmiRead8 (SPMI_SID_PM8550, VOLUP_EN_CTL_ADDR, &Enabled);

  Dir = (Mode >> PMIC_GPIO_MODE_DIR_SHIFT) & PMIC_GPIO_MODE_DIR_MASK;

  DEBUG ((
    DEBUG_ERROR,
    "PmicKeys: vol+ GPIO mode=0x%x dir=%u en=0x%x\n",
    Mode,
    Dir,
    Enabled
    ));

  if ((Dir == PMIC_GPIO_DIR_INPUT) && ((Enabled & PMIC_GPIO_MASTER_EN_BIT) != 0)) {
    mVolumeGpioInput = TRUE;
    return;
  }

  DEBUG ((DEBUG_ERROR, "PmicKeys: configuring vol+ GPIO as digital input\n"));

  if (Dir != PMIC_GPIO_DIR_INPUT) {
    /* function = normal (0), direction = digital input (0), value = 0 */
    Status = SpmiWrite8 (SPMI_SID_PM8550, VOLUP_MODE_CTL_ADDR, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "PmicKeys: MODE_CTL write failed (0x%x)\n", Status));
      return;
    }
  }

  if ((Enabled & PMIC_GPIO_MASTER_EN_BIT) == 0) {
    Status = SpmiWrite8 (SPMI_SID_PM8550, VOLUP_EN_CTL_ADDR, (UINT8)(Enabled | PMIC_GPIO_MASTER_EN_BIT));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "PmicKeys: EN_CTL write failed (0x%x)\n", Status));
      return;
    }
  }

  mVolumeGpioInput = TRUE;
}

STATIC
VOID
PmicKeysSelectPonSid (
  VOID
  )
{
  UINTN Index;
  INT32 Apid;

  mPonSid = mPonSidCandidates[0];

  for (Index = 0; Index < ARRAY_SIZE (mPonSidCandidates); Index++) {
    Apid = SpmiPpidToApId (mPonSidCandidates[Index], PON_PERIPH_ID);
    DEBUG ((
      DEBUG_ERROR,
      "PmicKeys: PON sid 0x%x -> apid %d\n",
      mPonSidCandidates[Index],
      Apid
      ));

    if (Apid >= 0) {
      mPonSid = mPonSidCandidates[Index];
      return;
    }
  }

  DEBUG ((DEBUG_ERROR, "PmicKeys: no mapped PON peripheral, falling back to sid 0x0\n"));
}

EFI_STATUS
EFIAPI
PmicKeysDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)SystemTable;

  Status = SpmiArbInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PmicKeys: SPMI arbiter init failed (0x%x)\n", Status));
    return Status;
  }

  PmicKeysSelectPonSid ();
  PmicKeysConfigureVolumeGpio ();

  InitializeListHead (&mNotifyList);

  /* SimpleTextIn WaitForKey */
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  PmicKeysWaitForKeyNotify,
                  NULL,
                  &mWaitForKey
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* SimpleTextInputEx WaitForKeyEx */
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  PmicKeysWaitForKeyExNotify,
                  NULL,
                  &mWaitForKeyEx
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* SimpleTextInputEx key notify dispatcher */
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PmicKeysNotifyProcess,
                  NULL,
                  &mNotifyEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mSimpleTextIn.Reset         = PmicKeysReset;
  mSimpleTextIn.ReadKeyStroke = PmicKeysReadKeyStroke;
  mSimpleTextIn.WaitForKey    = mWaitForKey;

  mSimpleTextInEx.Reset             = PmicKeysResetEx;
  mSimpleTextInEx.ReadKeyStrokeEx   = PmicKeysReadKeyStrokeEx;
  mSimpleTextInEx.SetState          = PmicKeysSetState;
  mSimpleTextInEx.RegisterKeyNotify = PmicKeysRegisterKeyNotify;
  mSimpleTextInEx.UnregisterKeyNotify = PmicKeysUnregisterKeyNotify;
  mSimpleTextInEx.WaitForKeyEx      = mWaitForKeyEx;

  /* Install SimpleTextIn + SimpleTextInputEx + DevicePath on one handle. */
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiSimpleTextInProtocolGuid,
                  &mSimpleTextIn,
                  &gEfiSimpleTextInputExProtocolGuid,
                  &mSimpleTextInEx,
                  &gEfiDevicePathProtocolGuid,
                  (VOID *)&mKeypadDevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PmicKeys: install protocols failed (0x%x)\n", Status));
    return Status;
  }

  /* Poll timer */
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PmicKeysPoll,
                  NULL,
                  &mTimerEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mTimerEvent, TimerPeriodic, KEY_POLL_INTERVAL_100NS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_ERROR,
    "PmicKeys: ready, sid=0x%x vol+ addr=0x%x input=%u\n",
    mPonSid,
    VOLUP_RT_STS_ADDR,
    mVolumeGpioInput ? 1 : 0
    ));

  return EFI_SUCCESS;
}
