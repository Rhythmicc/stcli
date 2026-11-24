"""Exercise the actual downloadable ELF with synthetic vendor API libraries."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import zlib

SOURCE = Path(__file__).resolve().parent
BINARY = Path(
    os.environ.get('STCLI_BINARY')
    or os.environ.get('GPU_ID_BINARY')
    or (SOURCE.parents[1] / 'artifacts/stcli/stcli-linux-x86_64')
)
CROCKFORD_ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
VENDORS = {
    'cuda': 'nvidia',
    'musa': 'moore_threads',
    'hcr': 'hcr',
    'intel': 'intel',
    'sycl': 'intel',
    'lisuan': 'lisuan',
    'lacm': 'loongson',
}


def format_b_checksum(backend, vendor, name, license_id):
    collapsed = re.sub(r'[ \t\r\n]+', ' ', name.strip())
    norm_name = ''.join(c.lower() if 'A' <= c <= 'Z' else c for c in collapsed)
    payload = f'{backend.lower()}\n{vendor.lower()}\n{norm_name}\n{license_id.strip()}'.encode('utf-8')
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    val = (crc >> 12) & 0xFFFFF
    return ''.join(CROCKFORD_ALPHABET[(val >> shift) & 31] for shift in (15, 10, 5, 0))


def parse_format_b(stdout):
    lines = stdout.splitlines()
    assert lines and len(lines) % 2 == 0, f'Expected even number of non-empty lines, got: {stdout!r}'
    rows = []
    for idx in range(0, len(lines), 2):
        header, body = lines[idx], lines[idx + 1]
        assert header.startswith('# '), f'Expected "# " header at line {idx + 1}: {header!r}'
        parts = [p.strip() for p in header[2:].split('|')]
        if len(parts) == 3:
            backend, vendor, name = parts
            assert backend == 'hip', f'Unexpected 3-part header for non-hip backend: {header!r}'
        elif len(parts) == 2:
            backend, name = parts
            assert backend != 'hip', f'HIP header must include vendor: {header!r}'
            vendor = VENDORS[backend]
        else:
            raise AssertionError(f'Invalid Format B header: {header!r}')
        m = re.fullmatch(r'([A-Za-z0-9_.:/-]{1,128}) \[([0-9A-HJKMNP-TV-Z]{4})\]', body)
        assert m, f'Invalid Format B ID line at line {idx + 2}: {body!r}'
        license_id, token = m.group(1), m.group(2)
        expected = format_b_checksum(backend, vendor, name, license_id)
        assert token == expected, f'Checksum mismatch for {license_id}: got {token}, expected {expected}'
        rows.append({'backend': backend, 'vendor': vendor, 'name': name, 'license_id': license_id, 'checksum': token})
    return rows


class HelperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        lib = cls.root / 'libcuda.so.1'
        subprocess.run(['gcc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', str(SOURCE / 'driver_fixture.c'), '-o', str(lib)], check=True)
        for name in ['libnvidia-ml.so.1', 'libmusa.so.1', 'libamdhip64.so',
                     'libhcruntime.so', 'libOpenCL.so.1', 'libze_loader.so.1']:
            (cls.root / name).symlink_to(lib.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_helper(self, *args, _binary=None, **env):
        base = {k: v for k, v in os.environ.items() if not k.startswith('FIXTURE_')}
        return subprocess.run([str(_binary or BINARY), *args], env={**base, 'LD_LIBRARY_PATH': str(self.root), **env}, text=True, capture_output=True)

    def test_default_format_b_per_card_headers_and_checksums(self):
        for args in [('gpu-id', '--backend', 'cuda'), ('--backend', 'cuda')]:
            p = self.run_helper(*args)
            self.assertEqual(p.returncode, 0, p.stderr)
            rows = parse_format_b(p.stdout)
            self.assertEqual([r['license_id'] for r in rows], ['00001231', '00001232'])
            self.assertEqual([r['name'] for r in rows], ['Fixture "GPU"', 'Fixture "GPU"'])
            self.assertEqual([r['backend'] for r in rows], ['cuda', 'cuda'])
            self.assertNotEqual(rows[0]['checksum'], rows[1]['checksum'])

    def test_display_name_sanitization(self):
        p = self.run_helper('gpu-id', '--backend', 'cuda', FIXTURE_COUNT='1', FIXTURE_NAME='RTX | 5060\tTi ')
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = parse_format_b(p.stdout)
        self.assertEqual(rows[0]['name'], 'RTX / 5060 Ti')

    def test_driver_cleanup_after_report_is_safe_and_still_runs(self):
        dispatcher = self.root / 'late-cleanup-dispatcher.so'
        subprocess.run(['gcc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(SOURCE / 'exit_fixture.c'), '-o', str(dispatcher)], check=True)
        for backend in ['hip', 'auto']:
            marker = self.root / ('cleanup-' + backend)
            p = self.run_helper('--backend', backend,
                                LD_PRELOAD=str(dispatcher), FIXTURE_LATE_CLEANUP='1', FIXTURE_COUNT='1',
                                FIXTURE_EXIT_MARKER=str(marker))
            self.assertEqual(p.returncode, 0, p.stderr)
            rows = parse_format_b(p.stdout)
            self.assertTrue(rows)
            self.assertEqual(marker.read_text(), 'cleanup\n')

    def test_serial_leading_zeros_preserved(self):
        p = self.run_helper('gpu-id', '--backend', 'cuda')
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = parse_format_b(p.stdout)
        self.assertEqual([r['license_id'] for r in rows], ['00001231', '00001232'])
        self.assertEqual(rows[0]['name'], 'Fixture "GPU"')

    def test_cuda_uuid_fallback(self):
        for invalid in ['0', 'N/A', 'Unknown', '']:
            p = self.run_helper('--backend', 'cuda', FIXTURE_SERIAL=invalid)
            self.assertEqual(p.returncode, 0, p.stderr)
            rows = parse_format_b(p.stdout)
            self.assertEqual(rows[0]['license_id'], 'cuda-00010203-04050607-08090a0b-0c0d0e0f')

    def test_musa_uuid_and_zero_pci(self):
        p = self.run_helper('--backend', 'musa')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(parse_format_b(p.stdout)[0]['license_id'], 'musa-00010203-04050607-08090a0b-0c0d0e0f')
        p = self.run_helper('--backend', 'musa', FIXTURE_ZERO_UUID='1')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual([r['license_id'] for r in parse_format_b(p.stdout)], ['musa-0000:01:00', 'musa-0000:02:00'])

    def test_driver_trailing_null_cannot_corrupt_metadata(self):
        p = self.run_helper('--backend', 'musa', FIXTURE_UUID_TRAILING_NUL='1')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(parse_format_b(p.stdout)[0]['backend'], 'musa')

    def test_hip_rejects_pci_fallback_and_placeholder_uuid(self):
        empty_pci = self.root / 'empty-pci'
        empty_pci.mkdir(exist_ok=True)
        empty_kfd = self.root / 'empty-kfd'
        empty_kfd.mkdir(exist_ok=True)
        binary = self.root / 'helper-hip-no-sysfs'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DGPU_ID_PCI_ROOT="' + str(empty_pci) + '"',
                        '-DGPU_ID_KFD_ROOT="' + str(empty_kfd) + '"',
                        str(SOURCE / 'gpu_id.c'), '-ldl', '-o', str(binary)], check=True)
        for setting in [{'FIXTURE_UUID_ERROR': '1'}, {'FIXTURE_ZERO_UUID': '1'}, {'FIXTURE_HIP_PLACEHOLDER_UUID': '1'}]:
            p = self.run_helper('--backend', 'hip', _binary=binary, **setting)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(p.stdout, '')
            self.assertIn('PCI address cannot uniquely identify a GPU card', p.stderr)
            self.assertNotIn('hip-0000:', p.stdout + p.stderr)

    def test_amd_vega_frontier_rocm452_sysfs_and_kfd_match_rocm721(self):
        expected_id = 'hip-30323133-66326536-39323461-31313634'
        pci = self.root / 'vega-pci'
        dev = pci / '0000:03:00.0'
        dev.mkdir(parents=True, exist_ok=True)
        kfd = self.root / 'vega-kfd'
        node = kfd / '1'
        node.mkdir(parents=True, exist_ok=True)
        kfd_dev = self.root / 'vega-kfd-dev'
        kfd_dev.write_text('')
        (dev / 'class').write_text('0x030000\n')
        (dev / 'vendor').write_text('0x1002\n')
        (dev / 'device').write_text('0x6863\n')
        (dev / 'unique_id').write_text('0213f2e6924a1164\n')
        (node / 'properties').write_text(f'domain 0\nlocation_id 768\nunique_id {int("0213f2e6924a1164", 16)}\n')
        binary = self.root / 'helper-vega'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DGPU_ID_PCI_ROOT="' + str(pci) + '"',
                        '-DGPU_ID_KFD_ROOT="' + str(kfd) + '"',
                        '-DGPU_ID_KFD_DEV="' + str(kfd_dev) + '"',
                        str(SOURCE / 'gpu_id.c'), '-ldl', '-o', str(binary)], check=True)
        # 1) ROCm 7.2.1 path via hipDeviceGetUuid (ASCII hex bytes).
        p = self.run_helper('--backend', 'hip', _binary=binary, FIXTURE_COUNT='1',
                            FIXTURE_NAME='Radeon Vega Frontier Edition', FIXTURE_PCI='0000:03:00.0',
                            FIXTURE_UUID_ASCII='0213f2e6924a1164')
        self.assertEqual(p.returncode, 0, p.stderr)
        row = parse_format_b(p.stdout)[0]
        self.assertEqual(row['license_id'], expected_id)
        self.assertEqual(row['vendor'], 'amd')
        # 2) ROCm 4.5.2 path: libamdhip64.so.4 under $ROCM_PATH/hip/lib without hipDeviceGetUuid.
        rocm452 = self.root / 'rocm-4.5.2'
        hip_lib_dir = rocm452 / 'hip' / 'lib'
        hip_lib_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(['gcc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-DFIXTURE_OMIT_HIP_UUID',
                        str(SOURCE / 'driver_fixture.c'), '-o', str(hip_lib_dir / 'libamdhip64.so.4')], check=True)
        base = {k: v for k, v in os.environ.items() if not k.startswith('FIXTURE_') and k not in ('LD_LIBRARY_PATH', 'ROCM_PATH', 'HIP_PATH')}
        p = subprocess.run([str(binary), '--backend', 'hip'],
                           env={**base, 'ROCM_PATH': str(rocm452), 'FIXTURE_COUNT': '1',
                                'FIXTURE_NAME': 'Radeon Vega Frontier Edition', 'FIXTURE_PCI': '0000:03:00.0'},
                           text=True, capture_output=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        row = parse_format_b(p.stdout)[0]
        self.assertEqual(row['license_id'], expected_id)
        self.assertEqual(row['vendor'], 'amd')
        # 3) KFD topology fallback when PCI unique_id is absent.
        (dev / 'unique_id').unlink()
        p = subprocess.run([str(binary), '--backend', 'hip'],
                           env={**base, 'ROCM_PATH': str(rocm452), 'FIXTURE_COUNT': '1',
                                'FIXTURE_NAME': 'Radeon Vega Frontier Edition', 'FIXTURE_PCI': '0000:03:00.0'},
                           text=True, capture_output=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(parse_format_b(p.stdout)[0]['license_id'], expected_id)
        # 4) Zero unique_id and absent KFD must fail closed with no stdout output.
        (dev / 'unique_id').write_text('0000000000000000\n')
        (node / 'properties').write_text('domain 0\nlocation_id 768\nunique_id 0\n')
        p = subprocess.run([str(binary), '--backend', 'hip'],
                           env={**base, 'ROCM_PATH': str(rocm452), 'FIXTURE_COUNT': '1',
                                'FIXTURE_NAME': 'Radeon Vega Frontier Edition', 'FIXTURE_PCI': '0000:03:00.0'},
                           text=True, capture_output=True)
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, '')
        self.assertIn('hipDeviceGetUuid unavailable', p.stderr)

    def test_linux_env_diagnostics_and_no_output_or_file_on_error(self):
        pci = self.root / 'diag-pci'
        dev = pci / '0000:03:00.0'
        dev.mkdir(parents=True, exist_ok=True)
        (dev / 'class').write_text('0x030000\n')
        (dev / 'vendor').write_text('0x1002\n')
        (dev / 'device').write_text('0x6863\n')
        missing_kfd = self.root / 'missing-kfd'
        binary = self.root / 'helper-diag'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DGPU_ID_PCI_ROOT="' + str(pci) + '"',
                        '-DGPU_ID_KFD_DEV="' + str(missing_kfd) + '"',
                        str(SOURCE / 'gpu_id.c'), '-ldl', '-o', str(binary)], check=True)
        empty_lib = self.root / 'empty-lib'
        empty_lib.mkdir(exist_ok=True)
        base = {k: v for k, v in os.environ.items() if not k.startswith('FIXTURE_') and k not in ('LD_LIBRARY_PATH', 'ROCM_PATH', 'HIP_PATH')}
        p = subprocess.run([str(binary), '--backend', 'hip'],
                           env={**base, 'LD_LIBRARY_PATH': str(empty_lib)}, text=True, capture_output=True)
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, '')
        self.assertIn('1002:6863', p.stderr)
        self.assertIn('/dev/kfd is missing', p.stderr)
        p = self.run_helper('--backend', 'hip', _binary=binary, FIXTURE_COUNT='0', ROCR_VISIBLE_DEVICES='')
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, '')
        self.assertIn('ROCR_VISIBLE_DEVICES', p.stderr)

    def test_hip_vendor_name_and_ambiguous_family(self):
        for name, vendor in [('Z100', 'hygon'), ('HYGON DCU', 'hygon'),
                             ('AMD Radeon RX 9070 XT', 'amd'), ('Radeon Vega Frontier Edition', 'amd'),
                             ('Vega 10', 'amd'), ('Unspecified GPU', 'hip_unknown')]:
            p = self.run_helper('--backend', 'hip', FIXTURE_NAME=name, FIXTURE_PCI='not-a-bdf')
            self.assertEqual(p.returncode, 0, p.stderr)
            row = parse_format_b(p.stdout)[0]
            self.assertEqual(row['vendor'], vendor)
            self.assertEqual(row['name'], name)
            self.assertEqual(row['license_id'], 'hip-00010203-04050607-08090a0b-0c0d0e0f')

    def test_hip_pci_vendor_reading_precedence_and_malformed_input(self):
        pci = self.root / 'pci'
        pci.mkdir(exist_ok=True)
        device = pci / '0000:99:00.0'
        device.mkdir(exist_ok=True)
        binary = self.root / 'helper-with-test-sysfs'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DGPU_ID_PCI_ROOT="' + str(pci) + '"', str(SOURCE / 'gpu_id.c'),
                        '-ldl', '-o', str(binary)], check=True)
        for value, name, vendor in [
            ('0x1d94\n', 'Z100', 'hygon'),
            ('0x1002\n', 'Z100', 'amd'),
            ('0x1d94\n', 'AMD misleading name', 'hygon'),
            ('0x1234\n', 'Unknown GPU', 'hip_unknown'),
            ('garbage', 'Z100', 'hygon'),
            ('0x1d94junk', 'Unknown GPU', 'hip_unknown'),
            ('0x100000', 'Unknown GPU', 'hip_unknown')]:
            (device / 'vendor').write_text(value)
            (device / 'device').write_text('0x54b7\n')
            p = self.run_helper('--backend', 'hip', _binary=binary, FIXTURE_NAME=name,
                                FIXTURE_PCI='0000:99:00.0')
            self.assertEqual(p.returncode, 0, p.stderr)
            row = parse_format_b(p.stdout)[0]
            self.assertEqual(row['vendor'], vendor)
        for bdf in ['../../etc/passwd', '0000:99:00.0/../../vendor', '0000:99:20.0', '0000:98:00.0']:
            p = self.run_helper('--backend', 'hip', _binary=binary, FIXTURE_NAME='Unknown GPU', FIXTURE_PCI=bdf)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(parse_format_b(p.stdout)[0]['vendor'], 'hip_unknown')

    def test_bw_sysfs_does_not_crash_and_emits_hygon(self):
        pci = self.root / 'activity-pci'
        device = pci / '0000:98:00.0'
        device.mkdir(parents=True, exist_ok=True)
        binary = self.root / 'helper-activity-metadata'
        subprocess.run(['gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-DGPU_ID_PCI_ROOT="' + str(pci) + '"',
                        str(SOURCE / 'gpu_id.c'), '-ldl', '-o', str(binary)], check=True)
        for key, value in [('vendor', '0x1d94'), ('device', '0xffff'), ('subsystem_vendor', '0x1d94'),
                           ('subsystem_device', '0xfffe'), ('mem_info_vram_total', str(64 * 1024 ** 3)),
                           ('product_number', '01-SYNTHETIC'), ('vbios_version', 'TEST-ONLY-VBIOS')]:
            (device / key).write_text(value + '\n')
        result = self.run_helper('--backend', 'hip', _binary=binary, FIXTURE_NAME='BW', FIXTURE_PCI='0000:98:00.0')
        self.assertEqual(result.returncode, 0, result.stderr)
        row = parse_format_b(result.stdout)[0]
        self.assertEqual(row['name'], 'BW')
        self.assertEqual(row['vendor'], 'hygon')
        self.assertEqual(row['license_id'], 'hip-00010203-04050607-08090a0b-0c0d0e0f')

    def test_hcr_matches_hpcc_prefix_and_rejects_invalid_uuid(self):
        p = self.run_helper('--backend', 'hcr')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(parse_format_b(p.stdout)[0]['license_id'], 'hc-00010203-04050607-08090a0b-0c0d0e0f')
        for setting in [{'FIXTURE_UUID_ERROR': '1'}, {'FIXTURE_ZERO_UUID': '1'}]:
            p = self.run_helper('--backend', 'hcr', **setting)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(p.stdout, '')

    def test_duplicate_ids_rejected(self):
        p = self.run_helper('--backend', 'cuda', FIXTURE_SERIAL='00001')
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, '')
        self.assertIn('duplicate', p.stderr)

    def test_no_device_init_error_and_count_bounds(self):
        for setting in [{'FIXTURE_COUNT': '0'}, {'FIXTURE_INIT': '100'}, {'FIXTURE_COUNT': '300'}, {'FIXTURE_COUNT': '-1'}]:
            p = self.run_helper('--backend', 'cuda', **setting)
            self.assertEqual(p.returncode, 1)
            self.assertEqual(p.stdout, '')

    def test_invalid_cli(self):
        for args in [('--backend', 'bad'), ('--output',), ('--output', 'report.txt'), ('gpu-id', '--output', 'report.txt'), ('--json',), ('--text',), ('--plain',), ('gpu-id', '--json'), ('--oops',)]:
            self.assertEqual(self.run_helper(*args).returncode, 2, args)

    def test_intel_level_zero_uuid_canonical(self):
        p = self.run_helper('--backend', 'intel')
        self.assertEqual(p.returncode, 0, p.stderr)
        rows = parse_format_b(p.stdout)
        self.assertEqual([r['license_id'] for r in rows], ['00000000-0000-0003-0000-0000e20b8086'])
        self.assertEqual(rows[0]['backend'], 'intel')

    def test_auto_skips_uninitialized_intel_loader_but_keeps_visible_gpus(self):
        for args in [('gpu-id',), ('gpu-id', '--backend', 'auto'), ('--backend', 'auto')]:
            p = self.run_helper(*args, FIXTURE_ZE_INIT='2013265921')
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertEqual(p.stderr, '')
            rows = parse_format_b(p.stdout)
            self.assertIn('cuda', {row['backend'] for row in rows})
            self.assertNotIn('intel', {row['backend'] for row in rows})

    def test_explicit_intel_still_reports_uninitialized_driver(self):
        p = self.run_helper('--backend', 'intel', FIXTURE_ZE_INIT='2013265921')
        self.assertEqual(p.returncode, 1)
        self.assertIn('2013265921', p.stderr)
        self.assertEqual(p.stdout, '')

    def test_auto_keeps_other_intel_errors_fatal(self):
        p = self.run_helper('gpu-id', FIXTURE_ZE_INIT='1879048193')
        self.assertEqual(p.returncode, 1)
        self.assertIn('intel:', p.stderr)
        self.assertEqual(p.stdout, '')
        p = self.run_helper('gpu-id', FIXTURE_ZERO_UUID='1', FIXTURE_COUNT='1')
        self.assertEqual(p.returncode, 1)
        self.assertIn('zero device UUID', p.stderr)
        self.assertEqual(p.stdout, '')

    def test_auto_without_any_visible_gpu_still_fails(self):
        p = self.run_helper('gpu-id', FIXTURE_COUNT='0', FIXTURE_ZE_INIT='2013265921')
        self.assertEqual(p.returncode, 1)
        self.assertIn('GPU:', p.stderr)
        self.assertNotIn('Level Zero initialization failed', p.stderr)
        self.assertEqual(p.stdout, '')

    def test_intel_rejects_zero_uuid(self):
        p = self.run_helper('--backend', 'intel', FIXTURE_ZERO_UUID='1')
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, '')

    def test_lisuan_opencl_uuid_zero_and_nonzero(self):
        p = self.run_helper('--backend', 'lisuan')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual([r['license_id'] for r in parse_format_b(p.stdout)], ['lisuan-00000000-00000000-00000000-00000000'])
        p = self.run_helper('--backend', 'lisuan', FIXTURE_OPENCL_UUID='1')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual([r['license_id'] for r in parse_format_b(p.stdout)], ['lisuan-20212223-24252627-28292a2b-2c2d2e2f'])

    def test_lisuan_missing_device_is_incomplete(self):
        p = self.run_helper('--backend', 'lisuan', FIXTURE_COUNT='0')
        self.assertEqual(p.returncode, 1)
        self.assertEqual(p.stdout, '')

    def test_version_and_help_brand(self):
        p = self.run_helper('--version')
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout.strip(), '1.3.0')
        h = self.run_helper('--help')
        self.assertEqual(h.returncode, 0, h.stderr)
        self.assertIn('stcli', h.stdout)
        self.assertNotIn('solved-tech-gpu-id', h.stdout)
        self.assertNotIn('opensparse-gpu-id', h.stdout)

    def test_bare_command_prints_help_and_exits_zero(self):
        bare = self.run_helper(FIXTURE_COUNT='0', FIXTURE_ZE_INIT='1879048193')
        self.assertEqual(bare.returncode, 0, bare.stderr)
        self.assertEqual(bare.stderr, '')
        self.assertNotIn('\x1b[', bare.stdout)
        self.assertNotIn('Copy the License ID', bare.stdout)
        for token in ('stcli', 'gpu-id', 'apply', 'list', 'Usage'):
            self.assertIn(token, bare.stdout)
        hlp = self.run_helper('help', FIXTURE_COUNT='0')
        self.assertEqual(hlp.returncode, 0, hlp.stderr)
        self.assertEqual(hlp.stdout, bare.stdout)
        gh = self.run_helper('gpu-id', '--help')
        self.assertEqual(gh.returncode, 0, gh.stderr)
        self.assertIn('Usage: stcli gpu-id', gh.stdout)
        for removed in ('--json', '--text', '--plain'):
            self.assertNotIn(removed, gh.stdout)
        gid = self.run_helper('gpu-id', '--backend', 'cuda')
        self.assertEqual(gid.returncode, 0, gid.stderr)
        self.assertEqual(len(parse_format_b(gid.stdout)), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
