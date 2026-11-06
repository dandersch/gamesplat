"""Native rendered conformance against codec.py; run after ./build.sh.

python tests/test_gsanim_render.py OUTPUT_DIRECTORY
Requires a working GL display (NVIDIA or Mesa), NumPy, Pillow, and the supplied
animation_poc inputs. Keeps final PNGs and comparison.json, removes temporary PLYs.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'animation_poc'))
import codec


def main():
    output = Path(sys.argv[1]).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if '--modes' in sys.argv:
        return layers_render_modes(output)
    if '--layers' in sys.argv:
        return layers_conformance(output)
    source = ROOT / 'animation_poc/tv_loop.gsanim'
    views = ROOT / 'animation_poc/reference_views.json'
    if not source.exists():
        source = ROOT / 'animation_poc_old/tv_loop.gsanim'
        views = ROOT / 'animation_poc_old/reference_views.json'
    results = []
    with tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        linear = scratch / 'linear.gsanim'
        with zipfile.ZipFile(source) as z, zipfile.ZipFile(linear, 'w') as target:
            for name in z.namelist():
                value = z.read(name)
                if name == 'manifest.json':
                    manifest = json.loads(value)
                    manifest['interpolation'] = 'linear'
                    value = json.dumps(manifest).encode()
                target.writestr(name, value)
        mixed = scratch / 'mixed.gsanim'
        with zipfile.ZipFile(source) as z, zipfile.ZipFile(mixed, 'w') as target:
            for name in z.namelist():
                value = z.read(name)
                if name == 'manifest.json':
                    manifest = json.loads(value)
                    manifest['version'] = '0.2'
                    for i, key in enumerate(manifest['keys']):
                        key['interpolation'] = 'linear' if i % 2 == 0 else 'step'
                    value = json.dumps(manifest).encode()
                target.writestr(name, value)
        for mode, path, times in [('step', source, [0, .1, .2, .3]),
                                  ('linear', linear, [.037, .337]),
                                  ('mixed02', mixed, [.037, .137, .337])]:
            asset = codec.load(path)
            for time in times:
                static = scratch / 'reference.ply'
                data = asset[1].copy()
                data[:, codec.SH] = codec.appearance(asset, time)
                static.write_bytes(codec.write_ply(data))
                for view in [0, .37, 2]:
                    name = f'{mode}-{time:g}-view{view:g}'
                    images = []
                    for kind, model in [('animated', path), ('static', static)]:
                        capture = scratch / 'image.ppm'
                        command = ['bash', 'tests/render_gsanim.sh', f'ply={model}',
                                   f'baked_views={views}', 'render_mode=alpha',
                                   f'anim_time={time}', f'view={view}', 'frames=8',
                                   'width=768', 'height=768', f'capture={capture}']
                        if kind == 'animated':
                            command.append('exercise_changes=1')
                        with (output / f'{name}-{kind}.log').open('w') as log:
                            subprocess.run(command, cwd=ROOT, stdout=log, stderr=log,
                                           check=True, timeout=60)
                        image = Image.open(capture)
                        assert image.size == (768, 768)
                        image.save(output / f'{name}-{kind}.png')
                        images.append(np.asarray(image).astype(np.int16))
                    delta = np.abs(images[0]-images[1])
                    result = dict(case=name, max_byte_error=int(delta.max()),
                                  mean_byte_error=float(delta.mean()),
                                  identical_percent=float((delta == 0).mean()*100))
                    results.append(result)
                    print(result, flush=True)
                    # CPU/GPU multiply-add rounding may change a few edge pixels.
                    assert delta.max() <= 2 and delta.mean() < .01, result
                    assert images[0].std() > 10, 'Empty/constant view is not a valid comparison'
    (output / 'comparison.json').write_text(json.dumps(results, indent=2))


def layers_render_modes(output):
    """All modes retain HDR and display-transform the final linear composite."""
    source = ROOT / 'animation_poc/coastal_layers_pilot.gsanim'
    asset = codec.load(source)
    results = []
    with tempfile.TemporaryDirectory() as temporary:
        capture = Path(temporary) / 'capture.ppm'
        raw = Path(temporary) / 'linear.f32'
        for mode in ['alpha', 'stochastic', 'gps']:
            cases = [('on', 'off'), ('off', 'off')]
            if mode == 'stochastic':
                cases.append(('on', 'on'))
            for accumulation, taa in cases:
                name = f'{mode}-{accumulation}' + ('-taa' if taa == 'on' else '')
                command = ['bash', 'tests/render_gsanim.sh', f'ply={source}',
                           'baked_views=animation_poc/layers_validation.json',
                           'fixed_time=2.1', 'view=0', 'frames=64', 'width=640', 'height=360',
                           f'render_mode={mode}', f'gps_accumulation={accumulation}', f'taa={taa}',
                           'exercise_modes=1', f'capture={capture}', f'linear_capture={raw}']
                with (output / f'{name}.log').open('w') as log:
                    subprocess.run(command, cwd=ROOT, stdout=log, stderr=log, check=True, timeout=90)
                image = Image.open(capture)
                image.save(output / f'{name}.png')
                actual = np.asarray(image).astype(float) / 255
                linear = np.fromfile(raw, dtype='f4').reshape(360, 640, 4)[..., :3]
                assert np.isfinite(linear).all() and linear.max() > 1, name
                expected = codec.display_transform(asset, linear)
                error = float(np.max(np.abs(actual - expected)) * 255)
                assert error <= 1.1, (name, error)
                assert actual.std() > .05, 'Empty view is not a valid comparison'
                result = dict(mode=mode, accumulation=accumulation, taa=taa,
                              peak_linear_radiance=float(linear.max()), max_display_error_bytes=error)
                results.append(result)
                print(result, flush=True)
    (output / 'comparison.json').write_text(json.dumps(results, indent=2))


def layers_conformance(output):
    """Compare all supplied 0.3 images and clamp-sensitive display probes.

    python tests/test_gsanim_render.py OUTPUT_DIRECTORY --layers
    OpenCV is used only to preserve the reference PNG's 16-bit RGB precision.
    """
    import cv2
    validation = ROOT / 'animation_poc/layers_validation.json'
    data = json.loads(validation.read_text())
    results = []
    with tempfile.TemporaryDirectory() as temporary:
        capture = Path(temporary) / 'capture.ppm'
        linear_capture = Path(temporary) / 'linear.f32'
        for check in data['checks']:
            view = next(i for i, v in enumerate(data['views']) if v['name'] == check['view'])
            name = Path(check['image']).stem
            command = ['bash', 'tests/render_gsanim.sh',
                       f'ply={validation.parent / data["asset"]}', f'baked_views={validation}',
                       f'anim_time={check["seconds"]}', f'view={view}', 'frames=8',
                       'width=640', 'height=360', 'exercise_changes=1',
                       f'capture={capture}', f'linear_capture={linear_capture}']
            with (output / f'{name}.log').open('w') as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=log, check=True, timeout=60)
            image = Image.open(capture)
            image.save(output / f'{name}.png')
            actual = np.asarray(image).astype(float) / 255
            expected = cv2.imread(str(validation.parent / check['image']), cv2.IMREAD_UNCHANGED)
            assert expected is not None and expected.dtype == np.uint16
            expected = expected[..., ::-1].astype(float) / 65535
            psnr = float(-10 * np.log10(np.mean((actual - expected)**2)))
            probe_error = max(float(np.max(np.abs(actual[p['y'], p['x']] - p['display']))) for p in check['probes'])
            linear = np.fromfile(linear_capture, dtype='f4').reshape(360, 640, 4)[..., :3]
            linear_error = max(float(np.max(np.abs(linear[p['y'], p['x']] - p['linear']))) for p in check['probes'])
            result = dict(view=check['view'], seconds=check['seconds'], psnr_db=psnr,
                          max_probe_error_bytes=probe_error * 255,
                          max_linear_probe_error=linear_error, peak_linear_radiance=float(linear.max()))
            results.append(result)
            print(result, flush=True)
    (output / 'comparison.json').write_text(json.dumps(results, indent=2))
    assert all(r['psnr_db'] >= 40 and r['max_probe_error_bytes'] <= 3 for r in results), results


if __name__ == '__main__':
    main()
