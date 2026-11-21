/**
 * @file openlru.c
 * @brief Public API: probing, opening, capturing images and templates.
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Each public function is a short sequence of sensor commands.  A command
 * is a packet (see olru_packet.c) whose first payload byte is an opcode:
 *
 *   opcode  name        payload after opcode   sensor answers with
 *   ------  ----------  ---------------------  ----------------------------
 *   0x13    Link        4 byte password        ack packet (code 0 = ok)
 *   0x01    GetImage    -                      ack packet (0 ok, 2 no finger)
 *   0x0A    UpImage     -                      73728 raw bytes (no ack)
 *   0x02    GenChar     buffer id              ack packet (0 = ok)
 *   0x08    UpChar      buffer id              512 raw bytes (no ack)
 *
 * Models with alt_opcodes use 0x33 instead of 0x13 and 0x32 instead of 0x01.
 */

#include "olru_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OP_LINK            0x13
#define OP_LINK_ALT        0x33
#define OP_GET_IMAGE       0x01
#define OP_GET_IMAGE_ALT   0x32
#define OP_GEN_CHAR        0x02
#define OP_UP_CHAR         0x08
#define OP_UP_IMAGE        0x0A

#define CODE_OK            0x00
#define CODE_NO_FINGER     0x02

#define CHAR_BUFFER        0x01   /* the sensor's feature buffer we use  */
#define UP_CHAR_SIZE       512    /* bytes sent by UpChar                */
#define LINK_PASSWORD      0x78   /* default handshake "password"        */
#define LINK_TRIES         3
#define LINK_DELAY_US      200000
#define POLL_INTERVAL_MS   100

/* ------------------------------------------------------------------ */
/* Command helpers                                                    */
/* ------------------------------------------------------------------ */

/** Build and send one command packet. */
static int send_command(olru_device *dev, const uint8_t *payload, size_t len)
{
    uint8_t packet[OLRU_PACKET_MAX];
    size_t total = olru_packet_build(packet, sizeof packet, OLRU_PID_COMMAND,
                                     payload, len);

    if (total == 0)
        return OLRU_ERR_ARG;
    return olru_link_write(dev, packet, total);
}

/** Read the sensor's reply packet and return its confirmation code. */
static int read_ack(olru_device *dev, uint8_t *code)
{
    uint8_t reply[OLRU_PACKET_MAX];
    int n = olru_link_read(dev, reply, sizeof reply);

    if (n < 0)
        return n;
    return olru_packet_parse_ack(reply, (size_t)n, code);
}

/** Turn a confirmation code into an olru_status and remember it. */
static int status_from_code(olru_device *dev, uint8_t code)
{
    dev->last_code = code;
    if (code == CODE_OK)
        return OLRU_OK;
    return code == CODE_NO_FINGER ? OLRU_ERR_NO_FINGER : OLRU_ERR_DEVICE;
}

/** Send a command and wait for its ack packet. */
static int run_command(olru_device *dev, const uint8_t *payload, size_t len)
{
    uint8_t code;
    int rc = send_command(dev, payload, len);

    if (rc != OLRU_OK)
        return rc;
    rc = read_ack(dev, &code);
    return rc != OLRU_OK ? rc : status_from_code(dev, code);
}

/* ------------------------------------------------------------------ */
/* Probing, opening, closing                                          */
/* ------------------------------------------------------------------ */

int olru_probe(olru_device_info *list, size_t max)
{
    if (!list || max == 0)
        return OLRU_ERR_ARG;
    return olru_usb_scan(list, max);
}

olru_device *olru_device_create(const struct olru_usb_ops *ops, void *ctx,
                                olru_link_type link, uint8_t alt_opcodes)
{
    olru_device *dev = calloc(1, sizeof *dev);

    if (!dev)
        return NULL;
    dev->ops = ops;
    dev->ctx = ctx;
    dev->link = link;
    dev->alt_opcodes = alt_opcodes;
    return dev;
}

int olru_open(const olru_device_info *info, olru_device **out)
{
    const struct olru_usb_ops *ops;
    void *ctx;
    olru_device *dev;
    int rc;

    if (!info || !out)
        return OLRU_ERR_ARG;
    rc = olru_usb_open(info, &ops, &ctx);
    if (rc != OLRU_OK)
        return rc;
    dev = olru_device_create(ops, ctx, info->link, info->alt_opcodes);
    if (!dev) {
        ops->close(ctx);
        return OLRU_ERR_NOMEM;
    }
    rc = olru_link_check(dev);
    if (rc != OLRU_OK) {
        olru_close(dev);
        return rc;
    }
    *out = dev;
    return OLRU_OK;
}

void olru_close(olru_device *dev)
{
    if (!dev)
        return;
    dev->ops->close(dev->ctx);
    free(dev);
}

/** One handshake attempt. */
static int link_once(olru_device *dev)
{
    uint8_t cmd[5] = { dev->alt_opcodes ? OP_LINK_ALT : OP_LINK,
                       LINK_PASSWORD, 0x00, 0x00, 0x00 };

    return run_command(dev, cmd, sizeof cmd);
}

int olru_link_check(olru_device *dev)
{
    int rc = OLRU_ERR_ARG;

    if (!dev)
        return rc;
    usleep(LINK_DELAY_US);               /* sensor may still be restarting */
    for (int attempt = 0; attempt < LINK_TRIES; attempt++) {
        rc = link_once(dev);
        if (rc == OLRU_OK)
            break;
        usleep(POLL_INTERVAL_MS * 1000);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Reading fingerprints                                               */
/* ------------------------------------------------------------------ */

int olru_capture_image(olru_device *dev)
{
    uint8_t cmd[1];

    if (!dev)
        return OLRU_ERR_ARG;
    cmd[0] = dev->alt_opcodes ? OP_GET_IMAGE_ALT : OP_GET_IMAGE;
    return run_command(dev, cmd, sizeof cmd);
}

int olru_wait_for_finger(olru_device *dev, unsigned timeout_ms)
{
    unsigned waited = 0;
    int rc;

    while ((rc = olru_capture_image(dev)) == OLRU_ERR_NO_FINGER) {
        if (waited >= timeout_ms)
            return OLRU_ERR_TIMEOUT;
        usleep(POLL_INTERVAL_MS * 1000);
        waited += POLL_INTERVAL_MS;
    }
    return rc;
}

int olru_read_image(olru_device *dev, uint8_t *buf, size_t size)
{
    uint8_t cmd[1] = { OP_UP_IMAGE };
    int rc;

    if (!dev || !buf || size < OLRU_IMAGE_SIZE)
        return OLRU_ERR_ARG;
    rc = send_command(dev, cmd, sizeof cmd);
    if (rc != OLRU_OK)
        return rc;
    return olru_link_read_block(dev, buf, OLRU_IMAGE_SIZE);
}

/** Read the sensor's feature buffer (UpChar); returns UP_CHAR_SIZE bytes. */
static int read_char_buffer(olru_device *dev, uint8_t *buf)
{
    uint8_t cmd[2] = { OP_UP_CHAR, CHAR_BUFFER };
    int n = send_command(dev, cmd, sizeof cmd);

    if (n != OLRU_OK)
        return n;
    n = olru_link_read(dev, buf, UP_CHAR_SIZE);
    if (n < 0)
        return n;
    return n == UP_CHAR_SIZE ? OLRU_OK : OLRU_ERR_IO;
}

int olru_read_template(olru_device *dev, uint8_t out[OLRU_TEMPLATE_SIZE])
{
    uint8_t gen[2] = { OP_GEN_CHAR, CHAR_BUFFER };
    uint8_t raw[UP_CHAR_SIZE];
    int rc;

    if (!dev || !out)
        return OLRU_ERR_ARG;
    rc = run_command(dev, gen, sizeof gen);     /* sensor extracts features */
    if (rc != OLRU_OK)
        return rc;
    rc = read_char_buffer(dev, raw);
    if (rc == OLRU_OK)
        memcpy(out, raw, OLRU_TEMPLATE_SIZE);   /* first 256 bytes are used */
    return rc;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

const char *olru_strerror(int status)
{
    switch (status) {
    case OLRU_OK:            return "Success";
    case OLRU_ERR_ARG:       return "Invalid argument";
    case OLRU_ERR_NO_DEVICE: return "No sensor found";
    case OLRU_ERR_ACCESS:    return "Permission denied (install udev rule or run as root)";
    case OLRU_ERR_IO:        return "USB transfer failed";
    case OLRU_ERR_TIMEOUT:   return "Timed out";
    case OLRU_ERR_PROTOCOL:  return "Unexpected reply from sensor";
    case OLRU_ERR_NO_FINGER: return "No finger on sensor";
    case OLRU_ERR_DEVICE:    return "Sensor reported an error";
    case OLRU_ERR_NOMEM:     return "Out of memory";
    default:                 return "Unknown error";
    }
}

uint8_t olru_device_code(const olru_device *dev)
{
    return dev ? dev->last_code : 0;
}

const char *olru_device_code_text(uint8_t code)
{
    switch (code) {
    case 0x00: return "OK";
    case 0x01: return "Communication error";
    case 0x02: return "No finger";
    case 0x03: return "Image capture failed";
    case 0x04: return "Finger too dry / image too light";
    case 0x05: return "Finger too wet / image too blurred";
    case 0x06: return "Image too messy";
    case 0x07: return "Too few feature points";
    case 0x15: return "No valid image in sensor buffer";
    default:   return "Unknown sensor error";
    }
}
