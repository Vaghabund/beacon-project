# BEACON — project notes

*A handheld instrument that turns the invisible Wi-Fi around you into a photograph, and hides the real radio data inside the picture.*

---

## the idea

Every room you walk into is full of radio you can't see. A dozen Wi-Fi networks,
each announcing itself — a name, a manufacturer's fingerprint in its MAC address,
a channel, a signal strength that quietly encodes how far away it is. It's an
ambient portrait of a place, broadcast constantly, and completely invisible.

BEACON is a camera for that invisible layer. You point it at a scene, press one
button, and it does two things at once: it photographs what's in front of you,
and it scans the Wi-Fi around you — then fuses them. The networks become
concentric rings of pixel-sorted distortion warped into the photo, strongest at
the centre. The result is a single image that is half landscape, half spectrum
analyser.

And it keeps the data honest. The picture isn't a decorative filter sitting on
top of a scan — the rings are a *direct readout* of the networks' identities, and
the **exact** scan is steganographically hidden in the pixels, so the original
data can always be recovered from the image. One artefact, two truths: one you
see, one you can read back.

---

## the object

It's self-contained — no phone app, no cloud, no companion device. Everything
runs on a single ESP32-S3 board the size of a matchbox: the camera, the colour
display, the capacitive touch, the Wi-Fi radio, the storage, and a little web
server all live on the one chip.

- **Tap** the screen → it captures, scans, encodes, and shows you the result in a
  few seconds.
- **Hold** → it becomes its own Wi-Fi hotspot and shows two QR codes; your phone
  joins it and browses an on-device gallery styled like a signals-intelligence
  terminal, where you can preview, save, or delete captures.

A 3D-printed enclosure (files coming) turns it into a finished object — a
pocket-sized field instrument for an invisible signal.

---

## the layers — why this is more than a filter

The thing I care about most in this project is that the image is *truthful*.

**The visible layer** is generative art with a rule: each network's name, MAC,
and channel are read as a string of numbers, and those numbers literally set the
shape of the ring — how deep each radial "spoke" of pixel-sort cuts into the
photo. Signal strength sets how far the smear reaches. So the same network always
draws the same rhythm, and a strong network tears harder at the image than a weak
one. You're not looking at a random glitch; you're looking at the data wearing a
costume.

**The hidden layer** is the data without the costume. The precise scan — every
SSID, BSSID, RSSI, and channel — is framed, checksummed, and tucked into the
least-significant bit of every colour channel. It's invisible: a one-in-256 nudge
to each pixel. But a decoder reads it straight back out, byte for byte. The only
constraint is that the image must be saved losslessly — a single JPEG re-save
would erase the hidden layer while leaving the art untouched, which is its own
quiet comment on what compression throws away.

---

## what this combines

This project sits on the seam between several disciplines, which is the part I
enjoy most:

- **Embedded systems / hardware bring-up.** Driving a camera, an SPI display,
  capacitive touch, PSRAM, and the Wi-Fi stack on one microcontroller — and
  budgeting memory and bus bandwidth so the camera, radio, and encoder never
  fight. (The bring-up included a satisfying bug: a scrambled camera data-line
  map that left geometry and brightness perfect but permuted every colour.)

- **Generative / glitch art.** A from-scratch radial pixel-sort encoder written
  in pure, dependency-free C — which means the *exact* device encoder also
  compiles and runs on a laptop, so the look can be tuned in seconds without
  reflashing.

- **Steganography & data encoding.** A compact binary frame format with a
  CRC, embedded in pixel LSBs, with a careful ordering guarantee so the art and
  the data never overwrite each other.

- **OSINT / SIGINT.** The recovered scan is the raw material of wardriving:
  vendor lookup from MAC prefixes, channel occupancy, 2.4 vs 5 GHz bands, rough
  distance from signal strength, and BSSIDs ready to geolocate. A browser-based
  decoder renders all of it locally — drop in a capture, read the room.

- **Web & tooling.** An offline, no-upload decode dashboard; a host-side preview
  harness that runs the real firmware C over photos; a self-testing data layer
  that validates its own format without hardware.

- **Industrial / product design.** One button, three states, a captive-portal
  sharing flow that had to be fought into working around iOS's locked-down
  captive browser — and an enclosure to make it a thing you'd actually carry.

---

## standard parts, an original idea

Almost none of the individual techniques here are new — and that's on purpose.
Splitting a byte into two nibbles (the 0–15 halves that shape each ring) is
bedrock: it's how every hex colour `#RRGGBB` and every MAC address is written.
Hiding data in the least-significant bits is textbook steganography. Pixel-sorting
is a well-worn glitch-art move. Using proven, boring plumbing is exactly what
makes the device reliable and the encoding easy to reason about.

What's original is the **coupling**: making a single image be *simultaneously* a
one-way artistic readout of a Wi-Fi scan (the rings you see) and a lossless,
fully recoverable record of that exact scan (the bits you don't). The components
are familiar; the combination — and the discipline of keeping both layers
truthful in one file — is the point.

## where it's going

The hidden data layer is wardriving-grade by design, so the natural next phase is
geolocation: feeding recovered BSSIDs to a service like WiGLE to place the
captured networks on a map — turning each image into not just *what* the radio
room looked like, but *where* it was. The web decoder already stubs that panel.

For now it's a working field instrument and an archive of captures: each one a
photograph of a place and a readable record of its invisible signal, in the same
file.

---

*Technical write-up: [README](../README.md) · how the encoding works, step by step: [ENCODING](ENCODING.md).*
