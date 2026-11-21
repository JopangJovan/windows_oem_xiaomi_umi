/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include "umi_pcie.h"
#include "umi_pcie_status.h"
#include "UmiWlanRpmh.inc"

static uint32_t ReadRegister(void *Context, uint32_t Address)
{
  (VOID)Context;
  return MmioRead32(Address);
}

static void WriteRegister(void *Context, uint32_t Address, uint32_t Value)
{
  (VOID)Context;
  MmioWrite32(Address, Value);
  MemoryFence();
}

static void Delay(void *Context, unsigned Microseconds)
{
  (VOID)Context;
  MicroSecondDelay(Microseconds);
}

static void Report(void *Context, const char *Label, uint32_t Value)
{
  EFI_FILE_PROTOCOL *Log = Context;
  CHAR8 Text[128];
  UINTN Length;
  if (Log == NULL) return;
  Length = AsciiSPrint(Text, sizeof(Text), "%a: %08x\r\n", Label, Value);
  Log->Write(Log, &Length, Text);
  Log->Flush(Log);
}

static int PowerOn(void *Context)
{
  EFI_STATUS Status = EnableWlanRails();
  if (EFI_ERROR(Status)) Report(Context, "RPMh rail index (zero based)", (UINT32)mWlanRailIndex);
  Report(Context, "RPMh status", (UINT32)Status);
  return EFI_ERROR(Status) ? EIO : 0;
}

static EFI_HANDLE FindWindowsEsp(VOID)
{
  EFI_HANDLE *Handles;
  EFI_HANDLE Esp = NULL;
  UINTN Count;
  EFI_STATUS Status = gBS->LocateHandleBuffer(ByProtocol,
    &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return NULL;

  for (UINTN Index = 0; Index < Count; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    EFI_FILE_PROTOCOL *Root, *Tag;

    Status = gBS->HandleProtocol(Handles[Index],
      &gEfiSimpleFileSystemProtocolGuid, (VOID **)&FileSystem);
    if (EFI_ERROR(Status) || EFI_ERROR(FileSystem->OpenVolume(FileSystem, &Root)))
      continue;

    Status = Root->Open(Root, &Tag, L"\\UMI_ESP_TARGET.TAG", EFI_FILE_MODE_READ, 0);
    Root->Close(Root);
    if (EFI_ERROR(Status)) continue;

    Tag->Close(Tag);
    Esp = Handles[Index];
    break;
  }

  gBS->FreePool(Handles);
  return Esp;
}

EFI_STATUS EFIAPI UmiWlanMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_FILE_PROTOCOL *Root, *Log, *Marker;
  BOOLEAN LeaveLinkUp, BootWindows;
  EFI_STATUS Status;
  EFI_HANDLE Next = NULL;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  struct umi_pcie Host = {0};
  struct umi_pcie_io Io = { NULL, ReadRegister, WriteRegister, PowerOn, Delay, Report };

  (VOID)SystemTable;
  Status = gBS->HandleProtocol(Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->HandleProtocol(Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid,
                               (VOID **)&FileSystem);
  if (EFI_ERROR(Status)) return Status;
  Status = FileSystem->OpenVolume(FileSystem, &Root);
  if (EFI_ERROR(Status)) return Status;
  LeaveLinkUp = !EFI_ERROR(Root->Open(Root, &Marker, L"\\umi-wlan-enumerate",
                                     EFI_FILE_MODE_READ, 0));
  if (LeaveLinkUp) Marker->Close(Marker);
  BootWindows = !EFI_ERROR(Root->Open(Root, &Marker, L"\\umi-wlan-win11",
                                      EFI_FILE_MODE_READ, 0));
  if (BootWindows) Marker->Close(Marker);
  Status = Root->Open(Root, &Log, L"\\umi-wlan-uefi.log",
                      EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
  Root->Close(Root);
  if (EFI_ERROR(Status)) return Status;
  Io.context = Log;
  Report(Log, "PCIe0 probe start", 1);
  int Result = umi_pcie_enumerate(&Host, &Io);
  Report(Log, "PCIe0 probe result", (UINT32)Result);
  /* The enumeration probe owns no Windows IOMMU/MSI configuration. */
  if (Result == 0) {
    Result = umi_pcie_prepare_stream(&Host);
    Report(Log, "PCIe0 stream setup result", (UINT32)Result);
    if (Result != 0 || !LeaveLinkUp) umi_pcie_quiesce(&Host);
    else Report(Log, "Leaving PCIe link up with bus mastering off", 1);
  }
  Log->Close(Log);

  EFI_HANDLE Target = BootWindows ? FindWindowsEsp() : Loaded->DeviceHandle;
  if (Target == NULL) return EFI_NOT_FOUND;
  Path = FileDevicePath(Target, BootWindows ? L"\\EFI\\BOOT\\bootaa64.efi"
                                            : L"\\EFI\\BOOT\\bootaa64.winpe");
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  Status = gBS->LoadImage(FALSE, Image, Path, NULL, 0, &Next);
  return EFI_ERROR(Status) ? Status : gBS->StartImage(Next, NULL, NULL);
}
