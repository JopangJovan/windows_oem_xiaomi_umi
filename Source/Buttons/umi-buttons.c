/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include "pmic-buttons.h"
/* Consumer volume increment/decrement (ID 1); Native System Control Power-Down (ID 2). */
static UCHAR descriptor[]={
 0x05,0x0c,0x09,1,0xa1,1,0x85,1,0x15,0,0x25,1,
 0x09,0xe9,0x09,0xea,0x75,1,0x95,2,0x81,2,
 0x75,6,0x95,1,0x81,3,0xc0,
 0x05,1,0x09,0x80,0xa1,1,0x85,2,0x09,0x81,
 0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
 0x75,7,0x95,1,0x81,3,0xc0};
typedef struct {
 UMI_BUTTON_BUS bus; VHFHANDLE vhf; WDFTIMER timer; PVOID sysState; volatile LONG running;
 UCHAR stable,candidate,armed,seenOr; BOOLEAN initial; ULONGLONG changed,lastPwr;
 ULONG errors,ticks,sends; NTSTATUS reportStatus,lastSampleStatus,lastS,lastT;
} CTX;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTX,GetCtx);
static void WriteDiag(CTX*c){
 WDFKEY k;
 if(NT_SUCCESS(WdfDeviceOpenRegistryKey(WdfTimerGetParentObject(c->timer),PLUGPLAY_REGKEY_DRIVER,KEY_WRITE,WDF_NO_OBJECT_ATTRIBUTES,&k))){
  UNICODE_STRING name;RtlInitUnicodeString(&name,L"UmiDiag");
  ULONG d[8]={c->ticks,c->errors,(ULONG)c->lastSampleStatus,((ULONG)c->seenOr<<8)|c->stable,c->sends,(ULONG)c->lastS,(ULONG)c->lastT,(ULONG)c->reportStatus};
  WdfRegistryAssignValue(k,&name,REG_BINARY,sizeof(d),d);
  WdfRegistryClose(k);
 }
}
static void Send(CTX*c,UCHAR mask){
 /* Only submit the HID collection whose bits actually changed (prevents spurious volume report from waking display on power release) */
 UCHAR diff=mask^c->stable;
 NTSTATUS s=STATUS_SUCCESS,t=STATUS_SUCCESS;
 if(diff&6){
  UCHAR volume[2]={1,(UCHAR)((mask>>1)&3)};
  HID_XFER_PACKET p={volume,sizeof(volume),1};s=VhfReadReportSubmit(c->vhf,&p);
 }
 if((diff&1)&&(mask&1)){
  ULONGLONG now=KeQueryInterruptTime();
  if(now-c->lastPwr>=5000000ULL){ /* 500ms cooldown against mechanical switch bounce */
   c->lastPwr=now;
   /* Atomic press+release pulse on power press so physical button release does not wake display back up */
   UCHAR pdown[2]={2,1},pup[2]={2,0};
   HID_XFER_PACKET p={pdown,sizeof(pdown),2};t=VhfReadReportSubmit(c->vhf,&p);
   KeStallExecutionProcessor(5000);
   p.reportBuffer=pup;VhfReadReportSubmit(c->vhf,&p);
  }
 }
 c->sends++;c->lastS=s;c->lastT=t;
 c->reportStatus=NT_SUCCESS(s)?t:s;
}
EVT_WDF_TIMER Tick;
void Tick(WDFTIMER timer){CTX*c=GetCtx(WdfTimerGetParentObject(timer));if(!c->running)return;
 UCHAR mask;NTSTATUS s=ButtonSample(&c->bus,&mask);ULONGLONG now=KeQueryInterruptTime();
 c->ticks++;c->lastSampleStatus=s;
 if(NT_SUCCESS(s)){
  c->errors=0;c->seenOr|=mask;
  if(!c->initial){c->initial=TRUE;c->candidate=mask;c->changed=now;c->armed=(~mask)&7;}
  else if(mask!=c->candidate){c->candidate=mask;c->changed=now;}
  else if(now-c->changed>=300000){ /* 30ms stable */
   c->armed|=(~mask)&7;UCHAR next=mask&c->armed;
   if(next!=c->stable){DbgPrintEx(DPFLTR_IHVDRIVER_ID,DPFLTR_ERROR_LEVEL,"UMI buttons state=%u\n",next);Send(c,next);c->stable=next;WriteDiag(c);}
  }
 }else if(++c->errors==3){Send(c,0);c->stable=0;c->initial=FALSE;WriteDiag(c);}
 if((c->ticks%100)==1)WriteDiag(c);
 if(c->running)WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(10));
}
static void Unmap(CTX*c){
 if(c->bus.core)MmUnmapIoSpace(c->bus.core,0x2000);
 if(c->bus.owners)MmUnmapIoSpace(c->bus.owners,0x1000);
 if(c->bus.observer)MmUnmapIoSpace(c->bus.observer,0x10000);
 RtlZeroMemory(&c->bus,sizeof(c->bus));
}
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
NTSTATUS Prepare(WDFDEVICE d,WDFCMRESLIST raw,WDFCMRESLIST tr){UNREFERENCED_PARAMETER(raw);CTX*c=GetCtx(d);
 for(ULONG i=0;i<WdfCmResourceListGetCount(tr);i++){
  PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(tr,i);if(r->Type!=CmResourceTypeMemory)continue;
  PUCHAR*dest=NULL;ULONG length=0;
  if(r->u.Memory.Start.QuadPart==0xc440000&&r->u.Memory.Length==0x2000){dest=&c->bus.core;length=0x2000;}
  if(r->u.Memory.Start.QuadPart==0xc40a000&&r->u.Memory.Length==0x1000){dest=&c->bus.owners;length=0x1000;}
  if(r->u.Memory.Start.QuadPart==0xe600000&&r->u.Memory.Length==0x10000){dest=&c->bus.observer;length=0x10000;}
  if(dest){if(*dest){Unmap(c);return STATUS_DEVICE_CONFIGURATION_ERROR;}*dest=MmMapIoSpace(r->u.Memory.Start,length,MmNonCached);}
 }
 if(!c->bus.core||!c->bus.owners||!c->bus.observer){Unmap(c);return STATUS_DEVICE_CONFIGURATION_ERROR;}return STATUS_SUCCESS;
}
EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
NTSTATUS Release(WDFDEVICE d,WDFCMRESLIST r){UNREFERENCED_PARAMETER(r);Unmap(GetCtx(d));return STATUS_SUCCESS;}
EVT_WDF_DEVICE_D0_ENTRY Enter;
NTSTATUS Enter(WDFDEVICE d,WDF_POWER_DEVICE_STATE prev){UNREFERENCED_PARAMETER(prev);CTX*c=GetCtx(d);NTSTATUS s=ButtonInitialize(&c->bus);if(!NT_SUCCESS(s))return s;
 /* Hold ES_SYSTEM_REQUIRED (without ES_DISPLAY_REQUIRED) so Modern Standby turns off the AMOLED cleanly on Power press without entering DRIPS/S0ix (which hard-reboots SM8250 without qcpep) */
 if(!c->sysState)c->sysState=PoRegisterSystemState(NULL,ES_SYSTEM_REQUIRED|ES_CONTINUOUS);
 c->initial=FALSE;c->stable=0;c->errors=0;InterlockedExchange(&c->running,TRUE);WdfTimerStart(c->timer,WDF_REL_TIMEOUT_IN_MS(10));return STATUS_SUCCESS;}
EVT_WDF_DEVICE_D0_EXIT Exit;
NTSTATUS Exit(WDFDEVICE d,WDF_POWER_DEVICE_STATE next){UNREFERENCED_PARAMETER(next);CTX*c=GetCtx(d);InterlockedExchange(&c->running,FALSE);WdfTimerStop(c->timer,TRUE);Send(c,0);return STATUS_SUCCESS;}
EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
void Cleanup(WDFOBJECT d){CTX*c=GetCtx(d);if(c->sysState){PoUnregisterSystemState(c->sysState);c->sysState=NULL;}if(c->vhf){VhfDelete(c->vhf,TRUE);c->vhf=NULL;}}
EVT_WDF_DRIVER_DEVICE_ADD Add;
NTSTATUS Add(WDFDRIVER driver,PWDFDEVICE_INIT init){UNREFERENCED_PARAMETER(driver);
 WDF_PNPPOWER_EVENT_CALLBACKS cb;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);cb.EvtDevicePrepareHardware=Prepare;cb.EvtDeviceReleaseHardware=Release;cb.EvtDeviceD0Entry=Enter;cb.EvtDeviceD0Exit=Exit;WdfDeviceInitSetPnpPowerEventCallbacks(init,&cb);
 WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,CTX);a.ExecutionLevel=WdfExecutionLevelPassive;a.EvtCleanupCallback=Cleanup;
 WDFDEVICE d;NTSTATUS s=WdfDeviceCreate(&init,&a,&d);if(!NT_SUCCESS(s))return s;CTX*c=GetCtx(d);
 WDF_TIMER_CONFIG tc;WDF_TIMER_CONFIG_INIT(&tc,Tick);tc.AutomaticSerialization=FALSE;WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=d;a.ExecutionLevel=WdfExecutionLevelPassive;
 s=WdfTimerCreate(&tc,&a,&c->timer);if(!NT_SUCCESS(s))return s;
 VHF_CONFIG v;VHF_CONFIG_INIT(&v,WdfDeviceWdmGetDeviceObject(d),sizeof(descriptor),descriptor);v.VhfClientContext=c;v.VendorID=0x0483;v.ProductID=0x0010;v.VersionNumber=1;
 s=VhfCreate(&v,&c->vhf);return NT_SUCCESS(s)?VhfStart(c->vhf):s;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT d,PUNICODE_STRING path){WDF_DRIVER_CONFIG c;WDF_DRIVER_CONFIG_INIT(&c,Add);return WdfDriverCreate(d,path,WDF_NO_OBJECT_ATTRIBUTES,&c,WDF_NO_HANDLE);}
