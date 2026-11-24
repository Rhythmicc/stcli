#!/usr/bin/env python3
"""End-to-end tests for stcli 1.3.0: compact ST. paste codes + TTY '*' echo."""
import base64
import calendar
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import time
import unittest
import uuid
import zlib

if os.name == 'posix':
    import pty

SOURCE = Path(__file__).resolve().parent
REPO_STORE_ROOT = SOURCE.parents[1]

ST3_EPOCH = 1700000000
PRODUCTS = ('pangulu', 'linglongamg', 'xianzhikrylov')
PROD_PREFIX = {'pangulu': 'pgl-', 'linglongamg': 'lla-', 'xianzhikrylov': 'xzk-'}
TERMS = ('hourly', 'daily', 'weekly', 'monthly', 'annual', 'perpetual', 'custom')
SERIAL_PREFIXES = ('cuda', 'musa', 'hip', 'hc', 'lisuan', 'sycl')


def encode_uvarint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def encode_bytes_u8(raw):
    return encode_uvarint(len(raw)) + raw


def encode_serial_v3(serial):
    m = re.fullmatch(
        r'(cuda|musa|hip|hc|lisuan|sycl)-([0-9a-f]{8})-([0-9a-f]{8})-([0-9a-f]{8})-([0-9a-f]{8})',
        serial,
    )
    if m:
        return SERIAL_PREFIXES.index(m.group(1)), bytes.fromhex(
            m.group(2) + m.group(3) + m.group(4) + m.group(5)
        )
    if re.fullmatch(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', serial):
        return 6, uuid.UUID(serial).bytes
    return 7, encode_bytes_u8(serial.encode('ascii'))


def implicit_expiry(issued_at, term):
    if term == 'perpetual':
        return 0
    if term == 'hourly':
        return issued_at + 3600
    if term == 'daily':
        return issued_at + 86400
    if term == 'weekly':
        return issued_at + 604800
    if term in ('monthly', 'annual'):
        if issued_at >= 253370764800:
            return None
        start = datetime.fromtimestamp(issued_at, timezone.utc)
        year = start.year + (1 if term == 'annual' else start.month == 12)
        month = start.month if term == 'annual' else start.month % 12 + 1
        return int(start.replace(year=year, month=month, day=min(start.day, calendar.monthrange(year, month)[1])).timestamp())
    return None


def deterministic_lid_hex(product, customer, issued_at, not_before, not_after, device):
    msg = f'st-lid-v1\n{product}\n{customer}\n{issued_at}\n{not_before}\n{not_after}\n{device}'.encode('ascii')
    return hashlib.sha1(msg).hexdigest()[:16]


def make_key_blob(
    *,
    product='linglongamg',
    device='hip-00010203-04050607-08090a0b-0c0d0e0f',
    issued_at=1700000000,
    not_before=None,
    not_after=None,
    term='monthly',
    min_ver='0.1.0',
    max_ver='',
    customer='opensparse-account-42',
    lid_suffix=None,
    extra_lines='',
    license_tier='user',
):
    if not_before is None:
        not_before = issued_at
    if not_after is None:
        not_after = implicit_expiry(issued_at, term)
        if not_after is None:
            not_after = issued_at + 72 * 3600
    if lid_suffix is None:
        lid_suffix = deterministic_lid_hex(product, customer, issued_at, not_before, not_after, device)
    lid = f'{PROD_PREFIX[product]}{issued_at}-{lid_suffix}'
    payload = (
        f'license_id={lid}\n'
        f'product={product}\n'
        f'min_ver={min_ver}\n'
        f'max_ver={max_ver}\n'
        f'gpu_serial={device}\n'
        f'issued_at={issued_at}\n'
        f'not_before={not_before}\n'
        f'not_after={not_after}\n'
        f'customer={customer}\n'
        f'license_tier={license_tier}\n'
        f'{extra_lines}'
    ).encode('ascii')
    sig = hashlib.sha512(payload).digest()
    blob = b'PGL-LICENSE-1\n' + base64.b64encode(payload) + b'\n' + base64.b64encode(sig) + b'\n'
    return blob, payload, sig


def wrap_st3_bin(body_without_crc):
    crc = zlib.crc32(body_without_crc) & 0xFFFFFFFF
    raw = body_without_crc + struct.pack('>I', crc)[:3]
    return 'ST.' + base64.urlsafe_b64encode(raw).rstrip(b'=').decode('ascii')


def encode_st3_canonical(entries):
    buf = bytearray([0x40 | (len(entries) - 1)])
    prev_meta = None
    for ent in entries:
        if ent.get('raw'):
            payload, sig = ent['payload'], ent['sig']
            buf.append(0x01)
            buf.extend(encode_bytes_u8(payload))
            buf.extend(sig)
            prev_meta = None
            continue
        cur_meta = (
            ent['product'], ent['term'], ent.get('min_ver', '0.1.0'), ent.get('max_ver', ''),
            ent.get('customer', 'opensparse-account-42'), ent['issued_at'],
            ent.get('not_before', ent['issued_at']), ent['not_after'],
        )
        same_meta = prev_meta == cur_meta and ent.get('allow_same', True)
        det_lid = deterministic_lid_hex(
            cur_meta[0], cur_meta[4], cur_meta[5], cur_meta[6], cur_meta[7], ent['device'],
        )
        custom_lid = ent.get('lid_suffix') is not None and ent['lid_suffix'] != det_lid
        skind, sbytes = encode_serial_v3(ent['device'])

        flags = (skind & 0x07) << 5
        if same_meta:
            flags |= 0x02
        if custom_lid:
            flags |= 0x04

        eb = bytearray()
        if not same_meta:
            min_v, max_v, cust, issued, nb, na = cur_meta[2], cur_meta[3], cur_meta[4], cur_meta[5], cur_meta[6], cur_meta[7]
            ext = 0
            if min_v != '0.1.0' or max_v != '':
                ext |= 0x01
            if nb != issued:
                ext |= 0x02
            m_cust = re.fullmatch(r'opensparse-account-(0|[1-9][0-9]*)', cust)
            if not m_cust:
                ext |= 0x04
            imp = implicit_expiry(issued, cur_meta[1]) if cur_meta[1] != 'custom' else None
            use_hours = False
            explicit_exp = False
            if imp is None or na != imp:
                if na > issued and (na - issued) % 3600 == 0:
                    ext |= 0x10
                    use_hours = True
                else:
                    ext |= 0x08
                    explicit_exp = True
            issued_off = issued - ST3_EPOCH
            raw_issued = not (0 <= issued_off <= 0xFFFFFFFF)
            if raw_issued:
                ext |= 0x20

            eb.append((PRODUCTS.index(cur_meta[0]) << 4) | TERMS.index(cur_meta[1]))
            if ext:
                flags |= 0x10
                eb.append(ext)
            if ext & 0x01:
                eb.extend(encode_bytes_u8(min_v.encode('ascii')))
                eb.extend(encode_bytes_u8(max_v.encode('ascii')))
            if ext & 0x04:
                eb.extend(encode_bytes_u8(cust.encode('ascii')))
            else:
                eb.extend(encode_uvarint(int(m_cust.group(1))))
            if raw_issued:
                eb.extend(encode_uvarint(issued))
            else:
                eb.extend(struct.pack('>I', issued_off))
            if ext & 0x02:
                eb.extend(encode_uvarint(nb))
            if use_hours:
                eb.extend(encode_uvarint((na - issued) // 3600))
            elif explicit_exp:
                eb.extend(encode_uvarint(0 if na == 0 else na - issued))
        eb.extend(sbytes)
        if custom_lid:
            eb.extend(encode_bytes_u8(ent['lid_suffix'].encode('ascii')))
        eb.extend(ent['sig'])
        buf.append(flags)
        buf.extend(eb)
        prev_meta = cur_meta
    return wrap_st3_bin(bytes(buf))


class LicensePasteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        env_bin = os.environ.get('STCLI_BINARY') or os.environ.get('GPU_ID_BINARY')
        if env_bin:
            cls.binary = Path(env_bin)
        else:
            cls.binary = cls.root / 'stcli'
            subprocess.run(
                ['cc', '-Wall', '-Wextra', '-Werror', '-O2', str(SOURCE / 'gpu_id.c'), '-ldl', '-o', str(cls.binary)],
                check=True,
            )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_stcli(self, *args, home=None, stdin_text=None, extra_env=None, binary=None):
        env = {k: v for k, v in os.environ.items() if k != 'NO_COLOR'}
        if home is not None:
            env['HOME'] = str(home)
            env['USERPROFILE'] = str(home)
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [str(binary or self.binary), *args],
            env=env,
            input=stdin_text,
            text=True,
            capture_output=True,
        )

    def _run_pty(self, args, writer_fn, home=None, extra_env=None):
        master, slave = pty.openpty()
        env = {k: v for k, v in os.environ.items() if k != 'NO_COLOR'}
        if home is not None:
            env['HOME'] = str(home)
            env['USERPROFILE'] = str(home)
        if extra_env:
            env.update(extra_env)
        proc = subprocess.Popen(
            [str(self.binary), *args],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=env,
            close_fds=True,
        )
        os.close(slave)
        out = bytearray()
        try:
            writer_fn(master, out)
            deadline = time.monotonic() + 8.0
            while proc.poll() is None and time.monotonic() < deadline:
                r, _, _ = select.select([master], [], [], 0.1)
                if r:
                    try:
                        chunk = os.read(master, 4096)
                        if not chunk:
                            break
                        out.extend(chunk)
                    except OSError:
                        break
            while True:
                r, _, _ = select.select([master], [], [], 0.05)
                if not r:
                    break
                try:
                    chunk = os.read(master, 4096)
                    if not chunk:
                        break
                    out.extend(chunk)
                except OSError:
                    break
            rc = proc.wait(timeout=3.0)
        finally:
            try:
                os.close(master)
            except OSError:
                pass
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        return rc, out.decode('utf-8', errors='replace')

    def test_st3_canonical_all_products_terms_calendar_serials_and_sizes(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()

            feb29 = int(datetime(2024, 1, 31, 12, 34, 56, tzinfo=timezone.utc).timestamp())
            feb28 = int(datetime(2025, 1, 31, 23, 59, 59, tzinfo=timezone.utc).timestamp())
            leap_yr = int(datetime(2024, 2, 29, 8, 0, 0, tzinfo=timezone.utc).timestamp())

            cases = [
                ('pangulu', 'hourly', '00001231', '0.1.0', '', 'opensparse-account-1', 1760000000, 1760000000, 1760003600, None),
                ('linglongamg', 'daily', 'hip-00010203-04050607-08090a0b-0c0d0e0f', '0.2.0', '2.0.0', 'opensparse-account-999999999999', 1760000100, 1760000050, 1760086500, None),
                ('xianzhikrylov', 'weekly', 'cuda-aabbccdd-eeff0011-22334455-66778899', '0.1.0', '', 'custom-corp-42', 1600000200, 1600000200, 1600605000, 'abcdef0123456789'),
                ('pangulu', 'monthly', 'musa-01020304-05060708-090a0b0c-0d0e0f10', '0.1.0', '', 'opensparse-account-7', feb29, feb29, implicit_expiry(feb29, 'monthly'), None),
                ('linglongamg', 'monthly', 'hc-11223344-55667788-99aabbcc-ddeeff00', '0.1.0', '', 'opensparse-account-8', feb28, feb28, implicit_expiry(feb28, 'monthly'), None),
                ('pangulu', 'annual', 'sycl-abcdef01-23456789-abcdef01-23456789', '0.1.0', '', 'opensparse-account-9', leap_yr, leap_yr, implicit_expiry(leap_yr, 'annual'), None),
                ('xianzhikrylov', 'perpetual', 'lisuan-00000000-00000000-00000000-00000000', '0.1.0', '', 'opensparse-account-0', 1760000500, 1760000500, 0, None),
                ('pangulu', 'custom', '00000000-0000-0003-0000-0000e20b8086', '0.1.0', '', 'opensparse-account-42', 1760000600, 1760000600, 1760000600 + 72 * 3600, None),
                ('pangulu', 'custom', 'LACM-LOONGSON-01', '0.1.0', '', 'opensparse-account-42', 1760000700, 1760000700, 1760001234, None),
            ]
            for idx, (prod, term, serial, min_v, max_v, cust, issued, nb, na, custom_lid) in enumerate(cases):
                blob, _, sig = make_key_blob(
                    product=prod, device=serial,
                    issued_at=issued, not_before=nb, not_after=na,
                    term=term, min_ver=min_v, max_ver=max_v,
                    customer=cust, lid_suffix=custom_lid,
                )
                code = encode_st3_canonical([{
                    'product': prod, 'term': term, 'device': serial,
                    'issued_at': issued, 'not_before': nb, 'not_after': na,
                    'min_ver': min_v, 'max_ver': max_v, 'customer': cust,
                    'lid_suffix': custom_lid, 'sig': sig,
                }])
                self.assertTrue(code.startswith('ST.'))
                if idx == 0:
                    self.assertEqual(len(code), 115)
                if idx in (3, 4, 5, 6):
                    self.assertEqual(len(code), 125)

                if idx % 3 == 0:
                    wrapped = '  ' + '\n  '.join(code[j:j + 40] for j in range(0, len(code), 40)) + '\t\r\n'
                    p = self.run_stcli('apply', home=home, stdin_text=wrapped)
                elif idx % 3 == 1:
                    p = self.run_stcli('apply', '--paste', home=home, stdin_text=code)
                else:
                    p = self.run_stcli('apply', code, home=home)
                self.assertEqual(p.returncode, 0, f'Case {idx} failed: {p.stderr}')
                self.assertIn('+ installed', p.stdout)
                self.assertIn(serial, p.stdout)

                p_list = self.run_stcli('list', '--json', home=home)
                self.assertEqual(p_list.returncode, 0, p_list.stderr)
                match = [r for r in json.loads(p_list.stdout)['licenses']
                         if r['product'] == prod and r['gpu_serial'] == serial]
                self.assertEqual(len(match), 1)
                self.assertEqual(Path(match[0]['path']).read_bytes(), blob)

    def test_st3_multi_card_64_raw_fallback_and_idempotency(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()

            entries = []
            expected_blobs = {}
            for i in range(64):
                dev = f'hip-000000{i:02x}-11223344-55667788-99aabbcc'
                if i == 5:
                    blob, payload, sig = make_key_blob(
                        product='linglongamg', device=dev,
                        extra_lines='note=custom-raw\n',
                    )
                    entries.append({'raw': True, 'payload': payload, 'sig': sig})
                else:
                    blob, _, sig = make_key_blob(product='linglongamg', device=dev, issued_at=1760000000, term='monthly')
                    entries.append({
                        'product': 'linglongamg', 'term': 'monthly', 'device': dev,
                        'issued_at': 1760000000, 'not_before': 1760000000, 'not_after': implicit_expiry(1760000000, 'monthly'),
                        'sig': sig,
                    })
                expected_blobs[dev] = blob

            code = encode_st3_canonical(entries)
            raw_total = sum(len(b) for b in expected_blobs.values())
            ratio = raw_total / len(code)
            self.assertGreater(ratio, 3.0, f'Expected >3x compression for 64 cards, got {ratio:.2f}x')

            p = self.run_stcli('apply', home=home, stdin_text=code)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.count('+ installed'), 64)

            p_list = self.run_cli_list = self.run_stcli('list', '--json', home=home)
            rows = json.loads(p_list.stdout)['licenses']
            self.assertEqual(len(rows), 64)
            for r in rows:
                self.assertEqual(Path(r['path']).read_bytes(), expected_blobs[r['gpu_serial']])

            p2 = self.run_stcli('apply', code, home=home)
            self.assertEqual(p2.returncode, 0, p2.stderr)
            self.assertEqual(p2.stdout.count('= unchanged'), 64)

    def test_st3_large_uvarint_and_multiple_code_args(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()

            cust_num = (1 << 63) + 5
            issued = 1 << 35
            na = issued + 3600
            blob1, _, sig1 = make_key_blob(
                product='pangulu', device='00009001',
                issued_at=issued, not_after=na, term='hourly', customer=f'opensparse-account-{cust_num}',
            )
            code1 = encode_st3_canonical([{
                'product': 'pangulu', 'term': 'hourly', 'device': '00009001',
                'issued_at': issued, 'not_after': na, 'customer': f'opensparse-account-{cust_num}',
                'sig': sig1,
            }])
            blob2, _, sig2 = make_key_blob(product='pangulu', device='00009002', issued_at=1760000000, term='daily')
            code2 = encode_st3_canonical([{
                'product': 'pangulu', 'term': 'daily', 'device': '00009002',
                'issued_at': 1760000000, 'not_after': implicit_expiry(1760000000, 'daily'), 'sig': sig2,
            }])

            p = self.run_stcli('apply', code1, code2, home=home)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.count('+ installed'), 2)
            self.assertEqual((home / '.solved-tech' / 'PanguLU' / 'pangulu-00009001.license.key').read_bytes(), blob1)
            self.assertEqual((home / '.solved-tech' / 'PanguLU' / 'pangulu-00009002.license.key').read_bytes(), blob2)

    @unittest.skipUnless(os.name == 'posix', 'PTY requires POSIX')
    def test_st3_paste_tty_asterisk_echo_backspace_and_modes(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()

            # 1) 24-GPU single-line paste on bare `stcli apply` WITHOUT trailing newline + verify '*' echo & no plaintext echo
            entries = []
            for i in range(24):
                dev = f'cuda-000000{i:02x}-01020304-05060708-090a0b0c'
                _, _, sig = make_key_blob(product='pangulu', device=dev, issued_at=1760000000, term='monthly')
                entries.append({
                    'product': 'pangulu', 'term': 'monthly', 'device': dev,
                    'issued_at': 1760000000, 'not_after': implicit_expiry(1760000000, 'monthly'), 'sig': sig,
                })
            big_code = encode_st3_canonical(entries)
            self.assertGreater(len(big_code), 2048)

            def write_no_newline(master, out):
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    r, _, _ = select.select([master], [], [], 0.1)
                    if r:
                        out.extend(os.read(master, 4096))
                        if b'Paste license code' in out:
                            break
                os.write(master, big_code.encode('ascii'))

            rc, out = self._run_pty(['apply'], write_no_newline, home=home)
            self.assertEqual(rc, 0, out)
            self.assertIn('Paste license code', out)
            self.assertEqual(out.count('*'), len(big_code))
            self.assertNotIn(big_code[:12], out)
            self.assertEqual(out.count('+ installed'), 24)

            # 2) Backspace (\x7f) + replacement char + multi-line wrapped paste on TTY
            blob2, _, sig2 = make_key_blob(product='linglongamg', device='GPU-TTY-WRAP', issued_at=1760000000, term='monthly')
            code2 = encode_st3_canonical([{
                'product': 'linglongamg', 'term': 'monthly', 'device': 'GPU-TTY-WRAP',
                'issued_at': 1760000000, 'not_after': implicit_expiry(1760000000, 'monthly'), 'sig': sig2,
            }])
            lines2 = [code2[i:i + 40] for i in range(0, len(code2), 40)]

            def write_with_backspace(master, out):
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    r, _, _ = select.select([master], [], [], 0.1)
                    if r:
                        out.extend(os.read(master, 4096))
                        if b'Paste license code' in out:
                            break
                os.write(master, (lines2[0] + 'X\x7f\n').encode('ascii'))
                time.sleep(0.02)
                for ln in lines2[1:]:
                    os.write(master, (ln + '\n').encode('ascii'))
                    time.sleep(0.02)

            rc2, out2 = self._run_pty(['apply', '--paste'], write_with_backspace, home=home)
            self.assertEqual(rc2, 0, out2)
            self.assertIn('\b \b', out2)
            self.assertEqual(out2.count('*'), len(code2) + 1)
            self.assertNotIn('ST.', out2)
            self.assertIn('+ installed', out2)
            self.assertEqual((home / '.solved-tech' / 'LinglongAMG' / 'linglongamg-GPU-TTY-WRAP.license.key').read_bytes(), blob2)

            # 3) Empty Enter on TTY exits 1 with clean message
            def write_empty(master, out):
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    r, _, _ = select.select([master], [], [], 0.1)
                    if r:
                        out.extend(os.read(master, 4096))
                        if b'Paste license code' in out:
                            break
                os.write(master, b'\n')

            rc3, out3 = self._run_pty(['apply'], write_empty, home=home)
            self.assertEqual(rc3, 1)
            self.assertIn('no license code received', out3)

    def test_st3_rejects_malformed_and_edge_cases(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()

            _, payload, sig = make_key_blob(product='pangulu', device='DEV-ERR', issued_at=1760000000, term='monthly')
            valid_entry = {
                'product': 'pangulu', 'term': 'monthly', 'device': 'DEV-ERR',
                'issued_at': 1760000000, 'not_after': implicit_expiry(1760000000, 'monthly'), 'sig': sig,
            }
            valid_code = encode_st3_canonical([valid_entry])
            valid_raw = base64.urlsafe_b64decode(valid_code[3:] + '=' * ((-len(valid_code[3:])) % 4))

            bad_tokens = [
                'STLIC1.' + valid_code[3:],
                'STLIC2.' + valid_code[3:],
                valid_code + '=',
                valid_code[:-1] + '+',
                'ST.A',
                'ST.Px',
                'ST.AAB',
                wrap_st3_bin(valid_raw[:-3])[:-1] + ('A' if valid_code[-1] != 'A' else 'B'),
                wrap_st3_bin(b'\x80' + valid_raw[1:-3]),  # wrong v3 hdr bits
                wrap_st3_bin(valid_raw[:-3] + b'\x00'),     # trailing byte
                wrap_st3_bin(b'\x40\x03' + encode_bytes_u8(payload) + sig),  # raw upper flag
                wrap_st3_bin(b'\x40\x01\x00' + sig),  # raw empty payload
                wrap_st3_bin(b'\x40\x01' + encode_bytes_u8(b'x' * 8193) + sig),
                wrap_st3_bin(b'\x40\x01' + encode_bytes_u8(b'a\x00b') + sig),
                wrap_st3_bin(b'\x40' + bytes([0x02]) + valid_raw[2:-3]),  # SAME_META on entry 0
                wrap_st3_bin(b'\x40' + bytes([0x08]) + valid_raw[2:-3]),  # reserved flag bit
                wrap_st3_bin(b'\x40\x00\x33' + valid_raw[2:-3]),          # unknown prod
                wrap_st3_bin(b'\x40\x00\x06\x01' + struct.pack('>I', 0) + sig),  # custom without expiry
            ]
            for bad in bad_tokens:
                p = self.run_stcli('apply', '--paste', home=home, stdin_text=bad)
                self.assertEqual(p.returncode, 1, f'Expected error for {bad[:40]}..., got {p.returncode}')

            p_arg_reject = self.run_stcli('apply', 'STLIC2.AAAA', home=home)
            self.assertEqual(p_arg_reject.returncode, 1)

    def test_st3_cross_interop_with_store_codec_and_sizes(self):
        stcli_codec_py = REPO_STORE_ROOT / 'store' / 'stcli_codec.py'
        venv_py = REPO_STORE_ROOT / '.venv' / 'bin' / 'python'
        if not (stcli_codec_py.exists() and venv_py.exists()):
            self.skipTest('store/stcli_codec.py not present in standalone tools/stcli checkout')

        single_blob = make_key_blob(product='pangulu', device='cuda-11223344-55667788-99aabbcc-ddeeff00', issued_at=1760000000, term='monthly')[0]
        four_blobs = [
            make_key_blob(product='pangulu', device=f'cuda-{i:08x}-55667788-99aabbcc-ddeeff00', issued_at=1760000000, term='monthly')[0]
            for i in range(4)
        ]
        mixed_blobs = [
            make_key_blob(product='pangulu', device='00001231', issued_at=1760000000, term='hourly')[0],
            make_key_blob(product='linglongamg', device='hip-00010203-04050607-08090a0b-0c0d0e0f',
                          min_ver='0.2.0', max_ver='3.0.0', issued_at=1760000000, not_before=1759999000,
                          term='custom', not_after=1760000000 + 72 * 3600,
                          customer='enterprise-custom-customer')[0],
            make_key_blob(product='xianzhikrylov', device='00000000-0000-0003-0000-0000e20b8086',
                          term='perpetual', not_after=0, license_tier='user', lid_suffix='0123456789abcdef')[0],
        ]
        script = (
            "import base64, django, json, sys; django.setup();"
            "from store.stcli_codec import encode_license_code;"
            "batches = json.loads(sys.argv[1]);"
            "print(json.dumps([encode_license_code([base64.b64decode(x) for x in b]) for b in batches]))"
        )
        res = subprocess.run(
            [str(venv_py), '-c', script, json.dumps([
                [base64.b64encode(single_blob).decode('ascii')],
                [base64.b64encode(b).decode('ascii') for b in four_blobs],
                [base64.b64encode(b).decode('ascii') for b in mixed_blobs],
            ])],
            cwd=str(REPO_STORE_ROOT),
            env={**os.environ, 'DJANGO_SETTINGS_MODULE': 'config.settings'},
            text=True,
            capture_output=True,
            check=True,
        )
        one_code, four_code, mixed_code = json.loads(res.stdout)
        self.assertTrue(one_code.startswith('ST.'))
        self.assertEqual(len(one_code), 125)
        self.assertEqual(len(four_code), 449)

        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()
            p = self.run_stcli('apply', home=home, stdin_text=mixed_code)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stdout.count('+ installed'), 3)
            p_list = self.run_stcli('list', '--json', home=home)
            installed = [Path(r['path']).read_bytes() for r in json.loads(p_list.stdout)['licenses']]
            self.assertEqual(sorted(installed), sorted(mixed_blobs))


if __name__ == '__main__':
    unittest.main(verbosity=2)
