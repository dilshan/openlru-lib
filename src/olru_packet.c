/**
 * @file olru_packet.c
 * @brief Sensor packet format (building commands, reading replies).
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * Every command and reply uses the same frame:
 *
 *   offset  size  meaning
 *   ------  ----  --------------------------------------------------
 *      0      2   header, always EF 01
 *      2      4   chip address, always FF FF FF FF
 *      6      1   packet id (1 = command, 7 = reply)
 *      7      2   length = payload size + 2 (checksum), big endian
 *      9      n   payload (first byte of a command is the opcode;
 *                 first byte of a reply is the confirmation code)
 *    9+n      2   checksum = 16 bit sum of bytes 6 .. 8+n, big endian
 */

#include "olru_internal.h"

#include <string.h>

#define HEADER_SIZE   9   /* header + address + pid + length */
#define CHECKSUM_SIZE 2

/** Store @p value as two big endian bytes. */
static void put_be16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/** Sum @p len bytes into a 16 bit checksum. */
static unsigned checksum16(const uint8_t *p, size_t len)
{
    unsigned sum = 0;

    while (len--)
        sum += *p++;
    return sum & 0xFFFF;
}

/** Write the fixed 9 byte frame header. */
static void write_header(uint8_t *out, uint8_t pid, size_t payload_len)
{
    out[0] = 0xEF;
    out[1] = 0x01;
    memset(out + 2, 0xFF, 4);               /* chip address */
    out[6] = pid;
    put_be16(out + 7, (unsigned)payload_len + CHECKSUM_SIZE);
}

size_t olru_packet_build(uint8_t *out, size_t out_size, uint8_t pid,
                         const uint8_t *payload, size_t payload_len)
{
    size_t total = HEADER_SIZE + payload_len + CHECKSUM_SIZE;

    if (total > out_size)
        return 0;
    write_header(out, pid, payload_len);
    memcpy(out + HEADER_SIZE, payload, payload_len);
    put_be16(out + HEADER_SIZE + payload_len,
             checksum16(out + 6, 3 + payload_len));
    return total;
}

int olru_packet_parse_ack(const uint8_t *in, size_t len, uint8_t *code)
{
    /* Like the vendor SDK, the reply checksum is not verified: USB already
     * protects the transfer, and the sensor's own checksum is advisory. */
    if (len < OLRU_ACK_MIN)
        return OLRU_ERR_PROTOCOL;
    if (in[0] != 0xEF || in[1] != 0x01 || in[6] != OLRU_PID_ACK)
        return OLRU_ERR_PROTOCOL;
    *code = in[HEADER_SIZE];
    return OLRU_OK;
}
