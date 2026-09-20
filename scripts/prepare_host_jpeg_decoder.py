#!/usr/bin/env python3
"""Copy esp_jpeg 1.3.1 with its external TJPGD callback ABI corrected for hosts.

The managed dependency and ESP32-S3 ROM build stay untouched. Keep this explicit
host adaptation until upstream supplies matching size_t input callbacks.
"""
import hashlib
from pathlib import Path
import sys


def prepare(source: Path, destination: Path) -> None:
    data = source.read_bytes()
    expected = "ea97dd2bedb8cebbc5afed2bbcb9f2d9239340b51eab4fabf3a9bccd5a23d949"
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError("esp_jpeg source changed; review the host callback adaptation")
    text = data.decode()
    text = text.replace("typedef unsigned int jpeg_decode_out_t;",
                        "typedef unsigned int jpeg_decode_in_t;\ntypedef unsigned int jpeg_decode_out_t;")
    text = text.replace("typedef int jpeg_decode_out_t;",
                        "typedef size_t jpeg_decode_in_t;\ntypedef int jpeg_decode_out_t;")
    text = text.replace("static unsigned int jpeg_decode_in_cb",
                        "static jpeg_decode_in_t jpeg_decode_in_cb")
    text = text.replace("uint8_t *buff, unsigned int nbyte)",
                        "uint8_t *buff, jpeg_decode_in_t nbyte)")
    text = text.replace("    uint32_t to_read = nbyte;",
                        "    if (nbyte > UINT32_MAX) {\n        return 0;\n    }\n\n"
                        "    uint32_t to_read = (uint32_t)nbyte;")
    with destination.open("x") as output:
        output.write(text)


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
