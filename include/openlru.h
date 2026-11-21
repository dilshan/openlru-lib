/**
 * @file openlru.h
 * @brief Open LRU Fingerprint Library (openlru-lib).
 * @author Dilshan R Jayakody (jayakody2000lk@gmail.com)
 *
 * A small, dependency-free C library that talks directly to LRU series
 * USB fingerprint sensors (the family sold with the "LRU series SDK").
 *
 * What the library does
 * ---------------------
 *   1. Probe    - list the supported sensors currently plugged in.
 *   2. Open     - claim a sensor and check that it answers.
 *   3. Capture  - detect a finger and read the raw fingerprint image.
 *   4. Template - ask the sensor to extract a 256 byte feature template
 *                 from the captured image and read it back.
 *
 * What the library deliberately does NOT do
 * -----------------------------------------
 *   There is no matching, searching, enrolment-merge or verification code.
 *   Templates and images are only *read* from the sensor; what you do with
 *   them afterwards is up to the application.
 *
 * Typical use
 * -----------
 * @code
 *   olru_device_info list[4];
 *   int count = olru_probe(list, 4);          // 1. find sensors
 *
 *   olru_device *dev;
 *   if (count > 0 && olru_open(&list[0], &dev) == OLRU_OK) {   // 2. open
 *       uint8_t raw[OLRU_IMAGE_SIZE];
 *
 *       if (olru_wait_for_finger(dev, 5000) == OLRU_OK &&      // 3. capture
 *           olru_read_image(dev, raw, sizeof raw) == OLRU_OK)
 *           olru_image_save_bmp(raw, "finger.bmp");
 *
 *       olru_close(dev);
 *   }
 * @endcode
 *
 * Threading: a device handle must not be used from two threads at once.
 */
#ifndef OPENLRU_H
#define OPENLRU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define OLRU_IMAGE_WIDTH     256                 /**< Raw image width in pixels.  */
#define OLRU_IMAGE_HEIGHT    288                 /**< Raw image height in pixels. */
#define OLRU_IMAGE_SIZE      (256 * 288)         /**< Raw image size: 8 bit/pixel (73728 bytes). */
#define OLRU_BMP_SIZE        (54 + 1024 + OLRU_IMAGE_SIZE) /**< Size of olru_image_to_bmp() output. */
#define OLRU_TEMPLATE_SIZE   256                 /**< Size of one feature template. */
#define OLRU_MAX_DEVICES     8                   /**< Handy upper bound for olru_probe() lists. */

/* ------------------------------------------------------------------ */
/* Status codes                                                       */
/* ------------------------------------------------------------------ */

/** Result of every library call.  Zero is success, negative is failure. */
typedef enum {
    OLRU_OK             =  0,  /**< Success.                                        */
    OLRU_ERR_ARG        = -1,  /**< Invalid argument (NULL pointer, bad size...).   */
    OLRU_ERR_NO_DEVICE  = -2,  /**< No supported sensor found / sensor unplugged.   */
    OLRU_ERR_ACCESS     = -3,  /**< Permission denied - see udev/99-openlru.rules.  */
    OLRU_ERR_IO         = -4,  /**< USB transfer failed.                            */
    OLRU_ERR_TIMEOUT    = -5,  /**< USB transfer or finger wait timed out.          */
    OLRU_ERR_PROTOCOL   = -6,  /**< The sensor sent an unexpected reply.            */
    OLRU_ERR_NO_FINGER  = -7,  /**< No finger on the sensor (not a real failure).   */
    OLRU_ERR_DEVICE     = -8,  /**< The sensor reported an error, see olru_device_code(). */
    OLRU_ERR_NOMEM      = -9   /**< Out of memory.                                  */
} olru_status;

/* ------------------------------------------------------------------ */
/* Types                                                              */
/* ------------------------------------------------------------------ */

/** How the sensor's USB interface carries commands. */
typedef enum {
    OLRU_LINK_BULK = 0,   /**< Vendor control request + plain bulk endpoints.   */
    OLRU_LINK_MSC  = 1    /**< Bulk endpoints wrapped in mass-storage CBW/CSW.  */
} olru_link_type;

/** Describes one sensor found by olru_probe(). */
typedef struct {
    uint16_t       vid;          /**< USB vendor id.                           */
    uint16_t       pid;          /**< USB product id.                          */
    uint8_t        bus;          /**< USB bus number.                          */
    uint8_t        address;      /**< USB device address on that bus.          */
    olru_link_type link;         /**< Transport used by this model.            */
    uint8_t        alt_opcodes;  /**< 1 if the model uses the alternate opcode set. */
    const char    *name;         /**< Static, human readable model name.       */
} olru_device_info;

/** Opaque handle to an opened sensor. */
typedef struct olru_device olru_device;

/* ------------------------------------------------------------------ */
/* Probing and opening                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Find supported sensors on the USB bus.
 *
 * Only reads sysfs; the sensors are not touched, so this is safe to call
 * repeatedly (for example from a "Refresh" button).
 *
 * Sensors that use the mass-storage transport (OLRU_LINK_MSC, USB ids
 * 2009:7638 and 2109:7638, shown by lsusb as "USBKey Module") are included.
 * Those ids are also used by some ordinary USB storage products; set the
 * environment variable OLRU_DENY_MSC to hide them from the list.
 *
 * @param list  Array that receives the sensors found.
 * @param max   Number of entries available in @p list.
 * @return Number of sensors stored in @p list (0 if none), or a negative
 *         olru_status on error.
 */
int olru_probe(olru_device_info *list, size_t max);

/**
 * @brief Open a sensor found by olru_probe() and check that it answers.
 *
 * Claims the USB interface (detaching a kernel driver if one is bound)
 * and performs the link handshake.  The caller must olru_close() it.
 *
 * @param info  Sensor description from olru_probe().
 * @param out   Receives the new handle on success.
 * @return OLRU_OK, or OLRU_ERR_ACCESS / OLRU_ERR_NO_DEVICE / OLRU_ERR_DEVICE...
 */
int olru_open(const olru_device_info *info, olru_device **out);

/** @brief Release the sensor and free the handle.  NULL is allowed. */
void olru_close(olru_device *dev);

/** @brief Re-run the link handshake; OLRU_OK means the sensor is responsive. */
int olru_link_check(olru_device *dev);

/* ------------------------------------------------------------------ */
/* Reading fingerprints                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Ask the sensor to capture an image once (non blocking).
 *
 * @return OLRU_OK        a finger was present and the image is stored in the
 *                        sensor, ready for olru_read_image() / olru_read_template();
 * @return OLRU_ERR_NO_FINGER  nothing on the sensor yet - call again later.
 */
int olru_capture_image(olru_device *dev);

/**
 * @brief Poll olru_capture_image() every 100 ms until a finger is detected.
 *
 * @param timeout_ms  Give up after this many milliseconds (0 = try once).
 * @return OLRU_OK, OLRU_ERR_TIMEOUT, or any error from olru_capture_image().
 */
int olru_wait_for_finger(olru_device *dev, unsigned timeout_ms);

/**
 * @brief Download the captured image from the sensor.
 *
 * Call after olru_capture_image() / olru_wait_for_finger() succeeded.
 *
 * @param buf   Receives OLRU_IMAGE_WIDTH x OLRU_IMAGE_HEIGHT 8 bit pixels,
 *              in the same row order as the vendor SDK's raw buffer.
 * @param size  Size of @p buf; must be at least OLRU_IMAGE_SIZE.
 */
int olru_read_image(olru_device *dev, uint8_t *buf, size_t size);

/**
 * @brief Have the sensor extract a feature template and read it back.
 *
 * Call after a successful capture.  The sensor does the feature extraction;
 * the library only transports the result.
 *
 * @param out  Receives OLRU_TEMPLATE_SIZE bytes.
 * @return OLRU_OK, or OLRU_ERR_DEVICE if the image was too poor to use
 *         (see olru_device_code()).
 */
int olru_read_template(olru_device *dev, uint8_t out[OLRU_TEMPLATE_SIZE]);

/* ------------------------------------------------------------------ */
/* Image export helpers                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Wrap a raw image in an 8 bit grey-scale BMP file image (in memory).
 *
 * The output is byte-identical in layout to the BMP written by the vendor
 * SDK, so it can be handed straight to image viewers or GdkPixbufLoader.
 *
 * @param raw  OLRU_IMAGE_SIZE bytes from olru_read_image().
 * @param out  Buffer of at least OLRU_BMP_SIZE bytes.
 * @return Number of bytes written (OLRU_BMP_SIZE) or a negative olru_status.
 */
int olru_image_to_bmp(const uint8_t *raw, uint8_t *out, size_t out_size);

/** @brief Convenience: write a raw image to a .bmp file. */
int olru_image_save_bmp(const uint8_t *raw, const char *path);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/** @brief Static, human readable text for an olru_status value. */
const char *olru_strerror(int status);

/**
 * @brief Raw confirmation code of the sensor's last reply.
 *
 * Useful after OLRU_ERR_DEVICE.  Typical values: 0x00 ok, 0x02 no finger,
 * 0x03 image capture failed, 0x06 image too messy, 0x07 too few features,
 * 0x15 no valid image in the buffer.
 */
uint8_t olru_device_code(const olru_device *dev);

/** @brief Static text for a sensor confirmation code. */
const char *olru_device_code_text(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* OPENLRU_H */
