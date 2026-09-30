// Shared sceUsbd name-binding for every consumer of the ps5_usbd table.
//
// INCLUDE THIS BEFORE any header that declares the p_sceUsbd* pointers
// (ps5_usbd.h via usb_ps4.h): the bodies below were written against the
// PS4's link-time sceUsbd* API, and these macros keep those call sites
// untouched while binding to the runtime-resolved pointers.
//
// INVARIANT: after ps5_usbd.c binds the module, a consumer may ONLY call
// these names via this header's p_* mapping. ps5_usbd.c itself enforces
// this by including the same header before its own definition/dlsym passes
// (the #name stringification reads the macro ARGUMENT token, so the dlsym
// strings stay correct under the mapping).
#ifndef PS5_USBD_NAMES_H_
#define PS5_USBD_NAMES_H_

#define sceUsbdInit p_sceUsbdInit
#define sceUsbdExit p_sceUsbdExit
#define sceUsbdGetDeviceList p_sceUsbdGetDeviceList
#define sceUsbdFreeDeviceList p_sceUsbdFreeDeviceList
#define sceUsbdGetDeviceDescriptor p_sceUsbdGetDeviceDescriptor
#define sceUsbdGetDeviceSpeed p_sceUsbdGetDeviceSpeed
#define sceUsbdOpenDeviceWithVidPid p_sceUsbdOpenDeviceWithVidPid
#define sceUsbdClose p_sceUsbdClose
#define sceUsbdSetConfiguration p_sceUsbdSetConfiguration
#define sceUsbdClaimInterface p_sceUsbdClaimInterface
#define sceUsbdReleaseInterface p_sceUsbdReleaseInterface
#define sceUsbdSetInterfaceAltSetting p_sceUsbdSetInterfaceAltSetting
#define sceUsbdControlTransfer p_sceUsbdControlTransfer
#define sceUsbdBulkTransfer p_sceUsbdBulkTransfer
#define sceUsbdSubmitTransfer p_sceUsbdSubmitTransfer
#define sceUsbdCancelTransfer p_sceUsbdCancelTransfer
#define sceUsbdAllocTransfer p_sceUsbdAllocTransfer
#define sceUsbdFreeTransfer p_sceUsbdFreeTransfer
#define sceUsbdHandleEventsTimeout p_sceUsbdHandleEventsTimeout
#define sceUsbdGetActiveConfigDescriptor p_sceUsbdGetActiveConfigDescriptor
#define sceUsbdFreeConfigDescriptor p_sceUsbdFreeConfigDescriptor
#define sceUsbdGetMaxIsoPacketSize p_sceUsbdGetMaxIsoPacketSize
#define sceUsbdGetDevice p_sceUsbdGetDevice

#endif
