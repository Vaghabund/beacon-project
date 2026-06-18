#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "wifi_rings.h"   // WifiNetwork, Pixel, IMG_PIXELS

// ─────────────────────────────────────────────────────────────────────────────
//  wifi_data — the RECOVERABLE layer
//
//  Where wifi_rings.c paints the WiFi scan into the image artistically (one-way,
//  not decodable), this module hides the *exact* scan data in the pixel LSBs so a
//  host decoder can read the real network list back out of a losslessly-saved
//  image. The two layers coexist: embed runs AFTER rings_encode(), so the
//  pixel-sort can never clobber the payload, and a ±1 LSB nudge is invisible
//  against the rings.
//
//  CRITICAL: the carrying image must be saved losslessly (BMP/PNG). JPEG
//  compression destroys the LSBs and with them the entire payload.
//
//  Frame format (self-delimiting — the decoder needs no separate length field):
//
//    offset  field        size   notes
//    0       magic        4      'W','B','C','N'
//    4       version      1      = WBCN_VERSION
//    5       n_nets       1      number of records that follow
//    6..     records      var    per record, in order:
//                                   ssid_len  1   (0..32)
//                                   ssid      ssid_len bytes (raw, any encoding)
//                                   bssid     6
//                                   rssi      1   (int8 stored as a byte)
//                                   channel   1
//    end-2   crc16        2      CRC-16/CCITT-FALSE over every preceding byte,
//                                big-endian
// ─────────────────────────────────────────────────────────────────────────────

#define WBCN_MAGIC0   'W'
#define WBCN_MAGIC1   'B'
#define WBCN_MAGIC2   'C'
#define WBCN_MAGIC3   'N'
#define WBCN_VERSION  1

// Largest possible frame: header(6) + MAX_NETWORKS records + crc(2).
// Per record max = 1 (ssid_len) + 32 (ssid) + 6 (bssid) + 1 (rssi) + 1 (ch) = 41.
#define WBCN_MAX_FRAME  (6 + MAX_NETWORKS * 41 + 2)

#ifdef __cplusplus
extern "C" {
#endif

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Exposed for the unit-style
// self-test and so the host decoder can be cross-checked against it.
uint16_t wbcn_crc16(const uint8_t* data, int len);

// Build the framed payload from a sorted network list.
//   out     — caller buffer, at least WBCN_MAX_FRAME bytes
//   out_max — capacity of out
// Returns the frame length in bytes, or -1 if it would not fit.
int data_build_payload(const WifiNetwork* nets, int n,
                       uint8_t* out, int out_max);

// Embed a byte buffer into the LSBs of an RGB888 image, MSB-first, walking
// channels in r,g,b row-major order. Writes len*8 bits.
//   img      — RGB888 buffer (treated as 3*n_pixels channel bytes)
//   n_pixels — number of pixels in img (e.g. IMG_PIXELS)
// Returns bits written, or -1 if the payload does not fit.
int data_embed_lsb(Pixel* img, int n_pixels, const uint8_t* buf, int len);

// Inverse of data_embed_lsb, with validation. Reads the magic, parses the
// self-delimiting frame to learn its length, reads the CRC and verifies it.
//   out     — caller buffer, at least WBCN_MAX_FRAME bytes
//   out_max — capacity of out
// Returns the recovered frame length on success, or -1 if magic/CRC/bounds fail.
int data_extract_lsb(const Pixel* img, int n_pixels,
                     uint8_t* out, int out_max);

#ifdef __cplusplus
}
#endif
