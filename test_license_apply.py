#!/usr/bin/env python3
"""Integration tests for stcli `apply` (code-only) and `list` subcommands."""
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile
import time
import unittest
import uuid
import zipfile
import zlib

SOURCE = Path(__file__).resolve().parent
BINARY = Path(
    os.environ.get('STCLI_BINARY')
    or os.environ.get('GPU_ID_BINARY')
    or (SOURCE.parents[1] / 'artifacts/stcli/stcli-linux-x86_64')
)

PREFIX = {'pangulu': 'pgl-', 'linglongamg': 'lla-', 'xianzhikrylov': 'xzk-'}


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


def make_license_bytes(
    *,
    product='linglongamg',
    device='hip-00010203-04050607-08090a0b-0c0d0e0f',
    issued_at=1700000000,
    not_after=2500000000,
    sig_len=64,
    extra_lines='',
):
    customer = 'opensparse-account-42'
    lid_hex = hashlib.sha1(
        f'st-lid-v1\n{product}\n{customer}\n{issued_at}\n{issued_at}\n{not_after}\n{device}'.encode('ascii')
    ).hexdigest()[:16]
    lid = PREFIX.get(product, 'unk-') + str(issued_at) + '-' + lid_hex
    payload = (
        f'license_id={lid}\n'
        f'product={product}\n'
        f'min_ver=0.1.0\n'
        f'max_ver=\n'
        f'gpu_serial={device}\n'
        f'issued_at={issued_at}\n'
        f'not_before={issued_at}\n'
        f'not_after={not_after}\n'
        f'customer={customer}\n'
        f'license_tier=user\n'
        f'{extra_lines}'
    ).encode('ascii')
    sig = hashlib.sha512(payload).digest()[:sig_len]
    return (
        b'PGL-LICENSE-1\n'
        + base64.b64encode(payload)
        + b'\n'
        + base64.b64encode(sig)
        + b'\n'
    ), lid


def make_st_code(blobs):
    """Encode 1..64 raw license blobs into an ST. v3 code for integration tests."""
    out = bytearray([0x40 | (len(blobs) - 1)])
    for blob in blobs:
        lines = blob.split(b'\n')
        payload = base64.b64decode(lines[1])
        sig = base64.b64decode(lines[2])
        out.append(0x01)
        out.extend(encode_bytes_u8(payload))
        out.extend(sig)
    crc = zlib.crc32(out) & 0xFFFFFFFF
    out.extend(struct.pack('>I', crc)[:3])
    return 'ST.' + base64.urlsafe_b64encode(out).rstrip(b'=').decode('ascii')


class LicenseApplyTests(unittest.TestCase):
    def run_cli(self, *args, home=None, stdin_text=None, **extra_env):
        env = {k: v for k, v in os.environ.items() if k != 'NO_COLOR'}
        if home is not None:
            env['HOME'] = str(home)
            env['USERPROFILE'] = str(home)
        env.update(extra_env)
        return subprocess.run(
            [str(BINARY), *args],
            env=env,
            input=stdin_text,
            text=True,
            capture_output=True,
        )

    def test_single_code_install_for_all_three_products_default_user_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            home = root / 'home'
            home.mkdir()
            cases = [
                ('pangulu', 'PanguLU', '00001231'),
                ('linglongamg', 'LinglongAMG', 'GPU-1111-2222-3333'),
                ('xianzhikrylov', 'XianzhiKrylov', 'hc-0123456789abcdef'),
            ]
            for idx, (product, lib_name, serial) in enumerate(cases):
                blob, _ = make_license_bytes(product=product, device=serial)
                code = make_st_code([blob])
                if idx % 2 == 0:
                    p = self.run_cli('apply', home=home, stdin_text=code)
                else:
                    p = self.run_cli('apply', code, home=home)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertNotIn('\x1b[', p.stdout)
                self.assertIn(os.path.join('~', '.solved-tech'), p.stdout)
                self.assertIn(lib_name, p.stdout)
                self.assertIn('+ installed', p.stdout)
                self.assertIn(serial, p.stdout)

                dest_dir = home / '.solved-tech' / lib_name
                keys = list(dest_dir.glob('*.key'))
                self.assertEqual(len(keys), 1)
                dest = keys[0]
                self.assertEqual(dest.read_bytes(), blob)
                if os.name == 'posix':
                    self.assertEqual(stat.S_IMODE(dest.stat().st_mode), 0o600)
                    self.assertEqual(stat.S_IMODE((home / '.solved-tech').stat().st_mode), 0o700)
                    self.assertEqual(stat.S_IMODE(dest_dir.stat().st_mode), 0o700)

    def test_multi_code_batch_install_and_sanitized_device_filename(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()
            b1, lid1 = make_license_bytes(product='linglongamg', device='hip-30323133-66326536-39323461-31313634')
            b2, lid2 = make_license_bytes(product='linglongamg', device='PCI:0000:01:00')
            code = make_st_code([b1, b2])

            p = self.run_cli('apply', home=home, stdin_text=code)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn('+ installed', p.stdout)
            self.assertIn('PCI:0000:01:00', p.stdout)

            p_list = self.run_cli('list', '--json', home=home)
            self.assertEqual(p_list.returncode, 0, p_list.stderr)
            rows = json.loads(p_list.stdout)['licenses']
            self.assertEqual(len(rows), 2)
            by_dev = {r['gpu_serial']: r for r in rows}
            self.assertEqual(
                by_dev['hip-30323133-66326536-39323461-31313634']['filename'],
                'linglongamg-hip-30323133-66326536-39323461-31313634.license.key',
            )
            self.assertTrue(by_dev['PCI:0000:01:00']['filename'].startswith('linglongamg-PCI_0000_01_00-'))
            self.assertTrue(by_dev['PCI:0000:01:00']['filename'].endswith('.license.key'))
            self.assertEqual(Path(by_dev['PCI:0000:01:00']['path']).read_bytes(), b2)
            self.assertEqual(by_dev['PCI:0000:01:00']['license_id'], lid2)
            self.assertEqual(by_dev['hip-30323133-66326536-39323461-31313634']['license_id'], lid1)

    def test_same_device_replacement_preserves_other_gpus_and_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            home = root / 'home'
            target_root = home / '.solved-tech'
            lla_dir = target_root / 'LinglongAMG'
            lla_dir.mkdir(parents=True)
            pgl_dir = target_root / 'PanguLU'
            pgl_dir.mkdir(parents=True)

            old_a1, _ = make_license_bytes(product='linglongamg', device='GPU-A', issued_at=1700000001, not_after=1700003601)
            old_a2, _ = make_license_bytes(product='linglongamg', device='GPU-A', issued_at=1700000002, not_after=1700003602)
            other_b, _ = make_license_bytes(product='linglongamg', device='GPU-B', issued_at=1700000010, not_after=2500000010)
            pgl_a, _ = make_license_bytes(product='pangulu', device='GPU-A', issued_at=1700000020, not_after=2500000020)

            (lla_dir / 'license.key').write_bytes(old_a1)
            (lla_dir / 'gpu-001.license.key').write_bytes(old_a2)
            (lla_dir / 'linglongamg-GPU-B.license.key').write_bytes(other_b)
            (lla_dir / 'unrelated-notes.txt').write_text('keep me\n')
            (lla_dir / 'corrupt-other.key').write_text('not a key\n')
            (pgl_dir / 'pangulu-GPU-A.license.key').write_bytes(pgl_a)

            new_a, new_lid = make_license_bytes(
                product='linglongamg', device='GPU-A',
                issued_at=1750000000, not_after=2600000000,
            )
            new_c, _ = make_license_bytes(
                product='linglongamg', device='GPU-C',
                issued_at=1750000000, not_after=2600000000,
            )
            code = make_st_code([new_a, new_c])

            p = self.run_cli('apply', code, home=home)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn('↻ replaced', p.stdout)
            self.assertIn('+ installed', p.stdout)

            self.assertFalse((lla_dir / 'license.key').exists())
            self.assertFalse((lla_dir / 'gpu-001.license.key').exists())
            self.assertEqual((lla_dir / 'linglongamg-GPU-A.license.key').read_bytes(), new_a)
            self.assertEqual((lla_dir / 'linglongamg-GPU-B.license.key').read_bytes(), other_b)
            self.assertEqual((lla_dir / 'linglongamg-GPU-C.license.key').read_bytes(), new_c)
            self.assertEqual((pgl_dir / 'pangulu-GPU-A.license.key').read_bytes(), pgl_a)
            self.assertTrue((lla_dir / 'unrelated-notes.txt').exists())
            self.assertTrue((lla_dir / 'corrupt-other.key').exists())

            p2 = self.run_cli('apply', home=home, stdin_text=code)
            self.assertEqual(p2.returncode, 0, p2.stderr)
            self.assertEqual(p2.stdout.count('= unchanged'), 2)
            p_list = self.run_cli('list', '--json', home=home)
            by_dev = {r['gpu_serial']: r for r in json.loads(p_list.stdout)['licenses'] if r['lib_name'] == 'LinglongAMG'}
            self.assertEqual(by_dev['GPU-A']['license_id'], new_lid)

    def test_file_args_removed_options_rejected_and_empty_paste_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            home = root / 'home'
            home.mkdir()
            blob, _ = make_license_bytes(product='pangulu', device='00009999')
            src = root / 'pgl.key'
            src.write_bytes(blob)
            zsrc = root / 'bundle.licenses.zip'
            with zipfile.ZipFile(zsrc, 'w') as zf:
                zf.writestr('gpu-001.license.key', blob)

            for file_arg in (str(src), str(zsrc)):
                p = self.run_cli('apply', file_arg, home=home)
                self.assertEqual(p.returncode, 1)
                self.assertIn('invalid license code', p.stderr)

            empty = self.run_cli('apply', home=home, stdin_text='')
            self.assertEqual(empty.returncode, 1)
            self.assertIn('no license code received', empty.stderr)

            code = make_st_code([blob])
            for bad_args in [
                ('apply', '--root', str(root), code),
                ('apply', '--user', code),
                ('apply', '--dry-run', code),
                ('apply', '--text', code),
                ('apply', '--json', code),
                ('apply', '--paste', code),
                ('list', '--root', str(root)),
                ('list', '--user'),
                ('list', '--product', 'PanguLU'),
                ('list', '--text'),
            ]:
                p = self.run_cli(*bad_args, home=home)
                self.assertEqual(p.returncode, 2, f'Expected exit 2 for {bad_args}, got {p.returncode}: {p.stderr}')

            sys_list = self.run_cli('list', '--system', '--json', home=home)
            self.assertEqual(sys_list.returncode, 0, sys_list.stderr)
            self.assertEqual(json.loads(sys_list.stdout)['scope'], 'system')

    def test_rejects_bad_payload_duplicates_and_symlink_target_dir(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            home = root / 'home'
            home.mkdir()

            # 1. Duplicate (product, gpu_serial) in input batch stops before ANY write occurs
            b_other, _ = make_license_bytes(product='linglongamg', device='DEV-UNIQUE')
            b_dup1, _ = make_license_bytes(product='pangulu', device='DEV-DUP', issued_at=1700000001)
            b_dup2, _ = make_license_bytes(product='pangulu', device='DEV-DUP', issued_at=1700000002)
            p = self.run_cli('apply', make_st_code([b_other, b_dup1, b_dup2]), home=home)
            self.assertEqual(p.returncode, 1)
            self.assertIn('duplicate license', p.stderr)
            self.assertFalse((home / '.solved-tech').exists())

            # 2. Invalid signature length inside decoded payload
            bad_sig, _ = make_license_bytes(product='pangulu', device='DEV-BADSIG', sig_len=32)
            p_sig = self.run_cli('apply', make_st_code([bad_sig]), home=home)
            self.assertEqual(p_sig.returncode, 1)

            # 3. Symlink target base dir rejected
            b1, _ = make_license_bytes(product='pangulu', device='DEV-1')
            if os.name == 'posix':
                sym_home = root / 'sym_home'
                sym_home.mkdir()
                real_dir = root / 'real_dir'
                real_dir.mkdir()
                (sym_home / '.solved-tech').symlink_to(real_dir)
                p_sym = self.run_cli('apply', make_st_code([b1]), home=sym_home)
                self.assertEqual(p_sym.returncode, 1)
                self.assertIn('not a real directory', p_sym.stderr)

    def test_list_subcommand_tui_statuses_alignment_and_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / 'home'
            home.mkdir()

            p0 = self.run_cli('list', '--json', home=home)
            self.assertEqual(p0.returncode, 0, p0.stderr)
            j0 = json.loads(p0.stdout)
            self.assertEqual(j0['schema'], 'solved-tech.licenses.v1')
            self.assertEqual(j0['tool_version'], '1.3.0')
            self.assertEqual(j0['licenses'], [])

            p0_txt = self.run_cli('list', home=home)
            self.assertEqual(p0_txt.returncode, 0, p0_txt.stderr)
            self.assertNotIn('\x1b[', p0_txt.stdout)
            self.assertTrue(p0_txt.stdout.startswith(os.path.join('~', '.solved-tech')))
            self.assertIn('No licenses installed.', p0_txt.stdout)

            now = int(time.time())
            b_active_long, _ = make_license_bytes(product='pangulu', device='cuda-00001111-22223333-44445555', issued_at=now - 60, not_after=now + 10 * 86400)
            b_active_short, _ = make_license_bytes(product='pangulu', device='1323520020744', issued_at=now - 60, not_after=now + 12 * 3600)
            b_expired, _ = make_license_bytes(product='linglongamg', device='GPU-EXP', issued_at=1600000000, not_after=1600003600)
            b_perp, _ = make_license_bytes(product='xianzhikrylov', device='GPU-PERP', issued_at=1700000000, not_after=0)

            res = self.run_cli('apply', make_st_code([b_active_long, b_active_short, b_expired, b_perp]), home=home)
            self.assertEqual(res.returncode, 0, res.stderr)

            (home / '.solved-tech' / 'LinglongAMG' / 'broken.key').write_text('not-a-valid-key\n')

            p_all = self.run_cli('list', '--json', home=home)
            self.assertEqual(p_all.returncode, 0, p_all.stderr)
            rows = json.loads(p_all.stdout)['licenses']
            statuses = {(r['lib_name'], r['filename']): r['status'] for r in rows}
            self.assertEqual(statuses[('PanguLU', 'pangulu-cuda-00001111-22223333-44445555.license.key')], 'active')
            self.assertEqual(statuses[('PanguLU', 'pangulu-1323520020744.license.key')], 'active')
            self.assertEqual(statuses[('LinglongAMG', 'linglongamg-GPU-EXP.license.key')], 'expired')
            self.assertEqual(statuses[('LinglongAMG', 'broken.key')], 'invalid')
            self.assertEqual(statuses[('XianzhiKrylov', 'xianzhikrylov-GPU-PERP.license.key')], 'perpetual')

            p_tui = self.run_cli('list', home=home)
            self.assertEqual(p_tui.returncode, 0, p_tui.stderr)
            self.assertNotIn('\x1b[', p_tui.stdout)
            lines = p_tui.stdout.splitlines()
            self.assertEqual(lines[0], '~/.solved-tech')
            for expected in ('PanguLU', 'LinglongAMG', 'XianzhiKrylov', '● active', '● perpetual', '× expired', '! invalid', 'ago'):
                self.assertIn(expected, p_tui.stdout)

            short_line = next(ln for ln in lines if '1323520020744' in ln)
            long_line = next(ln for ln in lines if 'cuda-00001111-22223333-44445555' in ln)
            exp_line = next(ln for ln in lines if 'GPU-EXP' in ln)
            cols = [(re.search(r'\d{4}-\d{2}-\d{2}', ln).start()) for ln in (short_line, long_line, exp_line)]
            self.assertEqual(len(set(cols)), 1, f'Dates misaligned across rows: {lines}')

    def test_subcommand_help_brand(self):
        for sub in ('apply', 'list'):
            p = self.run_cli(sub, '--help')
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertIn(f'Usage: stcli {sub}', p.stdout)
            self.assertNotIn('--root', p.stdout)
            self.assertNotIn('--dry-run', p.stdout)
            self.assertNotIn('--product', p.stdout)
            self.assertNotIn('--user', p.stdout)
            self.assertNotIn('--text', p.stdout)
            self.assertNotIn('solved-tech-gpu-id', p.stdout)
        apply_help = self.run_cli('apply', '--help').stdout
        self.assertNotIn('.licenses.zip', apply_help)
        self.assertNotIn('.key', apply_help)


if __name__ == '__main__':
    unittest.main()
