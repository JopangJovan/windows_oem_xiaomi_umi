/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UMI PM8150 button observer transport. No PMIC configuration writes.
 * Caller owns mapped resources and serializes observer-channel access.
 */
#pragma once
#include <ntddk.h>
typedef struct {
 PUCHAR core,owners,observer;
 USHORT pon,gpio;
 BOOLEAN volumeUpValid;
} UMI_BUTTON_BUS;
#define UMI_NO_APID 0xffff
static ULONG ButtonReg(PUCHAR p,ULONG o){return READ_REGISTER_ULONG((PULONG)(p+o));}
static USHORT ButtonFind(UMI_BUTTON_BUS*b,USHORT ppid,ULONG channels){
 USHORT found=UMI_NO_APID;
 for(ULONG i=0;i<channels;i++){
  ULONG m=ButtonReg(b->core,0x900+4*i);
  if(m&&((m>>8)&0xfff)==ppid){ULONG owner=ButtonReg(b->owners,0x700+4*i)&7;
   if(found==UMI_NO_APID||owner==0)found=(USHORT)i;
  }
 }
 return found;
}
static NTSTATUS ButtonRead(UMI_BUTTON_BUS*b,USHORT apid,USHORT address,UCHAR*v){
 if(apid==UMI_NO_APID||apid>=512)return STATUS_DEVICE_NOT_READY;
 PUCHAR channel=b->observer+0x80*apid;
 ULONGLONG limit=KeQueryInterruptTime()+20000; /* 2ms */
 WRITE_REGISTER_ULONG((PULONG)channel,(1u<<27)|((address&255u)<<4));
 do{
  ULONG s=ButtonReg(channel,8);
  if(s&1){if(s&14)return STATUS_IO_DEVICE_ERROR;*v=(UCHAR)ButtonReg(channel,0x18);return STATUS_SUCCESS;}
  KeStallExecutionProcessor(1);
 }while(KeQueryInterruptTime()<limit);
 return STATUS_IO_TIMEOUT;
}
static NTSTATUS ButtonInitialize(UMI_BUTTON_BUS*b){
 ULONG version=ButtonReg(b->core,0),channels=ButtonReg(b->core,4)&0x7ff;
 if(version<0x50000000||version>=0x70000000||!channels||channels>512)return STATUS_NOT_SUPPORTED;
 b->pon=ButtonFind(b,8,channels);b->gpio=ButtonFind(b,0xc5,channels);b->volumeUpValid=FALSE;
 if(b->pon==UMI_NO_APID)return STATUS_DEVICE_NOT_READY;
 UCHAR mode,enable,subtype;
 if(NT_SUCCESS(ButtonRead(b,b->gpio,0xc140,&mode))&&
    NT_SUCCESS(ButtonRead(b,b->gpio,0xc146,&enable))&&
    NT_SUCCESS(ButtonRead(b,b->gpio,0xc105,&subtype))){
  ULONG direction=mode&3;
  b->volumeUpValid=subtype>=0x10&&subtype<=0x13&&(enable&0x80)&&(direction==0||direction==2);
 }
 return b->volumeUpValid?STATUS_SUCCESS:STATUS_DEVICE_CONFIGURATION_ERROR;
}
/* Bit0 power, bit1 volume-up, bit2 volume-down. Failures are not releases. */
static NTSTATUS ButtonSample(UMI_BUTTON_BUS*b,UCHAR*mask){
 UCHAR pon,gpio;NTSTATUS s=ButtonRead(b,b->pon,0x0810,&pon);if(!NT_SUCCESS(s))return s;
 s=ButtonRead(b,b->gpio,0xc110,&gpio);if(!NT_SUCCESS(s))return s;
 *mask=(pon&1)|((gpio&1)?0:2)|((pon&2)?4:0);return STATUS_SUCCESS;
}
