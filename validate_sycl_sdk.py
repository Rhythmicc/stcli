"""Pinned, private, audit-only property probe. Never a customer utility."""
import ctypes
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent
sdk = root / 'SYCL-libpangulu.so'
assert hashlib.sha256(sdk.read_bytes()).hexdigest() == '249aa1d390b2d5dfe0ba3e61cb486185da1ce1ebc10ae2f115bc20309ef9909c'
lib = ctypes.CDLL(str(sdk))
base = ctypes.cast(lib.panguluPrintGpuInfo, ctypes.c_void_p).value - 0x21780
query = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int)(base + 0x39170)
properties = ctypes.create_string_buffer(0x130)
query(properties, 0)
print(json.dumps(dict(name=properties.raw[:256].split(b'\0')[0].decode(),
                     uuid=properties.raw[0x120:0x130].hex(),
                     sdk_sha256=hashlib.sha256(sdk.read_bytes()).hexdigest())))
