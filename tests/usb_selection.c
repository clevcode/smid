/* Descriptor/claim tests use fake handles; no physical USB access. */
#include "usb.c"
#include <assert.h>

static int claimed = -1, released = -1, alt_interface = -1, alt_value = -1;
static int current_config = 1, config_writes;
static struct libusb_endpoint_descriptor endpoints[] = {
    {.bEndpointAddress = 0x81, .bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT},
    {.bEndpointAddress = 0x01, .bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT},
    {.bEndpointAddress = 0x02, .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK},
};
static struct libusb_interface_descriptor alternatives[] = {
    {.bInterfaceNumber = 4, .bAlternateSetting = 0, .bInterfaceClass = 255},
    {.bInterfaceNumber = 4, .bAlternateSetting = 1, .bInterfaceClass = 255,
     .bNumEndpoints = 3, .endpoint = endpoints},
};
static struct libusb_interface interfaces[] = {
    {.altsetting = alternatives, .num_altsetting = 2},
};
static struct libusb_config_descriptor config = {.bNumInterfaces = 1, .interface = interfaces};

libusb_device *__wrap_libusb_get_device(libusb_device_handle *h) { (void)h; return NULL; }
int __wrap_libusb_get_config_descriptor_by_value(libusb_device *d, uint8_t v,
                                                struct libusb_config_descriptor **out) {
    (void)d; assert(v == 1); *out = &config; return 0;
}
void __wrap_libusb_free_config_descriptor(struct libusb_config_descriptor *c) { assert(c == &config); }
int __wrap_libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int on) {
    (void)h; assert(on == 1); return 0;
}
int __wrap_libusb_get_configuration(libusb_device_handle *h, int *value) {
    (void)h; *value = current_config; return 0;
}
int __wrap_libusb_set_configuration(libusb_device_handle *h, int value) {
    (void)h; (void)value; config_writes++; return 0;
}
int __wrap_libusb_claim_interface(libusb_device_handle *h, int iface) {
    (void)h; claimed = iface; return 0;
}
int __wrap_libusb_set_interface_alt_setting(libusb_device_handle *h, int iface, int alt) {
    (void)h; alt_interface = iface; alt_value = alt; return 0;
}
int __wrap_libusb_release_interface(libusb_device_handle *h, int iface) {
    (void)h; released = iface; return 0;
}
void __wrap_libusb_close(libusb_device_handle *h) { (void)h; }

int main(void) {
    struct smid_usb u = {.product_id = 0x0768, .interface_number = 4,
                         .handle = (libusb_device_handle *)(uintptr_t)1};
    assert(configure_handle(&u) == 0);
    assert(claimed == 4 && alt_interface == 4 && alt_value == 1 && config_writes == 0);
    close_handle_only(&u);
    assert(released == 4 && !u.handle && !u.iface_claimed);
    claimed = -1;
    u.handle = (libusb_device_handle *)(uintptr_t)1;
    current_config = 2;
    assert(configure_handle(&u) != 0 && config_writes == 0 && claimed == -1);
    current_config = 1;
    alternatives[1].bInterfaceNumber = 0;
    assert(configure_handle(&u) != 0 && claimed == -1);
    alternatives[1].bInterfaceNumber = 4;
    alternatives[1].bInterfaceClass = LIBUSB_CLASS_AUDIO;
    assert(configure_handle(&u) != 0 && claimed == -1);
    alternatives[1].bInterfaceClass = LIBUSB_CLASS_VENDOR_SPEC;
    endpoints[2].bmAttributes = LIBUSB_TRANSFER_TYPE_INTERRUPT;
    assert(configure_handle(&u) != 0 && claimed == -1);
    endpoints[2].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
    u.product_id = 0x0760;
    u.interface_number = alternatives[1].bInterfaceNumber = 0;
    assert(configure_handle(&u) == 0 && claimed == 0 && alt_interface == 0);
    assert(config_writes == 0);
    puts("PASS: 0768 display interface 4, descriptor rejection, release, 0760 interface 0");
    return 0;
}
