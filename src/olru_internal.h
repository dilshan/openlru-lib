/**
 * @file olru_internal.h
 * @brief Private declarations shared by the library's source files.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Layering (top to bottom):
 *
 *     openlru.c      API: commands such as "capture", "read image"
 *        |
 *     olru_packet.c  builds / parses the sensor's 0xEF01 command packets
 *        |
 *     olru_link.c    moves bytes over USB (BULK or MSC flavour)
 *        |
 *     olru_usb.c     Linux usbfs backend (control + bulk transfers)
 *
 */

#ifndef OLRU_INTERNAL_H
#define OLRU_INTERNAL_H

#include "openlru.h"

/* ------------------------------------------------------------------ */
/* USB backend interface                                               */
/* ------------------------------------------------------------------ */

#define OLRU_TIMEOUT_MS   5000   /**< Per USB transfer timeout.            */
#define OLRU_EP_IN        0x81   /**< Bulk IN endpoint (all models).       */
#define OLRU_EP_OUT_BULK  0x02   /**< Bulk OUT endpoint, OLRU_LINK_BULK.   */
#define OLRU_EP_OUT_MSC   0x01   /**< Bulk OUT endpoint, OLRU_LINK_MSC.    */

/**
 * Operations a USB backend must provide.  Every call returns the number of
 * bytes transferred (>= 0) or a negative olru_status.  A bulk transfer may
 * return fewer bytes than requested when the sensor ends with a short packet.
 */
struct olru_usb_ops {
    int  (*control_in)(void *ctx, uint8_t request, uint16_t value,
                       uint8_t *buf, uint16_t len);
    int  (*bulk_out)(void *ctx, uint8_t ep, const uint8_t *buf, size_t len);
    int  (*bulk_in)(void *ctx, uint8_t ep, uint8_t *buf, size_t len);
    void (*close)(void *ctx);
};

/** The real backend: Linux usbfs (olru_usb.c). */
int olru_usb_scan(olru_device_info *list, size_t max);
int olru_usb_open(const olru_device_info *info,
                  const struct olru_usb_ops **ops, void **ctx);

/* ------------------------------------------------------------------ */
/* Device handle                                                       */
/* ------------------------------------------------------------------ */

struct olru_device {
    const struct olru_usb_ops *ops;   /**< USB backend in use.            */
    void          *ctx;               /**< Backend private data.          */
    olru_link_type link;              /**< BULK or MSC framing.           */
    uint8_t        alt_opcodes;       /**< Alternate opcode set in use.   */
    uint8_t        last_code;         /**< Last confirmation code.        */
};

/** Create a handle around an already opened backend. */
olru_device *olru_device_create(const struct olru_usb_ops *ops, void *ctx,
                                olru_link_type link, uint8_t alt_opcodes);

/* ------------------------------------------------------------------ */
/* Packet layer (olru_packet.c)                                        */
/* ------------------------------------------------------------------ */

#define OLRU_PID_COMMAND  0x01   /**< Packet id: host -> sensor command. */
#define OLRU_PID_ACK      0x07   /**< Packet id: sensor -> host reply.   */
#define OLRU_PACKET_MAX   64     /**< Largest packet this library builds. */
#define OLRU_ACK_MIN      12     /**< Smallest valid reply packet.       */

/**
 * Build a command packet:
 *   EF 01 | FF FF FF FF | pid | len(2) | payload | checksum(2)
 * @return Total packet length, or 0 if @p out_size is too small.
 */
size_t olru_packet_build(uint8_t *out, size_t out_size, uint8_t pid,
                         const uint8_t *payload, size_t payload_len);

/**
 * Check a reply packet and extract its confirmation code.
 * @return OLRU_OK or OLRU_ERR_PROTOCOL.
 */
int olru_packet_parse_ack(const uint8_t *in, size_t len, uint8_t *code);

/* ------------------------------------------------------------------ */
/* Link layer (olru_link.c)                                            */
/* ------------------------------------------------------------------ */

/** Send @p len bytes to the sensor. */
int olru_link_write(olru_device *dev, const uint8_t *data, size_t len);

/** Read up to @p len bytes; returns bytes received (short reads allowed). */
int olru_link_read(olru_device *dev, uint8_t *buf, size_t len);

/** Read exactly @p total bytes (used for the 72 KB image). */
int olru_link_read_block(olru_device *dev, uint8_t *buf, size_t total);

#endif /* OLRU_INTERNAL_H */
