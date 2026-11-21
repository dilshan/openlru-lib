/**
 * @file olru_link.c
 * @brief Moves bytes to and from the sensor over USB.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Two kinds of sensor exist and they differ only in how a transfer is
 * announced; the bytes carried are identical.
 *
 * OLRU_LINK_BULK  (e.g. 0453:9005, 0618:9018)
 *   Before each transfer the host sends a vendor control request
 *   (type 0xC0, request 0 = "host will write", 1 = "host will read",
 *   wValue = length).  Data then moves on bulk endpoints 0x02 (out) and
 *   0x81 (in).
 *
 * OLRU_LINK_MSC   (e.g. 2009:7638, 2109:7638)
 *   Each transfer is wrapped like USB mass storage: a 31 byte "USBC"
 *   command block, the data stage, and a 13 byte "USBS" status block.
 *   Data moves on bulk endpoints 0x01 (out) and 0x81 (in).
 */

#include "olru_internal.h"

#include <string.h>

#define REQ_WRITE      0x00   /* BULK: announce host -> sensor transfer */
#define REQ_READ       0x01   /* BULK: announce sensor -> host transfer */
#define CTRL_REPLY_LEN 10     /* BULK: status bytes returned by the request */

#define CBW_SIZE       31
#define CSW_SIZE       13
#define MSC_OP_READ    0x85   /* vendor SCSI opcode: read from sensor  */
#define MSC_OP_WRITE   0x86   /* vendor SCSI opcode: write to sensor   */
#define MSC_TAG        { 0xB0, 0xFA, 0x69, 0x86 }
#define MSC_CHUNKS     8      /* the image is fetched in 8 equal pieces */

/* ------------------------------------------------------------------ */
/* BULK flavour                                                       */
/* ------------------------------------------------------------------ */

/** Announce a transfer.  The sensor's reply is ignored, as in the SDK. */
static void bulk_announce(olru_device *dev, uint8_t request, size_t len)
{
    uint8_t reply[CTRL_REPLY_LEN];

    dev->ops->control_in(dev->ctx, request, (uint16_t)len, reply,
                         sizeof reply);
}

static int bulk_write(olru_device *dev, const uint8_t *data, size_t len)
{
    int n;

    bulk_announce(dev, REQ_WRITE, len);
    n = dev->ops->bulk_out(dev->ctx, OLRU_EP_OUT_BULK, data, len);
    return n < 0 ? n : OLRU_OK;
}

static int bulk_read(olru_device *dev, uint8_t *buf, size_t len)
{
    bulk_announce(dev, REQ_READ, len);
    return dev->ops->bulk_in(dev->ctx, OLRU_EP_IN, buf, len);
}

/** Read @p total bytes as two halves after a single announcement. */
static int bulk_read_block(olru_device *dev, uint8_t *buf, size_t total)
{
    size_t half = total / 2;
    int n;

    bulk_announce(dev, REQ_READ, total);
    for (int part = 0; part < 2; part++) {
        n = dev->ops->bulk_in(dev->ctx, OLRU_EP_IN, buf + part * half, half);
        if (n < 0)
            return n;
        if ((size_t)n != half)
            return OLRU_ERR_IO;
    }
    return OLRU_OK;
}

/* ------------------------------------------------------------------ */
/* MSC flavour                                                         */
/* ------------------------------------------------------------------ */

/** Fill the 31 byte command block for a transfer of @p len bytes. */
static void msc_build_cbw(uint8_t *cbw, size_t len, int is_read)
{
    static const uint8_t tag[4] = MSC_TAG;

    memset(cbw, 0, CBW_SIZE);
    memcpy(cbw, "USBC", 4);
    memcpy(cbw + 4, tag, 4);
    for (int i = 0; i < 4; i++)               /* little endian length */
        cbw[8 + i] = (uint8_t)(len >> (8 * i));
    cbw[12] = is_read ? 0x80 : 0x00;          /* direction flag */
    cbw[14] = 0x0A;                           /* command block length */
    cbw[15] = is_read ? MSC_OP_READ : MSC_OP_WRITE;
}

/** Read the 13 byte status block and check it belongs to our command. */
static int msc_check_csw(olru_device *dev, const uint8_t *cbw)
{
    uint8_t csw[CSW_SIZE];
    int n = dev->ops->bulk_in(dev->ctx, OLRU_EP_IN, csw, sizeof csw);

    if (n < 0)
        return n;
    if (n != CSW_SIZE || memcmp(csw, "USBS", 4) != 0)
        return OLRU_ERR_PROTOCOL;
    if (memcmp(csw + 4, cbw + 4, 4) != 0 || csw[12] != 0)
        return OLRU_ERR_PROTOCOL;
    return OLRU_OK;
}

static int msc_write(olru_device *dev, const uint8_t *data, size_t len)
{
    uint8_t cbw[CBW_SIZE];
    int n;

    msc_build_cbw(cbw, len, 0);
    n = dev->ops->bulk_out(dev->ctx, OLRU_EP_OUT_MSC, cbw, sizeof cbw);
    if (n < 0)
        return n;
    n = dev->ops->bulk_out(dev->ctx, OLRU_EP_OUT_MSC, data, len);
    if (n < 0)
        return n;
    return msc_check_csw(dev, cbw);
}

static int msc_read(olru_device *dev, uint8_t *buf, size_t len)
{
    uint8_t cbw[CBW_SIZE];
    int got, rc;

    msc_build_cbw(cbw, len, 1);
    rc = dev->ops->bulk_out(dev->ctx, OLRU_EP_OUT_MSC, cbw, sizeof cbw);
    if (rc < 0)
        return rc;
    got = dev->ops->bulk_in(dev->ctx, OLRU_EP_IN, buf, len);
    if (got < 0)
        return got;
    rc = msc_check_csw(dev, cbw);
    return rc < 0 ? rc : got;
}

static int msc_read_block(olru_device *dev, uint8_t *buf, size_t total)
{
    size_t chunk = total / MSC_CHUNKS;
    int n;

    for (int i = 0; i < MSC_CHUNKS; i++) {
        n = msc_read(dev, buf + i * chunk, chunk);
        if (n < 0)
            return n;
        if ((size_t)n != chunk)
            return OLRU_ERR_IO;
    }
    return OLRU_OK;
}

/* ------------------------------------------------------------------ */
/* Public (library internal) entry points                              */
/* ------------------------------------------------------------------ */

int olru_link_write(olru_device *dev, const uint8_t *data, size_t len)
{
    return dev->link == OLRU_LINK_MSC ? msc_write(dev, data, len)
                                      : bulk_write(dev, data, len);
}

int olru_link_read(olru_device *dev, uint8_t *buf, size_t len)
{
    return dev->link == OLRU_LINK_MSC ? msc_read(dev, buf, len)
                                      : bulk_read(dev, buf, len);
}

int olru_link_read_block(olru_device *dev, uint8_t *buf, size_t total)
{
    return dev->link == OLRU_LINK_MSC ? msc_read_block(dev, buf, total)
                                      : bulk_read_block(dev, buf, total);
}
