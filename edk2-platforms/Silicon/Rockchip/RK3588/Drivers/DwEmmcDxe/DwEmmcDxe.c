/** @file
  This file implement the MMC Host Protocol for the DesignWare eMMC.

  WARNING:
  This driver fails to follow the UEFI driver model without a good
  reason, and only remains in the tree because it is still used by
  a small number of platforms. It will be removed when no longer used.

  Copyright (c) 2014-2017, Linaro Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/MmcHost.h>
#include <Protocol/ArmScmi.h>
#include <Protocol/ArmScmiClockProtocol.h>
#include <Library/RockchipPlatformLib.h>
#include "Include/Library/CruLib.h"
#include "Soc.h"
#include "DwEmmc.h"

#define SCMI_CCLK_SD			9

#define DW_DBG		DEBUG_BLKIO

#define DWEMMC_DESC_PAGE                1
#define DWEMMC_BLOCK_SIZE               512
#define DWEMMC_DMA_BUF_SIZE             (512 * 8)
#define DWEMMC_MAX_DESC_PAGES           512

typedef struct {
  UINT32                        Des0;
  UINT32                        Des1;
  UINT32                        Des2;
  UINT32                        Des3;
} DWEMMC_IDMAC_DESCRIPTOR;

EFI_MMC_HOST_PROTOCOL     *gpMmcHost;
DWEMMC_IDMAC_DESCRIPTOR   *gpIdmacDesc;
EFI_GUID mDwEmmcDevicePathGuid = EFI_CALLER_ID_GUID;
STATIC UINT32 mDwEmmcCommand;
STATIC UINT32 mDwEmmcArgument;

EFI_STATUS
DwEmmcReadBlockData (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN EFI_LBA                    Lba,
  IN UINTN                      Length,
  IN UINT32*                    Buffer
  );

BOOLEAN
DwEmmcIsPowerOn (
  VOID
  )
{
    return TRUE;
}

EFI_STATUS
DwEmmcInitialize (
  VOID
  )
{
    DEBUG ((DEBUG_BLKIO, "DwEmmcInitialize()"));
    return EFI_SUCCESS;
}

BOOLEAN
DwEmmcIsCardPresent (
  IN EFI_MMC_HOST_PROTOCOL     *This
  )
{
  /* check card present */
  return TRUE;
}

BOOLEAN
DwEmmcIsReadOnly (
  IN EFI_MMC_HOST_PROTOCOL     *This
  )
{
  return FALSE;
}

BOOLEAN
DwEmmcIsDmaSupported (
  IN EFI_MMC_HOST_PROTOCOL     *This
  )
{
  return TRUE;
}

EFI_STATUS
DwEmmcBuildDevicePath (
  IN EFI_MMC_HOST_PROTOCOL      *This,
  IN EFI_DEVICE_PATH_PROTOCOL   **DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL *NewDevicePathNode;

  NewDevicePathNode = CreateDeviceNode (HARDWARE_DEVICE_PATH, HW_VENDOR_DP, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (& ((VENDOR_DEVICE_PATH*)NewDevicePathNode)->Guid, &mDwEmmcDevicePathGuid);

  *DevicePath = NewDevicePathNode;
  return EFI_SUCCESS;
}

EFI_STATUS
DwEmmcUpdateClock (
  VOID
  )
{
  UINT32 Data;

  /* CMD_UPDATE_CLK */
  Data = BIT_CMD_WAIT_PRVDATA_COMPLETE | BIT_CMD_UPDATE_CLOCK_ONLY |
         BIT_CMD_START;
  MmioWrite32 (DWEMMC_CMD, Data);
  while (1) {
    Data = MmioRead32 (DWEMMC_CMD);
    if (!(Data & CMD_START_BIT)) {
      break;
    }
    Data = MmioRead32 (DWEMMC_RINTSTS);
    if (Data & DWEMMC_INT_HLE) {
      Print (L"failed to update mmc clock frequency\n");
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
DwEmmcSetClock (
  IN UINTN                     ClockFreq
  )
{
  UINT32 Data;
  EFI_STATUS Status;
  SCMI_CLOCK_PROTOCOL    *ClockProtocol;
  EFI_GUID               ClockProtocolGuid = ARM_SCMI_CLOCK_PROTOCOL_GUID;

  // Wait until MMC is idle
  do {
    Data = MmioRead32 (DWEMMC_STATUS);
  } while (Data & DWEMMC_STS_DATA_BUSY);

  // Disable MMC clock first
  MmioWrite32 (DWEMMC_CLKENA, 0);
  Status = DwEmmcUpdateClock ();
  ASSERT (!EFI_ERROR (Status));
  //HAL_CRU_ClkSetFreq(CLK_SDMMC0, ClockFreq);
  //DEBUG ((DW_DBG, "%a():HAL_CRU_ClkGetFreq:%d\n", __func__, HAL_CRU_ClkGetFreq(CLK_SDMMC0)));
  Status = gBS->LocateProtocol (
                  &ClockProtocolGuid,
                  NULL,
                  (VOID**)&ClockProtocol
                  );
  ASSERT (!EFI_ERROR (Status));
  MmioWrite32 (DWEMMC_CLKDIV, 1);
  Status = DwEmmcUpdateClock ();
  ASSERT (!EFI_ERROR (Status));
  Status = ClockProtocol->RateSet (
                            ClockProtocol,
                            SCMI_CCLK_SD,
                            ClockFreq
                            );
  ASSERT (!EFI_ERROR (Status));
  // Enable MMC clock
  MmioWrite32 (DWEMMC_CLKENA, 1);
  MmioWrite32 (DWEMMC_CLKSRC, 0);
  Status = DwEmmcUpdateClock ();
  ASSERT (!EFI_ERROR (Status));
  return EFI_SUCCESS;
}

EFI_STATUS
DwEmmcNotifyState (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN MMC_STATE                 State
  )
{
  UINT32      Data;
  EFI_STATUS  Status;

  switch (State) {
  case MmcInvalidState:
    return EFI_INVALID_PARAMETER;
  case MmcHwInitializationState:
    MmioWrite32 (DWEMMC_PWREN, 1);

    // If device already turn on then restart it
    Data = DWEMMC_CTRL_RESET_ALL;
    MmioWrite32 (DWEMMC_CTRL, Data);
    do {
      // Wait until reset operation finished
      Data = MmioRead32 (DWEMMC_CTRL);
    } while (Data & DWEMMC_CTRL_RESET_ALL);

    // Setup clock that could not be higher than 400KHz.
    Status = DwEmmcSetClock (400000);
    ASSERT (!EFI_ERROR (Status));
    // Wait clock stable
    MicroSecondDelay (100);

    MmioWrite32 (DWEMMC_RINTSTS, ~0);
    MmioWrite32 (DWEMMC_INTMASK, 0);
    MmioWrite32 (DWEMMC_TMOUT, ~0);
    MmioWrite32 (DWEMMC_IDINTEN, 0);
    MmioWrite32 (DWEMMC_BMOD, DWEMMC_IDMAC_SWRESET);
    MmioWrite32 (DWEMMC_CTYPE, 0);
    MmioWrite32 (DWEMMC_BLKSIZ, DWEMMC_BLOCK_SIZE);
    do {
      Data = MmioRead32 (DWEMMC_BMOD);
    } while (Data & DWEMMC_IDMAC_SWRESET);
    break;
  case MmcIdleState:
    break;
  case MmcReadyState:
    break;
  case MmcIdentificationState:
    break;
  case MmcStandByState:
    break;
  case MmcTransferState:
    break;
  case MmcSendingDataState:
    break;
  case MmcReceiveDataState:
    break;
  case MmcProgrammingState:
    break;
  case MmcDisconnectState:
    break;
  default:
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

// Need to prepare DMA buffer first before sending commands to MMC card
BOOLEAN
IsPendingReadCommand (
  IN MMC_CMD                    MmcCmd
  )
{
  UINTN  Mask;

  Mask = BIT_CMD_DATA_EXPECTED | BIT_CMD_READ;
  if ((MmcCmd & Mask) == Mask) {
    return TRUE;
  }
  return FALSE;
}

BOOLEAN
IsPendingWriteCommand (
  IN MMC_CMD                    MmcCmd
  )
{
  UINTN  Mask;

  Mask = BIT_CMD_DATA_EXPECTED | BIT_CMD_WRITE;
  if ((MmcCmd & Mask) == Mask) {
    return TRUE;
  }
  return FALSE;
}

EFI_STATUS
SendCommand (
  IN MMC_CMD                    MmcCmd,
  IN UINT32                     Argument
  )
{
  UINT32      Data, ErrMask;

  // Wait until MMC is idle
  do {
    Data = MmioRead32 (DWEMMC_STATUS);
  } while (Data & DWEMMC_STS_DATA_BUSY);

  MmioWrite32 (DWEMMC_RINTSTS, ~0);
  MmioWrite32 (DWEMMC_CMDARG, Argument);
  MmioWrite32 (DWEMMC_CMD, MmcCmd);

  ErrMask = DWEMMC_INT_EBE | DWEMMC_INT_HLE | DWEMMC_INT_RTO |
            DWEMMC_INT_RCRC | DWEMMC_INT_RE;
  ErrMask |= DWEMMC_INT_DCRC | DWEMMC_INT_DRT | DWEMMC_INT_SBE;
  do {
    MicroSecondDelay(500);
    Data = MmioRead32 (DWEMMC_RINTSTS);

    if (Data & ErrMask) {
      return EFI_DEVICE_ERROR;
    }
    if (Data & DWEMMC_INT_DTO) {     // Transfer Done
      break;
    }
  } while (!(Data & DWEMMC_INT_CMD_DONE));
  return EFI_SUCCESS;
}

EFI_STATUS
DwEmmcSendCommand (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN MMC_CMD                    MmcCmd,
  IN UINT32                     Argument
  )
{
  UINT32       Cmd = 0;
  EFI_STATUS   Status = EFI_SUCCESS;

  switch (MMC_GET_INDX(MmcCmd)) {
  case MMC_INDX(0):
    Cmd = BIT_CMD_SEND_INIT;
    break;
  case MMC_INDX(1):
    Cmd = BIT_CMD_RESPONSE_EXPECT;
    break;
  case MMC_INDX(2):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_LONG_RESPONSE |
           BIT_CMD_CHECK_RESPONSE_CRC | BIT_CMD_SEND_INIT;
    break;
  case MMC_INDX(3):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_SEND_INIT;
    break;
  case MMC_INDX(6):
      Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    break;
  case MMC_INDX(7):
    if (Argument)
        Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    else
        Cmd = 0;
    break;
  case MMC_INDX(8):
    if (Argument)
      Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
        BIT_CMD_WAIT_PRVDATA_COMPLETE;
    else
      Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
        BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
        BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(9):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_LONG_RESPONSE;
    break;
  case MMC_INDX(12):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_STOP_ABORT_CMD;
    break;
  case MMC_INDX(13):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(16):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    break;
  case MMC_INDX(17):
  case MMC_INDX(18):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(24):
  case MMC_INDX(25):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_DATA_EXPECTED | BIT_CMD_WRITE |
           BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  case MMC_INDX(30):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
           BIT_CMD_DATA_EXPECTED;
    break;
  case MMC_INDX(41):  //MMC_CMD41
    Cmd = BIT_CMD_RESPONSE_EXPECT;
    break;
  case MMC_INDX(51):
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC |
            BIT_CMD_DATA_EXPECTED | BIT_CMD_READ |
            BIT_CMD_WAIT_PRVDATA_COMPLETE;
    break;
  default:
    Cmd = BIT_CMD_RESPONSE_EXPECT | BIT_CMD_CHECK_RESPONSE_CRC;
    break;
  }

  Cmd |= MMC_GET_INDX(MmcCmd) | BIT_CMD_USE_HOLD_REG | BIT_CMD_START;
  if (IsPendingReadCommand (Cmd) || IsPendingWriteCommand (Cmd)) {
    mDwEmmcCommand = Cmd;
    mDwEmmcArgument = Argument;
  } else {
    Status = SendCommand (Cmd, Argument);
  }
  return Status;
}

EFI_STATUS
DwEmmcReceiveResponse (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN MMC_RESPONSE_TYPE          Type,
  IN UINT32*                    Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (   (Type == MMC_RESPONSE_TYPE_R1)
      || (Type == MMC_RESPONSE_TYPE_R1b)
      || (Type == MMC_RESPONSE_TYPE_R3)
      || (Type == MMC_RESPONSE_TYPE_R6)
      || (Type == MMC_RESPONSE_TYPE_R7))
  {
    Buffer[0] = MmioRead32 (DWEMMC_RESP0);
  } else if (Type == MMC_RESPONSE_TYPE_R2) {
    Buffer[0] = MmioRead32 (DWEMMC_RESP0);
    Buffer[1] = MmioRead32 (DWEMMC_RESP1);
    Buffer[2] = MmioRead32 (DWEMMC_RESP2);
    Buffer[3] = MmioRead32 (DWEMMC_RESP3);
  }
  return EFI_SUCCESS;
}

EFI_STATUS
PrepareDmaData (
  IN DWEMMC_IDMAC_DESCRIPTOR*    IdmacDesc,
  IN UINTN                      Length,
  IN UINT32*                    Buffer
  )
{
  UINTN  Cnt, Blks, Idx, LastIdx;

  Cnt = (Length + DWEMMC_DMA_BUF_SIZE - 1) / DWEMMC_DMA_BUF_SIZE;
  Blks = (Length + DWEMMC_BLOCK_SIZE - 1) / DWEMMC_BLOCK_SIZE;
  Length = DWEMMC_BLOCK_SIZE * Blks;

  for (Idx = 0; Idx < Cnt; Idx++) {
    (IdmacDesc + Idx)->Des0 = DWEMMC_IDMAC_DES0_OWN | DWEMMC_IDMAC_DES0_CH |
                              DWEMMC_IDMAC_DES0_DIC;
    (IdmacDesc + Idx)->Des1 = DWEMMC_IDMAC_DES1_BS1(DWEMMC_DMA_BUF_SIZE);
    /* Buffer Address */
    (IdmacDesc + Idx)->Des2 = (UINT32)((UINTN)Buffer + DWEMMC_DMA_BUF_SIZE * Idx);
    /* Next Descriptor Address */
    (IdmacDesc + Idx)->Des3 = (UINT32)((UINTN)IdmacDesc +
                                       (sizeof(DWEMMC_IDMAC_DESCRIPTOR) * (Idx + 1)));
  }
  /* First Descriptor */
  IdmacDesc->Des0 |= DWEMMC_IDMAC_DES0_FS;
  /* Last Descriptor */
  LastIdx = Cnt - 1;
  (IdmacDesc + LastIdx)->Des0 |= DWEMMC_IDMAC_DES0_LD;
  (IdmacDesc + LastIdx)->Des0 &= ~(DWEMMC_IDMAC_DES0_DIC | DWEMMC_IDMAC_DES0_CH);
  (IdmacDesc + LastIdx)->Des1 = DWEMMC_IDMAC_DES1_BS1(Length -
                                                      (LastIdx * DWEMMC_DMA_BUF_SIZE));
  /* Set the Next field of Last Descriptor */
  (IdmacDesc + LastIdx)->Des3 = 0;
  MmioWrite32 (DWEMMC_DBADDR, (UINT32)((UINTN)IdmacDesc));

  return EFI_SUCCESS;
}

VOID
StartDma (
  UINTN    Length
  )
{
  UINT32 Data;

  Data = MmioRead32 (DWEMMC_CTRL);
  Data |= DWEMMC_CTRL_INT_EN | DWEMMC_CTRL_DMA_EN | DWEMMC_CTRL_IDMAC_EN;
  MmioWrite32 (DWEMMC_CTRL, Data);
  Data = MmioRead32 (DWEMMC_BMOD);
  Data |= DWEMMC_IDMAC_ENABLE | DWEMMC_IDMAC_FB;
  MmioWrite32 (DWEMMC_BMOD, Data);

  MmioWrite32 (DWEMMC_BLKSIZ, DWEMMC_BLOCK_SIZE);
  MmioWrite32 (DWEMMC_BYTCNT, Length);
}

#define FIFO_RESET        (0x1<<1)	/* Reset FIFO */
#define FIFO_EMPTY        (0x1<<2)
#define FIFO_RESET        (0x1<<1)	/* Reset FIFO */
#define DWEMMC_MSHCI_FIFO           ((UINT32)PcdGet32 (PcdDwEmmcDxeBaseAddress) + 0x200)

EFI_STATUS
DwEmmcReadBlockData (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN EFI_LBA                    Lba,
  IN UINTN                      Length,
  IN UINT32*                   Buffer
  )
{
  EFI_STATUS	Status;
  UINT32		DataLen = Length>>2; //byte to word
  EFI_STATUS	ret = EFI_SUCCESS;
  UINT32		Data;
  UINT32		TimeOut = 0;
  UINT32		value = 0;

  if (mDwEmmcCommand & BIT_CMD_WAIT_PRVDATA_COMPLETE) {
    do {
      Data = MmioRead32 (DWEMMC_STATUS);
    } while (Data & DWEMMC_STS_DATA_BUSY);
  }

  if ((mDwEmmcCommand & BIT_CMD_STOP_ABORT_CMD) || (mDwEmmcCommand & BIT_CMD_DATA_EXPECTED)) {
    if (!(MmioRead32 (DWEMMC_STATUS) & FIFO_EMPTY)) {
      Data = MmioRead32 (DWEMMC_CTRL);
      Data |= FIFO_RESET;
      MmioWrite32 (DWEMMC_CTRL, Data);

      TimeOut = 100000;
      while (((value = MmioRead32 (DWEMMC_CTRL)) & (FIFO_RESET)) && (TimeOut > 0)) {
        TimeOut--;
      }
      if (TimeOut == 0) {
        DEBUG ((DEBUG_ERROR, "%a():  CMD=%d SDC_SDC_ERROR\n", __func__, mDwEmmcCommand&0x3f));
        return EFI_DEVICE_ERROR;
      }
    }
  }

  MmioWrite32 (DWEMMC_BLKSIZ, 512);
  MmioWrite32 (DWEMMC_BYTCNT, Length);

  Status = SendCommand (mDwEmmcCommand, mDwEmmcArgument);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to read data, mDwEmmcCommand:%x, mDwEmmcArgument:%x, Status:%r\n", mDwEmmcCommand, mDwEmmcArgument, Status));
    return EFI_DEVICE_ERROR;
  }

  TimeOut = 1000000;
  while (DataLen) {
    if (MmioRead32(DWEMMC_RINTSTS) & (DWEMMC_INT_DRT | DWEMMC_INT_SBE | DWEMMC_INT_EBE | DWEMMC_INT_DCRC))  {
      DEBUG ((DEBUG_ERROR, "%a(): EFI_DEVICE_ERROR DWEMMC_RINTSTS=0x%x DataLen=%d\n",
        __func__, MmioRead32(DWEMMC_RINTSTS), DataLen));
      return EFI_DEVICE_ERROR;
    }

    while((!(MmioRead32(DWEMMC_STATUS) & FIFO_EMPTY)) && DataLen) {
      *Buffer++ = MmioRead32(DWEMMC_MSHCI_FIFO);
      DataLen--;
      TimeOut = 1000000;
    }

    if (!DataLen) {
      ret = (MmioRead32(DWEMMC_RINTSTS) & (DWEMMC_INT_DRT | DWEMMC_INT_SBE | DWEMMC_INT_EBE | DWEMMC_INT_DCRC))? 
        EFI_DEVICE_ERROR : EFI_SUCCESS;
      DEBUG((DW_DBG, "%a(): DataLen end :%d\n", __func__, ret));
      break;
    }

    NanoSecondDelay(1);
    TimeOut--;
    if (TimeOut == 0) {
      ret = EFI_DEVICE_ERROR;
      DEBUG ((DEBUG_ERROR, "%a(): TimeOut! DataLen=%d\n", __func__, DataLen));
      break;
    }
  }

  return ret;
}

#define MMC_GET_FCNT(x)		        (((x)>>17) & 0x1FF)
#define INTMSK_HTO      (0x1<<10)

/* Common flag combinations */
#define MMC_DATA_ERROR_FLAGS (DWEMMC_INT_DRT | DWEMMC_INT_DCRC | DWEMMC_INT_FRUN | \
	DWEMMC_INT_HLE | INTMSK_HTO | DWEMMC_INT_SBE  | \
	DWEMMC_INT_EBE)

EFI_STATUS
DwEmmcWriteBlockData (
  IN EFI_MMC_HOST_PROTOCOL     *This,
  IN EFI_LBA                    Lba,
  IN UINTN                      Length,
  IN UINT32*                    Buffer
  )
{
  UINT32 *DataBuffer = Buffer;
  UINTN Count=0;
  UINTN Size32 = Length / 4;
  UINT32 Mask;
  EFI_STATUS	Status;
  UINT32		Data;
  UINT32		TimeOut = 0;
  UINT32		value = 0;

  if (mDwEmmcCommand & BIT_CMD_WAIT_PRVDATA_COMPLETE) {
    do {
      Data = MmioRead32 (DWEMMC_STATUS);
    } while (Data & DWEMMC_STS_DATA_BUSY);
  }

  if (!(((mDwEmmcCommand&0x3f) == 6) || ((mDwEmmcCommand&0x3f) == 51))) {
    if ((mDwEmmcCommand & BIT_CMD_STOP_ABORT_CMD) || (mDwEmmcCommand & BIT_CMD_DATA_EXPECTED)) {
      if (!(MmioRead32 (DWEMMC_STATUS) & FIFO_EMPTY)) {
        Data = MmioRead32 (DWEMMC_CTRL);
        Data |= FIFO_RESET;
        MmioWrite32 (DWEMMC_CTRL, Data);

        TimeOut = 100000;
        while (((value = MmioRead32 (DWEMMC_CTRL)) & (FIFO_RESET)) && (TimeOut > 0)) {
          TimeOut--;
        }
        if (TimeOut == 0) {
          DEBUG ((DEBUG_ERROR, "%a():  CMD=%d SDC_SDC_ERROR\n", __func__, mDwEmmcCommand&0x3f));
          return EFI_DEVICE_ERROR;
        }
      }
    }
  }

  MmioWrite32 (DWEMMC_BLKSIZ, 512);
  MmioWrite32 (DWEMMC_BYTCNT, Length);

  Status = SendCommand (mDwEmmcCommand, mDwEmmcArgument);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to write data, mDwEmmcCommand:%x, mDwEmmcArgument:%x, Status:%r\n", mDwEmmcCommand, mDwEmmcArgument, Status));
    return EFI_DEVICE_ERROR;
  }

  for (Count = 0; Count < Size32; Count++) {
    while(MMC_GET_FCNT(MmioRead32(DWEMMC_STATUS)) >32)
    MicroSecondDelay(1);
    MmioWrite32((DWEMMC_MSHCI_FIFO), *DataBuffer++);
  }

  do {
    Mask = MmioRead32(DWEMMC_RINTSTS);
    if (Mask & (MMC_DATA_ERROR_FLAGS)) {
      DEBUG((DEBUG_ERROR, "SdmmcWriteData error, RINTSTS = 0x%08x\n", Mask));
      return EFI_DEVICE_ERROR;
    }	
  } while (!(Mask & DWEMMC_INT_DTO));

  return EFI_SUCCESS;
}

EFI_STATUS
DwEmmcSetIos (
  IN EFI_MMC_HOST_PROTOCOL      *This,
  IN  UINT32                    BusClockFreq,
  IN  UINT32                    BusWidth,
  IN  UINT32                    TimingMode
  )
{
  EFI_STATUS Status = EFI_SUCCESS;
  UINT32    Data;

  if ((PcdGet32 (PcdDwEmmcDxeMaxClockFreqInHz) != 0) &&
      (BusClockFreq > PcdGet32 (PcdDwEmmcDxeMaxClockFreqInHz))) {
    return EFI_UNSUPPORTED;
  }
  if (TimingMode != EMMCBACKWARD) {
    Data = MmioRead32 (DWEMMC_UHSREG);
    switch (TimingMode) {
    case EMMCHS52DDR1V2:
    case EMMCHS52DDR1V8:
      Data |= 1 << 16;
      break;
    case EMMCHS52:
    case EMMCHS26:
      Data &= ~(1 << 16);
      break;
    default:
      return EFI_UNSUPPORTED;
    }
    MmioWrite32 (DWEMMC_UHSREG, Data);
  }

  switch (BusWidth) {
  case 1:
    MmioWrite32 (DWEMMC_CTYPE, 0);
    break;
  case 4:
    MmioWrite32 (DWEMMC_CTYPE, 1);
    break;
  case 8:
    MmioWrite32 (DWEMMC_CTYPE, 1 << 16);
    break;
  default:
    return EFI_UNSUPPORTED;
  }
  if (BusClockFreq) {
    Status = DwEmmcSetClock (BusClockFreq);
  }
  return Status;
}

BOOLEAN
DwEmmcIsMultiBlock (
  IN EFI_MMC_HOST_PROTOCOL      *This
  )
{
  return TRUE;
}

EFI_MMC_HOST_PROTOCOL gMciHost = {
  MMC_HOST_PROTOCOL_REVISION,
  DwEmmcIsCardPresent,
  DwEmmcIsReadOnly,
  DwEmmcBuildDevicePath,
  DwEmmcNotifyState,
  DwEmmcSendCommand,
  DwEmmcReceiveResponse,
  DwEmmcReadBlockData,
  DwEmmcWriteBlockData,
  DwEmmcSetIos,
  DwEmmcIsMultiBlock
};

EFI_STATUS
DwEmmcDxeInitialize (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS    Status;
  EFI_HANDLE    Handle;

  if (!FixedPcdGetBool (PcdDwPermitObsoleteDrivers)) {
    ASSERT (FALSE);
    return EFI_UNSUPPORTED;
  }

  Handle = NULL;
  DwEmmcDxeIoMux ();
  gpIdmacDesc = (DWEMMC_IDMAC_DESCRIPTOR *)AllocatePages (DWEMMC_MAX_DESC_PAGES);
  if (gpIdmacDesc == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }

  DEBUG ((DEBUG_BLKIO, "DwEmmcDxeInitialize()\n"));

  //Publish Component Name, BlockIO protocol interfaces
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEmbeddedMmcHostProtocolGuid,    &gMciHost,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}
