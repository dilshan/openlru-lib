/**
 * @file olru_usb.c
 * @brief Linux usbfs backend: finds sensors and does USB transfers.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Uses only the kernel's /dev/bus/usb interface (ioctl), so the library
 * needs no libusb.  Devices are discovered through /sys/bus/usb/devices.
 *
 * Non-root access needs a udev rule, see udev/99-openlru.rules.
 */

#include "olru_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SYSFS_USB_DIR   "/sys/bus/usb/devices"
#define INTERFACE_NUM   0
#define VENDOR_IN       0xC0           /* control: device-to-host, vendor */
#define MAX_URB_BYTES   16384          /* keep each bulk ioctl small      */
#define RESET_SETTLE_US 200000         /* let the sensor restart          */

/* ------------------------------------------------------------------ */
/* Known sensors                                                      */
/* ------------------------------------------------------------------ */

/** Models handled by the vendor SDK (see OpenLibUSB in libfpengine). */
static const olru_device_info known_models[] = {
    { 0x0453, 0x9005, 0, 0, OLRU_LINK_BULK, 0, "LRU USB fingerprint reader" },
    { 0x0618, 0x9018, 0, 0, OLRU_LINK_BULK, 1, "LRU USB fingerprint reader (alt opcodes)" },
    { 0x2009, 0x7638, 0, 0, OLRU_LINK_MSC,  0, "LRU USB fingerprint reader (MSC)" },
    { 0x2109, 0x7638, 0, 0, OLRU_LINK_MSC,  0, "LRU USB fingerprint reader (MSC)" },
};

/**
 * The mass-storage IDs (2009:7638, 2109:7638) enumerate as "USBKey Module"
 * and are also used by some ordinary USB storage products.  They are
 * enabled by default; set OLRU_DENY_MSC=1 to hide them if you share the bus
 * with such a drive.
 */
static int model_allowed(const olru_device_info *model)
{
    return model->link != OLRU_LINK_MSC || getenv("OLRU_DENY_MSC") == NULL;
}

/** @return the table entry for vid:pid, or NULL if unsupported/not allowed. */
static const olru_device_info *find_model(unsigned vid, unsigned pid)
{
    for (size_t i = 0; i < sizeof known_models / sizeof known_models[0]; i++)
        if (known_models[i].vid == vid && known_models[i].pid == pid &&
            model_allowed(&known_models[i]))
            return &known_models[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* sysfs scanning                                                     */
/* ------------------------------------------------------------------ */

/** Read one numeric sysfs attribute (hex or decimal) of a USB device. */
static int read_attr(const char *dev, const char *attr, int base, long *out)
{
    char path[320], text[32];
    FILE *f;
    int ok;

    snprintf(path, sizeof path, SYSFS_USB_DIR "/%s/%s", dev, attr);
    f = fopen(path, "r");
    if (!f)
        return 0;
    ok = fgets(text, sizeof text, f) != NULL;
    fclose(f);
    if (ok)
        *out = strtol(text, NULL, base);
    return ok;
}

/** Fill @p info for sysfs entry @p name; returns 1 if it is a known sensor. */
static int load_entry(const char *name, olru_device_info *info)
{
    long vid, pid, bus, addr;
    const olru_device_info *model;

    if (!read_attr(name, "idVendor", 16, &vid) ||
        !read_attr(name, "idProduct", 16, &pid))
        return 0;
    model = find_model((unsigned)vid, (unsigned)pid);
    if (!model || !read_attr(name, "busnum", 10, &bus) ||
        !read_attr(name, "devnum", 10, &addr))
        return 0;
    *info = *model;
    info->bus = (uint8_t)bus;
    info->address = (uint8_t)addr;
    return 1;
}

int olru_usb_scan(olru_device_info *list, size_t max)
{
    DIR *dir = opendir(SYSFS_USB_DIR);
    struct dirent *entry;
    size_t count = 0;

    if (!dir)
        return OLRU_ERR_IO;
    while ((entry = readdir(dir)) != NULL && count < max) {
        if (entry->d_name[0] == '.' || strchr(entry->d_name, ':'))
            continue;                    /* skip ".", ".." and interfaces */
        if (load_entry(entry->d_name, &list[count]))
            count++;
    }
    closedir(dir);
    return (int)count;
}

/* ------------------------------------------------------------------ */
/* Transfers                                                          */
/* ------------------------------------------------------------------ */

struct usbfs_ctx {
    int fd;                              /* open /dev/bus/usb/BBB/DDD */
};

/** Translate errno from a failed ioctl into an olru_status. */
static int map_errno(int err)
{
    switch (err) {
    case ETIMEDOUT: return OLRU_ERR_TIMEOUT;
    case ENODEV:
    case ESHUTDOWN: return OLRU_ERR_NO_DEVICE;
    case EACCES:
    case EPERM:     return OLRU_ERR_ACCESS;
    default:        return OLRU_ERR_IO;
    }
}

static int usbfs_control_in(void *ctx, uint8_t request, uint16_t value,
                            uint8_t *buf, uint16_t len)
{
    struct usbfs_ctx *c = ctx;
    struct usbdevfs_ctrltransfer ct = {
        .bRequestType = VENDOR_IN,
        .bRequest     = request,
        .wValue       = value,
        .wIndex       = 0,
        .wLength      = len,
        .timeout      = OLRU_TIMEOUT_MS,
        .data         = buf,
    };
    int n = ioctl(c->fd, USBDEVFS_CONTROL, &ct);

    return n < 0 ? map_errno(errno) : n;
}

/** One bulk ioctl of at most MAX_URB_BYTES. */
static int bulk_chunk(struct usbfs_ctx *c, uint8_t ep, void *buf, size_t len)
{
    struct usbdevfs_bulktransfer bt = {
        .ep      = ep,
        .len     = (unsigned)len,
        .timeout = OLRU_TIMEOUT_MS,
        .data    = buf,
    };
    int n = ioctl(c->fd, USBDEVFS_BULK, &bt);

    return n < 0 ? map_errno(errno) : n;
}

/** Bulk transfer in MAX_URB_BYTES pieces; stops early on a short packet. */
static int bulk_all(struct usbfs_ctx *c, uint8_t ep, uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        size_t want = len - done < MAX_URB_BYTES ? len - done : MAX_URB_BYTES;
        int n = bulk_chunk(c, ep, buf + done, want);

        if (n < 0)
            return n;
        done += (size_t)n;
        if ((size_t)n < want)
            break;                       /* short packet = end of data */
    }
    return (int)done;
}

static int usbfs_bulk_out(void *ctx, uint8_t ep, const uint8_t *buf, size_t len)
{
    return bulk_all(ctx, ep, (uint8_t *)buf, len);
}

static int usbfs_bulk_in(void *ctx, uint8_t ep, uint8_t *buf, size_t len)
{
    return bulk_all(ctx, ep, buf, len);
}

static void usbfs_close(void *ctx)
{
    struct usbfs_ctx *c = ctx;
    unsigned iface = INTERFACE_NUM;

    ioctl(c->fd, USBDEVFS_RELEASEINTERFACE, &iface);
    close(c->fd);
    free(c);
}

static const struct olru_usb_ops usbfs_ops = {
    .control_in = usbfs_control_in,
    .bulk_out   = usbfs_bulk_out,
    .bulk_in    = usbfs_bulk_in,
    .close      = usbfs_close,
};

/* ------------------------------------------------------------------ */
/* Opening                                                            */
/* ------------------------------------------------------------------ */

/** Claim interface 0, detaching any kernel driver (e.g. usb-storage). */
static int claim_interface(int fd)
{
    struct usbdevfs_disconnect_claim dc = { .interface = INTERFACE_NUM };
    unsigned iface = INTERFACE_NUM;

    if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc) == 0)
        return OLRU_OK;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) == 0)
        return OLRU_OK;
    return map_errno(errno);
}

int olru_usb_open(const olru_device_info *info,
                  const struct olru_usb_ops **ops, void **ctx)
{
    char path[64];
    struct usbfs_ctx *c;
    int rc;

    snprintf(path, sizeof path, "/dev/bus/usb/%03u/%03u",
             info->bus, info->address);
    c = malloc(sizeof *c);
    if (!c)
        return OLRU_ERR_NOMEM;
    c->fd = open(path, O_RDWR | O_CLOEXEC);
    rc = c->fd < 0 ? map_errno(errno) : claim_interface(c->fd);
    if (rc != OLRU_OK) {
        if (c->fd >= 0)
            close(c->fd);
        free(c);
        return rc;
    }
    if (info->link == OLRU_LINK_BULK) {     /* the vendor SDK resets these */
        ioctl(c->fd, USBDEVFS_RESET, 0);
        usleep(RESET_SETTLE_US);
    }
    *ops = &usbfs_ops;
    *ctx = c;
    return OLRU_OK;
}
