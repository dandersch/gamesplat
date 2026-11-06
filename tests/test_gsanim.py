"""CPU GSAnim conformance: python -m unittest discover -s tests -p test_gsanim.py.

Requires NumPy and SDL3 development files.
All builds and generated fixtures are temporary; no renderer/GPU is involved.
"""
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'animation_poc'))
import codec
import test_codec as reference_tests


class GSAnimTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name) / 'decode'
        miniz = Path(cls.tmp.name) / 'miniz.o'
        subprocess.run(['cc', '-O2', '-Ivendor', '-c', 'vendor/miniz.c', '-o', str(miniz)],
                       cwd=ROOT, check=True)
        subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-O2',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I.', '-Ivendor',
                        'tests/gsanim_decode.cpp', 'src/gaussian_loader_gsanim.cpp',
                        str(miniz), '-lSDL3', '-o', str(cls.exe)],
                       cwd=ROOT, check=True)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'fixture.gsanim'
        self.base = np.zeros((2, 62), dtype='<f4')
        self.base[:, :3] = [[1, -2, 3], [-7, 8, 9]]
        self.base[:, codec.SH] = (np.arange(96).reshape(2, 48) - 37) / 19
        self.base[:, 54:58] = [[-2, -1, 0, 1], [3, 2, -2, .5]]
        self.base[:, 58:62] = [[2, 1, -3, 4], [-1, 2, 3, -4]]
        self.q = ((np.arange(192).reshape(2, 2, 48) * 17) % 255 - 127).astype('i1')
        self.q[0, 0, 0] = 0
        self.scales = np.linspace(.001, .07, 48, dtype='<f4')
        self.m = dict(format='gsanim', version='0.1', sh_degree=3, gaussian_count=2,
                      state_count=3, coordinate_system='blender_world_z_up_meters',
                      color_encoding='display_referred_agx_baked', duration_seconds=8,
                      interpolation='step', keys=[dict(time_seconds=0, state=2),
                          dict(time_seconds=2, state=0), dict(time_seconds=5, state=1)])

    def entries(self):
        return {'manifest.json': json.dumps(self.m).encode(),
                'base.ply': codec.write_ply(self.base),
                'scales.bin': self.scales.tobytes(), 'deltas.bin': self.q.tobytes()}

    def pack(self, entries=None, compression=zipfile.ZIP_STORED):
        if entries is None:
            entries = self.entries()
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(self.path, 'w', compression=compression) as z:
                for name, value in (entries.items() if isinstance(entries, dict) else entries):
                    z.writestr(name, value)

    def run_probe(self, path=None, *args):
        return subprocess.run([str(self.exe), str(path or self.path), *map(str, args)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)

    def compare(self, path, times):
        asset = codec.load(path)
        result = self.run_probe(path, *times)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        actual = np.frombuffer(result.stdout, dtype='f4').reshape(len(times), -1, 48)
        for i, t in enumerate(times):
            np.testing.assert_allclose(actual[i], codec.appearance(asset, t), rtol=3e-6, atol=2e-6)
        return [tuple(map(float, line.split())) for line in result.stderr.decode().splitlines()]

    def test_real_asset_all_coefficients_all_states(self):
        path = ROOT / 'animation_poc/tv_loop.gsanim'
        if not path.exists():
            path = ROOT / 'animation_poc_old/tv_loop.gsanim'
        self.compare(path, [0, .1, .2, .3, .4, -.1])

    def test_asymmetric_step_linear_and_single_key(self):
        times = [0, .5, 1.999, 2, 2.75, 5, 6, 7.999, 8, -1, -8, 17]
        for mode in ['step', 'linear']:
            with self.subTest(mode=mode):
                self.m['interpolation'] = mode
                self.pack(compression=zipfile.ZIP_DEFLATED)
                samples = self.compare(self.path, times)
                for i, state in [(0, 2), (3, 0), (5, 1), (8, 2), (10, 2)]:
                    self.assertEqual(samples[i][0], state)
                    self.assertEqual(samples[i][2], 0)
                if mode == 'linear':
                    self.assertEqual(samples[1], (2, 0, .25))
                    self.assertEqual(samples[4], (0, 1, .25))
                    self.assertAlmostEqual(samples[6][2], 1/3, places=7)
                    self.assertEqual(samples[6][:2], (1, 2))
                original = self.m['keys']
                self.m['keys'] = [dict(time_seconds=0, state=1)]
                self.pack()
                self.assertTrue(all(s == (1, 1, 0) for s in self.compare(self.path, times)))
                self.m['keys'] = original

    def test_geometry_and_original_row_order(self):
        self.pack()
        r = self.run_probe(None, 'geometry')
        self.assertEqual(r.returncode, 0, r.stderr)
        geometry = np.frombuffer(r.stdout, dtype='f4').reshape(2, 11)
        expected = np.concatenate([self.base[:, :3], np.exp(self.base[:, 55:58]),
                                   self.base[:, 58:62] / np.linalg.norm(self.base[:, 58:62], axis=1)[:, None],
                                   (1/(1+np.exp(-self.base[:, 54])))[:, None]], axis=1)
        np.testing.assert_allclose(geometry, expected, rtol=1e-6)

    def test_invalid_manifests(self):
        cases = [('format', 'other'), ('version', '0.4'), ('sh_degree', 2),
                 ('coordinate_system', 'other'), ('color_encoding', 'linear'),
                 ('gaussian_count', 0), ('gaussian_count', 3), ('gaussian_count', 1000001),
                 ('gaussian_count', True), ('gaussian_count', 2.0),
                 ('state_count', 1), ('state_count', 257), ('state_count', 3.5),
                 ('duration_seconds', 0), ('duration_seconds', -1),
                 ('duration_seconds', float('inf')), ('duration_seconds', True),
                 ('interpolation', 'cubic'), ('keys', []),
                 ('keys', [dict(time_seconds=1, state=0)]),
                 ('keys', [dict(time_seconds=0, state=3)]),
                 ('keys', [dict(time_seconds=0, state=-1)]),
                 ('keys', [dict(time_seconds=0, state=True)]),
                 ('keys', [dict(time_seconds=0, state=0), dict(time_seconds=0, state=1)]),
                 ('keys', [dict(time_seconds=0, state=0), dict(time_seconds=8, state=1)])]
        for field, value in cases:
            with self.subTest(field=field, value=value):
                m = copy.deepcopy(self.m); m[field] = value
                entries = self.entries(); entries['manifest.json'] = json.dumps(m).encode()
                self.pack(entries)
                self.assertEqual(self.run_probe().returncode, 1)
        for field in self.m:
            m = copy.deepcopy(self.m); del m[field]
            entries = self.entries(); entries['manifest.json'] = json.dumps(m).encode()
            self.pack(entries)
            self.assertEqual(self.run_probe().returncode, 1, field)
        text = self.entries()['manifest.json']
        for bad in [text + b' {}', text.replace(b',', b'', 1), text[:-1] + b',}',
                    text.replace(b'"format":', b'"format"'),
                    text[:-1] + b',"state_count":3}',
                    text.replace(b'"version": "0.1"', b'"version": "\\q"'),
                    b' ' * (1024*1024+1), b'[' * 40 + b']' * 40]:
            entries = self.entries(); entries['manifest.json'] = bad
            self.pack(entries)
            self.assertEqual(self.run_probe().returncode, 1)

    def test_bad_payloads_and_archive(self):
        original = self.entries()
        variants = []
        for name in original:
            variants.append({k: v for k, v in original.items() if k != name})
            variants.append({**original, name: original[name][:-1]})
            variants.append({**original, name: original[name] + b'X'})
        variants += [{**original, '../bad': b'x'}, list(original.items()) + [('base.ply', original['base.ply'])],
                     {**original, 'deltas.bin': b'\x80' + original['deltas.bin'][1:]}]
        for value in [0., -1., float('nan'), float('inf'), 3e38]:
            variants.append({**original, 'scales.bin': struct.pack('<f', value) + original['scales.bin'][4:]})
        for index, value in [(0, float('nan')), (3, float('inf')), (55, 1000), (55, -1000), (58, 3e38)]:
            b = self.base.copy(); b[0, index] = value
            variants.append({**original, 'base.ply': codec.write_ply(b)})
        for old, new in [(b'float x', b'float y'), (b'float x', b'double x'),
                         (b'element vertex 2', b'element vertex 3'),
                         (b'end_header', b'element face 1\nend_header'),
                         (b'\n', b'\r\n')]:
            variants.append({**original, 'base.ply': original['base.ply'].replace(old, new)})
        for i, entries in enumerate(variants):
            with self.subTest(case=i):
                self.pack(entries)
                self.assertEqual(self.run_probe().returncode, 1)
        self.pack(compression=zipfile.ZIP_BZIP2)
        self.assertEqual(self.run_probe().returncode, 1)
        # Corrupt each stored payload without updating its ZIP CRC.
        for name in original:
            self.pack()
            with zipfile.ZipFile(self.path) as z:
                offset = z.getinfo(name).header_offset
            blob = bytearray(self.path.read_bytes())
            n, extra = struct.unpack_from('<HH', blob, offset + 26)
            blob[offset + 30 + n + extra] ^= 1
            self.path.write_bytes(blob)
            self.assertEqual(self.run_probe().returncode, 1, name)
        self.pack()
        self.path.write_bytes(self.path.read_bytes()[:-10])
        self.assertEqual(self.run_probe().returncode, 1)

    def test_comments_metadata_and_repeated_states(self):
        self.m['provenance'] = {'unknown': ['✓', {'deep': True}, None]}
        self.m['keys'][1]['state'] = 2
        e = self.entries()
        e['base.ply'] = e['base.ply'].replace(b'element vertex', b'comment allowed\nelement vertex')
        self.pack(e)
        self.compare(self.path, [0, 2, 5, 8])
        e['manifest.json'] = e['manifest.json'].replace(b'"format"', b'"\\u0066ormat"').replace(b'"0.1"', b'"\\u0030.1"')
        self.pack(e)
        self.compare(self.path, [0, 2, 5, 8])

    def test_mixed_02_outgoing_modes(self):
        self.m['version'] = '0.2'
        times = [0, .5, 1.999, 2, 2.75, 4.999, 5, 6, 7.999, 8, -1, -8, 17]
        for global_mode in ['step', 'linear']:
            self.m['interpolation'] = global_mode
            for key, mode in zip(self.m['keys'], ['linear', 'step', 'linear']):
                key['interpolation'] = mode
            self.pack()
            samples = self.compare(self.path, times)
            self.assertEqual(samples[1], (2, 0, .25))
            self.assertEqual(samples[4], (0, 0, 0))
            self.assertEqual(samples[7][:2], (1, 2))
        for index in range(3):
            for value in [None, 'cubic', True]:
                m = copy.deepcopy(self.m)
                if value is None:
                    del m['keys'][index]['interpolation']
                else:
                    m['keys'][index]['interpolation'] = value
                e = self.entries(); e['manifest.json'] = json.dumps(m).encode()
                self.pack(e)
                self.assertEqual(self.run_probe().returncode, 1)
        self.m['keys'] = [dict(time_seconds=0, state=1, interpolation='linear')]
        self.pack()
        self.assertTrue(all(s == (1, 1, 0) for s in self.compare(self.path, times)))

    def compare_layers(self, path):
        asset = codec.load(path)
        times = [0, .5, .999999, 1, 1.49, 1.5, 1.75, 1.99999, -.25, -2, 2, 4.5]
        r = self.run_probe(path, *times)
        self.assertEqual(r.returncode, 0, r.stderr)
        actual = np.frombuffer(r.stdout, dtype='f4').reshape(len(times), -1, 3)
        for i, t in enumerate(times):
            np.testing.assert_allclose(actual[i], codec.layer_weights(asset[0], t), rtol=1e-6, atol=1e-6)
        for mode, expected in [('layers', asset[2].astype('<f2').view('<u2')), ('lut', asset[3])]:
            r = self.run_probe(path, mode)
            self.assertEqual(r.returncode, 0, r.stderr)
            np.testing.assert_array_equal(np.frombuffer(r.stdout, dtype='<u2'), expected.ravel())
            if mode == 'lut':
                d = asset[0]['display_transform']
                self.assertEqual(tuple(map(float, r.stderr.split())), (d['size'], d['log2_min'], d['log2_max']))

    def test_layers_reference_fixture_and_export(self):
        path = reference_tests.LayerTests().fixture(self.directory.name)
        self.compare_layers(path)
        a = codec.load(path)
        lut = np.random.default_rng(2).random((3, 3, 3, 3))
        codec.export_layers(a[1], a[2], [l['keys'] for l in a[0]['layers']],
                            ['steady', 'lamp'], lut, -2, 3, 2, self.path, 'test')
        self.compare_layers(self.path)

    def test_real_layers_asset(self):
        path = ROOT / 'animation_poc/coastal_layers_pilot.gsanim'
        self.compare_layers(path)
        validation = json.loads((ROOT / 'animation_poc/layers_validation.json').read_text())
        checks = validation['checks']
        result = self.run_probe(path, *(c['seconds'] for c in checks))
        self.assertEqual(result.returncode, 0, result.stderr)
        weights = np.frombuffer(result.stdout, dtype='f4').reshape(len(checks), -1, 3)
        names = [layer['name'] for layer in codec.load(path)[0]['layers']]
        for actual, check in zip(weights, checks):
            np.testing.assert_allclose(actual, [check['layer_weights'][n] for n in names], atol=1e-6)

    def test_layer_count_and_half_boundaries(self):
        path = reference_tests.LayerTests().fixture(self.directory.name)
        with zipfile.ZipFile(path) as z:
            original = {n: z.read(n) for n in z.namelist()}
        m = json.loads(original['manifest.json'])
        bits = np.array([0, 0x8000, 1, 0x8001, 0x03ff, 0x0400, 0x7bff, 0xfbff], dtype='<u2')
        self.pack({**original, 'layers.bin': np.resize(bits, 192).tobytes()})
        self.compare_layers(self.path)
        for count in [256, 257]:
            m['layers'] = [dict(name=str(i), keys=[dict(time_seconds=0, weight=[1, 2, 3], interpolation='step')]) for i in range(count)]
            self.pack({**original, 'manifest.json': json.dumps(m).encode(), 'layers.bin': bytes(count*2*48*2)})
            self.assertEqual(self.run_probe().returncode, 0 if count == 256 else 1)

    def test_layers_malformed_manifests(self):
        path = reference_tests.LayerTests().fixture(self.directory.name)
        with zipfile.ZipFile(path) as z:
            original = {n: z.read(n) for n in z.namelist()}
        m = json.loads(original['manifest.json'])
        mutations = []
        for field in m:
            bad = copy.deepcopy(m); del bad[field]; mutations.append(bad)
        for field, values in [('layers', [[], {}, [m['layers'][0]]*257]),
                              ('duration_seconds', [0, True, float('inf')]),
                              ('color_encoding', ['display_referred_agx_baked'])]:
            for value in values:
                bad = copy.deepcopy(m); bad[field] = value; mutations.append(bad)
        for field, values in [('name', ['', None, 3, 'steady']), ('keys', [[], {}])]:
            for value in values:
                bad = copy.deepcopy(m); bad['layers'][1][field] = value; mutations.append(bad)
        for field, values in [('weight', [[1, -1, 1], [True, 1, 1], [1, 2], [1, 2, 3, 4], 'abc', [1, float('nan'), 1], [1e300, 1, 1]]),
                              ('interpolation', [None, 'cubic', True]),
                              ('time_seconds', [1, -1, True, float('inf')])]:
            for value in values:
                bad = copy.deepcopy(m); bad['layers'][1]['keys'][0][field] = value; mutations.append(bad)
        for field in ['weight', 'time_seconds', 'interpolation']:
            bad = copy.deepcopy(m); del bad['layers'][1]['keys'][2][field]; mutations.append(bad)
        for time in [0, 2]:
            bad = copy.deepcopy(m); bad['layers'][1]['keys'][1]['time_seconds'] = time; mutations.append(bad)
        for field, values in [('size', [1, 257, 2.0, True]), ('log2_min', [1, 2, float('nan')]),
                              ('log2_max', [float('inf')]), ('layout', ['b_fastest']),
                              ('encoding', ['float16']), ('type', ['other'])]:
            for value in values:
                bad = copy.deepcopy(m); bad['display_transform'][field] = value; mutations.append(bad)
        for field in m['display_transform']:
            bad = copy.deepcopy(m); del bad['display_transform'][field]; mutations.append(bad)
        for i, bad in enumerate(mutations):
            with self.subTest(case=i):
                self.pack({**original, 'manifest.json': json.dumps(bad).encode()})
                self.assertEqual(self.run_probe().returncode, 1)

    def test_layers_bad_payloads_and_crc(self):
        path = reference_tests.LayerTests().fixture(self.directory.name)
        with zipfile.ZipFile(path) as z:
            original = {n: z.read(n) for n in z.namelist()}
        variants = []
        for name in original:
            variants.extend([{**original, name: original[name][:-1]},
                             {**original, name: original[name] + b'X'}])
        for bits in [0x7c00, 0xfc00, 0x7e00]:
            variants.append({**original, 'layers.bin': struct.pack('<H', bits) + original['layers.bin'][2:]})
        for index in [6, 8, 9, 53]:
            base = codec.read_ply(original['base.ply']); base[0, index] = 1
            variants.append({**original, 'base.ply': codec.write_ply(base)})
        variants.extend([{**original, 'deltas.bin': b''},
                         list(original.items()) + [('layers.bin', original['layers.bin'])]])
        for i, e in enumerate(variants):
            with self.subTest(case=i):
                self.pack(e)
                self.assertEqual(self.run_probe().returncode, 1)
        for name in original:
            self.pack(original)
            with zipfile.ZipFile(self.path) as z:
                offset = z.getinfo(name).header_offset
            blob = bytearray(self.path.read_bytes())
            n, extra = struct.unpack_from('<HH', blob, offset + 26)
            blob[offset + 30 + n + extra] ^= 1
            self.path.write_bytes(blob)
            self.assertEqual(self.run_probe().returncode, 1)

    def test_layer_unicode_names(self):
        path = reference_tests.LayerTests().fixture(self.directory.name)
        with zipfile.ZipFile(path) as z:
            original = {n: z.read(n) for n in z.namelist()}
        for names in [['café', '😀'], ['a\nb', 'a\x00b']]:
            m = json.loads(original['manifest.json'])
            for l, name in zip(m['layers'], names):
                l['name'] = name
            self.pack({**original, 'manifest.json': json.dumps(m).encode()})
            r = self.run_probe(None, 'names')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout, b''.join(struct.pack('<I', len(n.encode())) + n.encode() for n in names))
        for first, second in [(b'caf\\u00e9', 'café'.encode()),
                              (b'\\ud83d\\ude00', '😀'.encode()),
                              (b'a\\u0000b', b'a\\u0000b')]:
            text = original['manifest.json'].replace(b'"steady"', b'"' + first + b'"').replace(b'"lamp"', b'"' + second + b'"')
            self.pack({**original, 'manifest.json': text})
            self.assertEqual(self.run_probe().returncode, 1)
        for bad in [b'\\ud800', b'\\udc00', b'\\ud800x', b'\\ud800\\u0061', b'\xff']:
            self.pack({**original, 'manifest.json': original['manifest.json'].replace(b'"lamp"', b'"' + bad + b'"')})
            self.assertEqual(self.run_probe().returncode, 1)


if __name__ == '__main__':
    unittest.main()
