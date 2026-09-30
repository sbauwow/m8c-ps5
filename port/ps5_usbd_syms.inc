// The sceUsbd symbol list, in one place.
// Used with the X-macro in ps5_usbd.h / ps5_usbd.c:
//   USBD_DECL(ret, name, (params))
// Signatures = FreeBSD libusb-1.0 (both consoles' modules clone it).
#if !defined(USBD_DECL) && !defined(USBD_DEF)
#error "include ps5_usbd.h, not ps5_usbd_syms.inc directly"
#endif

USBD_DECL(int, sceUsbdInit, (void))
USBD_DECL(void, sceUsbdExit, (void))
USBD_DECL(int, sceUsbdGetDeviceList, (libusb_device ***))
USBD_DECL(void, sceUsbdFreeDeviceList, (libusb_device **))
USBD_DECL(int, sceUsbdGetDeviceDescriptor, (libusb_device *, struct libusb_device_descriptor *))
USBD_DECL(int, sceUsbdGetDeviceSpeed, (libusb_device *))
USBD_DECL(libusb_device_handle *, sceUsbdOpenDeviceWithVidPid, (uint16_t, uint16_t))
USBD_DECL(int, sceUsbdClose, (libusb_device_handle *))
USBD_DECL(int, sceUsbdSetConfiguration, (libusb_device_handle *, int))
USBD_DECL(int, sceUsbdClaimInterface, (libusb_device_handle *, int))
USBD_DECL(int, sceUsbdReleaseInterface, (libusb_device_handle *, int))
USBD_DECL(int, sceUsbdSetInterfaceAltSetting, (libusb_device_handle *, int, int))
USBD_DECL(int, sceUsbdControlTransfer, (libusb_device_handle *, uint8_t, uint8_t, uint16_t,
                                        uint16_t, unsigned char *, uint16_t, unsigned int))
USBD_DECL(int, sceUsbdBulkTransfer, (libusb_device_handle *, unsigned char, unsigned char *, int,
                                     int *, unsigned int))
USBD_DECL(int, sceUsbdSubmitTransfer, (struct libusb_transfer *))
USBD_DECL(int, sceUsbdCancelTransfer, (struct libusb_transfer *))
USBD_DECL(struct libusb_transfer *, sceUsbdAllocTransfer, (int))
USBD_DECL(void, sceUsbdFreeTransfer, (struct libusb_transfer *))
USBD_DECL(int, sceUsbdHandleEventsTimeout, (struct timeval *))
USBD_DECL(int, sceUsbdGetActiveConfigDescriptor, (libusb_device *,
                                                  struct libusb_config_descriptor **))
USBD_DECL(void, sceUsbdFreeConfigDescriptor, (struct libusb_config_descriptor *))
USBD_DECL(int, sceUsbdGetMaxIsoPacketSize, (libusb_device *, unsigned char))
USBD_DECL(libusb_device *, sceUsbdGetDevice, (libusb_device_handle *))
