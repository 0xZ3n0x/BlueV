// SPDX-License-Identifier: GPL-3.0-or-later
/* usbprobe: asks libSceUsbd (the system's libusb) which USB devices there
 * are and whether the console's own kernel driver is attached to the
 * interfaces of a DualSense and of our Bluetooth adapter. It tells the
 * "communication method" setting of the controller (USB cable or Bluetooth)
 * apart, if the driver follows it. Nothing is written or claimed: the
 * devices are opened only to ask, then closed.
 *
 * libSceUsbd is libusb-1.0 with the default context:
 *   int  sceUsbdInit(void)                      libusb_init(NULL)
 *   long sceUsbdGetDeviceList(dev ***list)      count
 *   int  sceUsbdGetDeviceDescriptor(dev, desc*) 18 byte descriptor
 *   int  sceUsbdOpen(dev, handle **)
 *   int  sceUsbdKernelDriverActive(handle, interface)   1 = driver attached
 */
#include <libusb.h> /* structure layouts only, nothing is linked */
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int sceKernelLoadStartModule(const char* path, int argc, const char* argv, int flags, int* unk, int* unk2);

struct descriptor
{
    uint8_t length, type;
    uint16_t usb;
    uint8_t device_class, sub_class, protocol, max_packet0;
    uint16_t vendor, product, device;
    uint8_t manufacturer, product_string, serial, configurations;
} __attribute__((packed));

static int (*p_init)(void);
static void (*p_exit)(void);
static long (*p_list)(void*** list);
static void (*p_free_list)(void** list, int unref);
static int (*p_descriptor)(void* dev, struct descriptor* out);
static int (*p_open)(void* dev, void** handle);
static void (*p_close)(void* handle);
static int (*p_driver_active)(void* handle, int iface);
static int (*p_bus)(void* dev);
static int (*p_address)(void* dev);
static int (*p_active_config)(void* dev, struct libusb_config_descriptor** cfg);
static void (*p_free_config)(struct libusb_config_descriptor* cfg);
static int (*p_string_ascii)(void* handle, uint8_t index, uint8_t* data, int length);

static void* resolve(int h, const char* nid, const char* name)
{
    void* p = (void*)kernel_dynlib_resolve(getpid(), h, nid);

    if (p == NULL)
    {
        klog_printf("usbprobe: %s (%s) not found\n", name, nid);
    }
    return p;
}

static void dump_config(void* dev)
{
    struct libusb_config_descriptor* cfg = NULL;
    int r, i, a, e;

    if (!p_active_config)
    {
        return;
    }
    r = p_active_config(dev, &cfg);
    if (r != 0 || cfg == NULL)
    {
        klog_printf("usbprobe:   active configuration -> %d\n", r);
        return;
    }
    klog_printf("usbprobe:   configuration %d, %d interface(s), total %d bytes\n", cfg->bConfigurationValue,
                cfg->bNumInterfaces, cfg->wTotalLength);
    for (i = 0; i < cfg->bNumInterfaces; i++)
    {
        for (a = 0; a < cfg->interface[i].num_altsetting; a++)
        {
            const struct libusb_interface_descriptor* id = &cfg->interface[i].altsetting[a];

            klog_printf("usbprobe:     interface %d alt %d class %02x/%02x/%02x, %d endpoint(s)\n",
                        id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass, id->bInterfaceSubClass,
                        id->bInterfaceProtocol, id->bNumEndpoints);
            for (e = 0; e < id->bNumEndpoints; e++)
            {
                const struct libusb_endpoint_descriptor* ep = &id->endpoint[e];
                static const char* const type[4] = {"control", "isochronous", "bulk", "interrupt"};

                klog_printf("usbprobe:       endpoint %02x %s %s max %d interval %d\n", ep->bEndpointAddress,
                            type[ep->bmAttributes & 3], (ep->bEndpointAddress & 0x80) ? "in" : "out",
                            ep->wMaxPacketSize, ep->bInterval);
            }
        }
    }
    if (p_free_config)
    {
        p_free_config(cfg);
    }
}

int main(void)
{
    int dummy = 0, h, r, i, iface;
    void** list = NULL;
    long count;

    klog_printf("usbprobe: start uid=%d euid=%d\n", getuid(), geteuid());
    h = sceKernelLoadStartModule("/system/common/lib/libSceUsbd.sprx", 0, NULL, 0, NULL, &dummy);
    klog_printf("usbprobe: load libSceUsbd -> %d\n", h);
    if (h < 0)
    {
        return 1;
    }
    p_init = resolve(h, "TOhg7P6kTH4", "sceUsbdInit");
    p_exit = resolve(h, "Fq6+0Fm55xU", "sceUsbdExit");
    p_list = resolve(h, "8qB9Ar4P5nc", "sceUsbdGetDeviceList");
    p_free_list = resolve(h, "EQ6SCLMqzkM", "sceUsbdFreeDeviceList");
    p_descriptor = resolve(h, "bhomgbiQgeo", "sceUsbdGetDeviceDescriptor");
    p_open = resolve(h, "VJ6oMq-Di2U", "sceUsbdOpen");
    p_close = resolve(h, "HarYYlaFGJY", "sceUsbdClose");
    p_driver_active = resolve(h, "RLf56F-WjKQ", "sceUsbdKernelDriverActive");
    p_bus = resolve(h, "t7WE9mb1TB8", "sceUsbdGetBusNumber");
    p_address = resolve(h, "GjlCrU4GcIY", "sceUsbdGetDeviceAddress");
    p_active_config = resolve(h, "S1o1C6yOt5g", "sceUsbdGetActiveConfigDescriptor");
    p_free_config = resolve(h, "Hvd3S--n25w", "sceUsbdFreeConfigDescriptor");
    p_string_ascii = resolve(h, "t4gUfGsjk+g", "sceUsbdGetStringDescriptorAscii");
    if (!p_init || !p_list || !p_descriptor || !p_open || !p_close || !p_driver_active)
    {
        return 1;
    }

    r = p_init();
    klog_printf("usbprobe: sceUsbdInit -> %d\n", r);
    if (r != 0)
    {
        return 0;
    }
    count = p_list(&list);
    klog_printf("usbprobe: device list -> %ld\n", count);
    for (i = 0; i < count && list != NULL; i++)
    {
        struct descriptor d;
        void* handle = NULL;

        memset(&d, 0, sizeof d);
        r = p_descriptor(list[i], &d);
        klog_printf("usbprobe: device %d bus %d addr %d: descriptor -> %d "
                    "%04x:%04x class %02x configs %d\n",
                    i, p_bus ? p_bus(list[i]) : -1, p_address ? p_address(list[i]) : -1, r, d.vendor, d.product,
                    d.device_class, d.configurations);
        dump_config(list[i]);
        if (r != 0 || (d.vendor != 0x054c && d.vendor != 0x0a12 && d.vendor != 0x0e8d))
        {
            continue;
        }
        r = p_open(list[i], &handle);
        klog_printf("usbprobe:   open -> %d\n", r);
        if (r != 0 || handle == NULL)
        {
            continue;
        }
        if (d.vendor == 0x0a12 && p_string_ascii)
        { /* the adapter only */
            uint8_t text[64];
            const uint8_t idx[3] = {d.manufacturer, d.product_string, d.serial};
            static const char* const what[3] = {"manufacturer", "product", "serial"};
            int k;

            for (k = 0; k < 3; k++)
            {
                memset(text, 0, sizeof text);
                if (idx[k] == 0)
                {
                    klog_printf("usbprobe:   %s: no string (index 0)\n", what[k]);
                    continue;
                }
                r = p_string_ascii(handle, idx[k], text, sizeof text - 1);
                klog_printf("usbprobe:   %s string %d -> %d '%s'\n", what[k], idx[k], r,
                            r > 0 ? (const char*)text : "");
            }
        }
        for (iface = 0; iface < 6; iface++)
        {
            klog_printf("usbprobe:   interface %d kernel driver active -> %d\n", iface, p_driver_active(handle, iface));
        }
        p_close(handle);
    }
    if (list != NULL && p_free_list)
    {
        p_free_list(list, 1);
    }
    if (p_exit)
    {
        p_exit();
    }
    klog_printf("usbprobe: done\n");
    return 0;
}
