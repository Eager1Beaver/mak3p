import array
import csv
import hashlib
import json
import math
import os
import pathlib
import shutil
import signal
import struct
import subprocess
import sys
import time

scenario, runner, cli, ffmpeg, ffprobe, fixtures_arg, artifacts_arg, audio_fixtures = sys.argv[1:]
fixtures = pathlib.Path(fixtures_arg)
root = pathlib.Path(artifacts_arg) / scenario
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True)
manifest = json.loads((fixtures / 'manifest.json').read_text(encoding='utf-8'))
matrix = manifest['media']


def require(condition, explanation):
    if not condition:
        raise AssertionError(explanation)


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def command(arguments, timeout=90):
    result = subprocess.run([str(value) for value in arguments], capture_output=True, timeout=timeout)
    require(result.returncode == 0, f'{arguments}: {result.stderr.decode(errors="replace")}')
    return result.stdout


def probe(path, *input_options):
    return json.loads(command([ffprobe, '-v', 'error', '-show_format', '-show_streams',
                               '-of', 'json', *input_options, path]))


def audio_stream(path):
    return next(stream for stream in probe(path)['streams'] if stream['codec_type'] == 'audio')


def copy(name, target=None):
    destination = root / (target or name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(fixtures / name, destination)
    return destination


run_index = 0


def run(inputs, **options):
    global run_index
    run_index += 1
    run_dir = root / f'run-{run_index}'
    run_dir.mkdir()
    output = run_dir / 'outputs'
    output.mkdir()
    config = {'application_directory': str(run_dir / 'application'),
              'ffmpeg': ffmpeg, 'ffprobe': ffprobe, 'inputs': [str(value) if not isinstance(value, dict)
                                                            else value for value in inputs],
              'output_directory': str(output), **options}
    if config.get('same_as_source'):
        del config['output_directory']
        del config['same_as_source']
    input_paths = [pathlib.Path(value['path'] if isinstance(value, dict) else value)
                   for value in config['inputs']]
    before = {str(path): digest(path) for path in input_paths if path.is_file()}
    config_path = run_dir / 'request.json'
    result_path = run_dir / 'result.json'
    config_path.write_text(json.dumps(config, indent=2, ensure_ascii=False), encoding='utf-8')
    command([runner, config_path, result_path], timeout=170)
    result = json.loads(result_path.read_text(encoding='utf-8'))
    require(not result['warnings'], f'Unexpected batch warnings: {result["warnings"]}')
    require(result['progress_monotonic'] and result['eta_valid'], 'Invalid progress or ETA observed')
    require(result['remaining_ffmpeg_children'] == 0, 'Child process survived completion')
    for name, checksum in before.items():
        require(digest(name) == checksum, f'Source changed: {name}')
    for temporary in root.rglob('*.partial.mp3'):
        raise AssertionError(f'Temporary file leaked: {temporary}')
    return result


def completed(result, count):
    require(result['counts']['completed'] == count and result['counts']['failed'] == 0 and
            result['counts']['cancelled'] == 0,
            f'Unexpected results: {result["counts"]}; jobs={result["jobs"]}')


def output_properties(job, bitrate=None, channels=None, rate=None, duration=None):
    path = pathlib.Path(job['output'])
    info = probe(path)
    streams = info['streams']
    require(len(streams) == 1 and streams[0]['codec_name'] == 'mp3', f'Unexpected streams: {streams}')
    stream = streams[0]
    bitrate = bitrate or job['effective_bitrate']
    require(int(stream['bit_rate']) == bitrate * 1000, f'Wrong output bitrate: {stream}')
    if channels:
        require(stream['channels'] == channels, f'Wrong channel count: {stream}')
    if rate:
        require(int(stream['sample_rate']) == rate, f'Wrong sample rate: {stream}')
    if duration:
        require(abs(float(info['format']['duration']) - duration) < 0.18,
                f'Duration differs: {info["format"]["duration"]} vs {duration}')
    packets = json.loads(command([ffprobe, '-v', 'error', '-select_streams', 'a:0', '-show_packets',
                                  '-show_entries', 'packet=size', '-of', 'json', path]))['packets']
    frame_size = 144000 * bitrate // int(stream['sample_rate'])
    require(packets and all(int(packet['size']) in (frame_size, frame_size + 1) for packet in packets),
            'Packet sizes do not independently match the expected constant bitrate')
    command([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-i', path, '-f', 'null', '-'])
    require(job['progress'] == 1, 'Published output has incomplete progress')


def pcm(path, channels=1, rate=44100):
    values = array.array('f', command([ffmpeg, '-nostdin', '-v', 'error', '-i', path,
                                      '-map', '0:a:0', '-ac', str(channels), '-ar', str(rate),
                                      '-f', 'f32le', '-']))
    if sys.byteorder != 'little':
        values.byteswap()
    require(values and all(math.isfinite(value) for value in values), 'Nonfinite or empty decoded signal')
    return values


def matrix_conversion(mode):
    result = run([fixtures / entry['name'] for entry in matrix], channels=mode, force=True)
    completed(result, len(matrix))
    entries = {entry['name']: entry for entry in matrix}
    for job in result['jobs']:
        entry = entries[pathlib.Path(job['source']).name]
        require(job['state'] == 'completed', f'Matrix job did not complete: {job}')
        output_properties(job, 192, 1 if mode == 'mono' else 2, entry['output_rate'], entry['duration'])
        samples = pcm(job['output'])
        require(max(abs(value) for value in samples) > 0.00001, f'Silent output for {entry["name"]}')
    (root / 'summary.json').write_text(json.dumps({'conversions': len(matrix), 'mode': mode,
                                                 'input_bytes': manifest['total_matrix_bytes']}, indent=2))


def mixed_batch():
    families = {}
    for entry in matrix:
        families.setdefault(entry['family'], fixtures / entry['name'])
    result = run(list(families.values()), force=True, channels='stereo', workers=3)
    completed(result, len(families))
    require(set(families) >= {'wav', 'flac', 'wv', 'm4a', 'mp4', 'dsf', 'ac3', 'aiff',
                              'ogg', 'opus', 'wma', 'mp3'}, 'Missing a required priority format')
    for job in result['jobs']:
        output_properties(job, 192, 2)


def corrupted_inputs():
    inputs = [fixtures / name for name in ('invalid.mp3', 'unsupported.wav', 'truncated.mp3',
                                          'truncated.flac', 'no_audio.mp4')]
    inputs += [fixtures / 'does-not-exist.flac', fixtures / 'sine441_mono.wav']
    result = run(inputs, channels='mono')
    completed(result, 1)
    require(result['counts']['skipped'] == 6, f'Corrupt/unsupported input unexpectedly accepted: {result}')
    require(all(job['diagnostic']['key'] for job in result['jobs']), 'Input rejection lacks a reason')


def bitrate_policy():
    source = fixtures / 'sine320.mp3'
    for target in (128, 160, 192, 224, 256):
        result = run([source], bitrate=target, force=False)
        completed(result, 1)
        output_properties(result['jobs'][0], target)
    for bitrate in (32, 64, 112, 128, 160, 192, 224, 256):
        source = fixtures / f'cbr{bitrate}.mp3'
        result = run([source], bitrate=256, force=False)
        if bitrate == 256:
            require(result['counts']['skipped'] == 1 and result['jobs'][0]['state'] == 'same_bitrate',
                    'An exact requested CBR match did not skip')
        else:
            completed(result, 1)
            output_properties(result['jobs'][0], bitrate)
        forced = run([source], bitrate=256, force=True, channels='mono')
        completed(forced, 1)
        output_properties(forced['jobs'][0], 256, 1)
    result = run([fixtures / 'below_minimum.flac'], force=False)
    require(result['jobs'][0]['state'] == 'below_minimum', 'Below-minimum source was increased')
    forced = run([fixtures / 'below_minimum.flac'], force=True)
    completed(forced, 1)
    output_properties(forced['jobs'][0], 192)
    result = run([fixtures / 'pink_vbr.mp3'], bitrate=192, force=False)
    completed(result, 1)
    source_bitrate = int(audio_stream(fixtures / 'pink_vbr.mp3')['bit_rate'])
    require(result['jobs'][0]['effective_bitrate'] * 1000 <= source_bitrate,
            'VBR source was increased without force')
    output_properties(result['jobs'][0])
    lossless = fixtures / 'metadata.flac'
    source_bitrate = int(probe(lossless)['format']['bit_rate'])
    require(source_bitrate < 192000, 'The lossless matching fixture no longer has a lower bitrate')
    result = run([lossless], bitrate=192, force=False)
    completed(result, 1)
    expected = max(rate for rate in (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192)
                   if rate * 1000 <= source_bitrate)
    output_properties(result['jobs'][0], expected)


def realtime_engine():
    if os.name == 'nt':
        wrapper = root / 'ffmpeg-realtime.exe'
        shutil.copy2(runner, wrapper)
        wrapper.with_suffix('.path').write_text(ffmpeg, encoding='utf-8')
        return str(wrapper)
    wrapper = root / 'ffmpeg-realtime'
    wrapper.write_text('#!/usr/bin/env python3\nimport os,sys\na=sys.argv[1:]\n'
                       'if "-progress" in a and not any("fast" in x for x in a):\n'
                       ' a.insert(a.index("-i"),"-re")\n'
                       f'os.execv({ffmpeg!r},[{ffmpeg!r},*a])\n')
    wrapper.chmod(0o755)
    return str(wrapper)


def parallel_workers():
    sources = [copy('sine441_mono.wav', f'track-{index}.wav') for index in range(6)]
    result = run(sources, workers=2, ffmpeg=realtime_engine())
    completed(result, 6)
    require(result['peak_workers'] == 2 and result['peak_converting_rows'] == 2 and
            result['peak_ffmpeg_children'] == 2, f'Actual process concurrency was not bounded: {result}')
    require(result['saw_progress'] and result['saw_eta'], 'Real progress/ETA were not observed')


def hundred_files():
    sources = [copy('sine441_mono.wav', f'inputs/group-{index % 5}/track-{index // 5}.wav')
               for index in range(100)]
    total = sum(path.stat().st_size for path in sources)
    require(total < 100_000_000, 'Queue test exceeds the requested input-size budget')
    result = run([*sources, sources[0], sources[5]], workers=4, remove_indices=[10], readd=[str(sources[10])])
    completed(result, 100)
    require(result['counts']['total'] == 100 and result['duplicates'] == 2 and result['removed'] == 1,
            'Large queue lost duplicate/removal invariants')
    outputs = [job['output'] for job in result['jobs']]
    require(len(set(outputs)) == 100 and result['peak_workers'] <= 4, 'Large batch collided or exceeded its bound')
    for job in result['jobs']:
        output_properties(job, 192)
    (root / 'summary.json').write_text(json.dumps({'files': 100, 'input_bytes': total,
        'output_bytes': sum(pathlib.Path(path).stat().st_size for path in outputs),
        'elapsed_seconds': result['elapsed_seconds'], 'peak_workers': result['peak_workers'],
        'peak_ffmpeg_children': result['peak_ffmpeg_children'],
        'max_snapshot_seconds': result['max_snapshot_seconds']}, indent=2))


def destinations():
    left = copy('sine441_mono.wav', 'left/音楽 $(literal);.wav')
    right = copy('sine441_mono.wav', 'right/音楽 $(literal);.wav')
    existing = left.with_suffix('.mp3')
    existing.write_bytes(b'pre-existing output')
    previous = digest(existing)
    result = run([left, right], same_as_source=True)
    completed(result, 2)
    require(digest(existing) == previous, 'A pre-existing output changed')
    require({pathlib.Path(job['output']).parent for job in result['jobs']} == {left.parent, right.parent},
            'Source-local output did not follow each input directory')
    shared = root / 'shared'
    shared.mkdir()
    result = run([left, right], output_directory=str(shared), persist=True)
    completed(result, 2)
    require(len(set(job['output'] for job in result['jobs'])) == 2 and
            all(pathlib.Path(job['output']).parent == shared for job in result['jobs']), 'Custom folder flattening failed')
    require(result['persisted']['custom'] == str(shared), 'Custom destination did not persist')
    result = run([left, right], output_directory=str(root / 'missing'))
    require(result['counts']['failed'] == 2 and not result['counts']['completed'], 'Missing destination did not fail safely')
    readonly = root / 'readonly'
    readonly.mkdir()
    if os.name == 'nt':
        user = command(['whoami.exe', '/user', '/fo', 'csv', '/nh']).decode(errors='replace')
        sid = next(csv.reader([user.strip()]))[1]
        command(['icacls.exe', readonly, '/deny', f'*{sid}:(WD,AD)'])
    else:
        readonly.chmod(0o500)
    try:
        result = run([left], output_directory=str(readonly))
        require(result['counts']['failed'] == 1, 'Read-only destination did not fail safely')
    finally:
        if os.name == 'nt':
            command(['icacls.exe', readonly, '/remove:d', f'*{sid}'])
        else:
            readonly.chmod(0o700)
    long_source = copy('sine441_mono.wav', 'long/' + 'é' * 110 + '.wav')
    result = run([long_source], same_as_source=True)
    completed(result, 1)
    require(len(pathlib.Path(result['jobs'][0]['output']).name.encode()) <= 255, 'Output exceeded filename byte limit')
    late = root / 'late'
    late.mkdir()
    conflicting = late / left.with_suffix('.mp3').name
    result = run([left], output_directory=str(late), ffmpeg=realtime_engine(), late_conflict=str(conflicting))
    completed(result, 1)
    require(result['late_conflict_created'] and conflicting.read_bytes() == b'late external output' and
            pathlib.Path(result['jobs'][0]['output']) != conflicting, 'Late publication collision overwrote another file')


def magnitude(values, frequency, rate):
    start, count = rate // 10, min(len(values) - rate // 10, rate // 3)
    real = imaginary = 0.0
    for index in range(count):
        phase = 2 * math.pi * frequency * index / rate
        value = values[start + index]
        real += value * math.cos(phase)
        imaginary += value * math.sin(phase)
    return 2 * math.hypot(real, imaginary) / count


def signal_quality():
    result = run([fixtures / 'sine441_mono.wav', fixtures / 'sine96.aiff', fixtures / 'sine_dsd64.dsf'],
                 channels='stereo')
    completed(result, 3)
    measurements = []
    for job in result['jobs']:
        values = pcm(job['output'], channels=2)
        left, right = values[::2], values[1::2]
        require(max(abs(a - b) for a, b in zip(left, right)) < 0.001, 'Mono-to-stereo introduced channel differences')
        tone = magnitude(left, 997, 44100)
        off_tone = magnitude(left, 1600, 44100)
        require(tone > 0.02 and tone > off_tone * 10, f'Sine frequency/resampling damaged: {tone}, {off_tone}')
        measurements.append({'source': job['source'], 'tone_amplitude': tone, 'off_tone_amplitude': off_tone})
    result = run([fixtures / 'tones_6ch.wav'], channels='mono')
    completed(result, 1)
    values = pcm(result['jobs'][0]['output'])
    tones = {frequency: magnitude(values, frequency, 44100) for frequency in (401, 607, 809, 1013, 1201)}
    require(min(tones.values()) > 0.005, f'Downmix dropped a non-LFE channel: {tones}')
    (root / 'signal_measurements.json').write_text(json.dumps({'sines': measurements, 'downmix': tones}, indent=2))


def metadata():
    result = run([fixtures / 'metadata.flac', fixtures / 'artwork.flac'], channels='mono')
    completed(result, 2)
    expected = {'TITLE': ['Étude 日本語'], 'ARTIST': ['Validation Artist'], 'ALBUM': ['Test Album'],
                'TRACKNUMBER': ['3/12'], 'DISCNUMBER': ['1/2'], 'DATE': ['2001-02-03'],
                'COMPOSER': ['Composer'], 'COMMENT': ['Preserve this comment']}
    for job in result['jobs']:
        text = job['metadata']['text']
        require(all(text.get(key) == value for key, value in expected.items()), f'Metadata mismatch: {text}')
        require('PRIVATE_FIELD' not in text and any('PRIVATE_FIELD' in warning['message'] for warning in job['warnings']),
                'Unsupported metadata was copied or omitted without a warning')
        if pathlib.Path(job['source']).name == 'artwork.flac':
            pictures = job['metadata']['pictures']
            require(len(pictures) == 1 and bytes.fromhex(pictures[0]['hex']) == (fixtures / 'cover.png').read_bytes(),
                    'Artwork bytes were not preserved')
    report = pathlib.Path(result['log']).read_text(encoding='utf-8')
    require('PRIVATE_FIELD' in report, 'Metadata omission was missing from the log')


def malformed_metadata():
    source = copy('metadata.mp3', 'damaged-id3.mp3')
    data = bytearray(source.read_bytes())
    title = data.find(b'TIT2')
    require(title >= 0, 'Cannot locate synthetic ID3 title')
    data[title + 4:title + 8] = b'\x7f\x7f\x7f\x7f'
    source.write_bytes(data)
    invalid = root / 'invalid-id3.mp3'
    invalid.write_bytes(b'ID3\x04\x00\x00\x7f\x7f\x7f\x7f' + b'bad' * 20)
    result = run([source, invalid, fixtures / 'sine441_mono.wav'])
    require(result['counts']['completed'] == 2 and result['counts']['skipped'] == 1,
            f'Malformed metadata did not isolate failures: {result}')
    for job in result['jobs']:
        if 'output' in job:
            output_properties(job, 192)


def cancellation():
    sources = [copy('sine441_mono.wav', 'fast.wav')]
    sources += [copy('sine441_mono.wav', f'slow-{index}.wav') for index in range(8)]
    result = run(sources, workers=2, ffmpeg=realtime_engine(), cancel_after_completed=1, cancel_when_active=2)
    require(result['cancel_requested'] and result['counts']['completed'] >= 1 and
            result['counts']['cancelled'] >= 1 and result['counts']['failed'] == 0,
            f'Cancellation did not retain completions/isolate unfinished jobs: {result}')
    require(result['cancel_call_seconds'] < 0.1, 'Cancel request blocked the caller')
    require(result['counts']['completed'] + result['counts']['cancelled'] == 9, 'Cancellation lost a queued entry')
    for job in result['jobs']:
        if 'output' in job:
            output_properties(job, 192)


def cli_workflow():
    application = root / 'application'
    application.mkdir()
    executable = application / pathlib.Path(cli).name
    shutil.copy2(cli, executable)
    input_file = copy('sine441_mono.wav')
    output_dir = root / 'relative-output'
    output_dir.mkdir()
    ini = root / 'selected.ini'
    ini.write_text('[conversion]\nbitrate_kbps=192\nchannel_mode=mono\noutput_mode=custom_directory\n'
                   'custom_output_directory=relative-output\nforce_upsampling=true\n[processing]\nworkers=1\n[logging]\nenabled=true\n')
    before = digest(input_file)
    text = command([executable, 'convert', input_file, '--config', ini, '--save-settings']).decode()
    require('Converted: 1' in text and (output_dir / input_file.with_suffix('.mp3').name).is_file(),
            'CLI INI/relative-output workflow failed')
    require(digest(input_file) == before and (application / 'config.ini').exists() and
            len(list(application.glob('Mak3P_*.log'))) == 1, 'CLI persistence/log/source protection failed')
    text = command([executable, 'convert', input_file]).decode()
    require('Converted: 1' in text and len(list(output_dir.glob('*.mp3'))) == 2 and
            len(list(application.glob('Mak3P_*.log'))) == 2, 'Repeated CLI conversion overwrote outputs/logs')
    saved = application / 'config.ini'
    settings = saved.read_text()
    saved.write_text(settings.replace('output_mode=custom_directory', 'output_mode=same_as_source'))
    command([executable, 'convert', input_file])
    require(input_file.with_suffix('.mp3').is_file() and 'relative-output' in saved.read_text(),
            'Switching to source output forgot the inactive custom folder')
    saved.write_text(settings)
    command([executable, 'convert', input_file])
    require(len(list(output_dir.glob('*.mp3'))) == 3, 'Switching back to the custom output folder failed')


def wavpack_hybrid():
    source = copy('hybrid.wv')
    correction = copy('hybrid.wvc')
    original = digest(correction)
    with_sidecar = run([source], force=True)
    completed(with_sidecar, 1)
    pcm_with = command([ffmpeg, '-nostdin', '-v', 'error', '-i', source, '-f', 'hash', '-hash', 'sha256', '-'])
    correction.rename(root / 'detached.wvc')
    without_sidecar = run([source], force=True)
    completed(without_sidecar, 1)
    pcm_without = command([ffmpeg, '-nostdin', '-v', 'error', '-i', source, '-f', 'hash', '-hash', 'sha256', '-'])
    require(digest(root / 'detached.wvc') == original and pcm_with == pcm_without,
            'Sidecar changed or decoder behavior differed from the documented native FFmpeg behavior')
    (root / 'hybrid_evidence.json').write_text(json.dumps({
        'decoded_pcm_with_sidecar': pcm_with.decode().strip(),
        'decoded_pcm_without_sidecar': pcm_without.decode().strip(),
        'correction_file_sha256': original}, indent=2))
    warnings = [warning for job in with_sidecar['jobs'] for warning in job['warnings']]
    require(any(warning['key'] == 'wavpack_hybrid' for warning in warnings),
            'Hybrid WavPack correction-file limitation was not disclosed')


def folder_import():
    folder = root / 'inputs'
    shutil.copytree(pathlib.Path(audio_fixtures) / 'Media folder', folder)
    inputs = sorted(folder.iterdir())
    original = {path: digest(path) for path in inputs}
    sources = {path.stem: path for path in inputs if path.name.startswith('track-') or path.name == 'video.mp4'}
    inspected = {name: probe(path, *(['-f', 'mp3'] if path.suffix == '.jpg' else []))
                 for name, path in sources.items()}
    settings = root / 'settings.ini'
    settings.write_text('', encoding='utf-8')

    def convert(destination, bitrate, *options):
        destination.mkdir(exist_ok=True)
        result = subprocess.run([str(value) for value in [
            cli, 'batch', '--config', settings, '--ffmpeg', ffmpeg, '--ffprobe', ffprobe,
            '--output-dir', destination, '--workers', 2, '--bitrate', bitrate,
            *options, '--folder-import', '--', *inputs,
        ]], capture_output=True, timeout=90)
        require(result.returncode == 0, f'Folder conversion failed: {result.stderr!r}; {result.stdout!r}')
        require(b'TagLib:' not in result.stderr, f'Unexpected metadata diagnostics: {result.stderr!r}')
        return {path.stem: path for path in destination.glob('*.mp3')}

    def check(name, output, target, channels, force):
        before = inspected[name]
        source_rate = next(int(stream['bit_rate']) for stream in before['streams'] if stream['codec_type'] == 'audio')
        bitrate = target if force else max(rate for rate in (32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
                                          if rate <= target and rate * 1000 <= source_rate)
        after = probe(output)
        streams = [stream for stream in after['streams'] if stream['codec_type'] == 'audio']
        require(len(streams) == 1 and streams[0]['codec_name'] == 'mp3' and
                int(streams[0]['bit_rate']) == bitrate * 1000 and streams[0]['channels'] == channels and
                streams[0]['sample_rate'] == '44100', f'Wrong audio parameters: {output}')
        require(abs(float(before['format']['duration']) - float(after['format']['duration'])) < 0.15,
                f'Duration changed: {output}')
        old_tags = before['format'].get('tags', {})
        new_tags = after['format'].get('tags', {})
        require(all(new_tags.get(key) == old_tags.get(key) for key in ('title', 'artist', 'album', 'comment')),
                f'Metadata changed: {output}')
        picture = [stream['index'] for stream in after['streams'] if stream['disposition']['attached_pic']]
        require(len(picture) == int(name == 'track-c'), f'Unexpected artwork streams: {output}')
        if picture:
            artwork = ['-c', 'copy', '-f', 'image2pipe', '-']
            expected = command([ffmpeg, '-v', 'error', '-f', 'mp3', '-i', sources[name], '-map', '0:v:0', *artwork])
            actual = command([ffmpeg, '-v', 'error', '-i', output, '-map', f'0:{picture[0]}', *artwork])
            require(actual == expected, f'Artwork changed: {output}')
        command([ffmpeg, '-v', 'error', '-xerror', '-i', output, '-map', '0:a:0', '-f', 'null', '-'])

    cases = [
        ('excluded', 320, 2, (), {'video'}),
        ('included', 320, 2, ('--include-mp3',), set(sources)),
        ('matching', 160, 2, ('--include-mp3',), {'track-d', 'track-e', 'video'}),
        ('mono', 128, 1, ('--include-mp3', '--channels', 'mono'), set(sources)),
        ('forced', 320, 2, ('--include-mp3', '--force-upsampling'), set(sources)),
    ]
    for case, target, channels, options, expected in cases:
        outputs = convert(root / case, target, *options)
        require(set(outputs) == expected, f'Unexpected {case} outputs: {list(outputs)}')
        for name, path in outputs.items():
            check(name, path, target, channels, '--force-upsampling' in options)

    retained = {path: digest(path) for path in (root / 'included').glob('*.mp3')}
    repeated = convert(root / 'included', 320, '--include-mp3')
    require(set(repeated) == set(sources) | {f'{name} (1)' for name in sources}, 'Repeat did not use collision suffixes')
    for name in sources:
        check(name, repeated[f'{name} (1)'], 320, 2, False)
    require(all(digest(path) == checksum for path, checksum in retained.items()), 'Repeat changed an existing output')
    require(all(digest(path) == checksum for path, checksum in original.items()), 'Folder inputs changed')
    require(not list(root.rglob('*.partial.mp3')), 'Folder conversion left a temporary file')


def encoding_failure():
    result = run([fixtures / 'late_corruption.flac', fixtures / 'sine441_mono.wav'])
    require(result['counts']['completed'] == 1 and result['counts']['failed'] == 1 and
            result['counts']['skipped'] == 0, f'Midstream decode failure was misclassified or aborted the batch: {result}')
    failure = next(job for job in result['jobs'] if job['state'] == 'failed')
    require(failure['diagnostic']['detail'] and failure['diagnostic']['key'] in ('encode_failed', 'output_invalid'),
            'Decode/encode failure lacked useful technical diagnostics')
    report = pathlib.Path(result['log']).read_text(encoding='utf-8')
    require(failure['diagnostic']['message'] in report, 'Failure missing from the log')
    require(len(list((root / 'run-1/outputs').glob('*.mp3'))) == 1,
            'A failed job published an extra output')


def cli_cancellation():
    application = root / 'application'
    application.mkdir()
    executable = application / pathlib.Path(cli).name
    shutil.copy2(cli, executable)
    sources = [copy('sine441_mono.wav', f'slow-{index}.wav') for index in range(4)]
    output = root / 'outputs'
    output.mkdir()
    arguments = [str(executable), 'batch', *map(str, sources), '--ffmpeg', realtime_engine(),
                 '--ffprobe', ffprobe, '--output-dir', str(output), '--workers', '2', '--log']
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == 'nt' else 0
    process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=creationflags)
    try:
        deadline = time.monotonic() + 10
        while not list(output.glob('*.partial.mp3')):
            require(process.poll() is None and time.monotonic() < deadline, 'CLI did not begin conversion')
            time.sleep(0.01)
        time.sleep(0.1)
        process.send_signal(signal.CTRL_BREAK_EVENT if os.name == 'nt' else signal.SIGINT)
        stdout, stderr = process.communicate(timeout=10)
        require(process.returncode == 130 and b'cancelled: 4' in stdout,
                f'CLI console cancellation failed: {process.returncode}, {stdout}, {stderr}')
        require(not list(output.iterdir()), 'CLI cancellation left partial or incomplete outputs')
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


functions = {'matrix_mono': lambda: matrix_conversion('mono'), 'matrix_stereo': lambda: matrix_conversion('stereo'),
             'mixed_batch': mixed_batch, 'corrupted_inputs': corrupted_inputs, 'bitrate_policy': bitrate_policy,
             'parallel_workers': parallel_workers, 'hundred_files': hundred_files, 'destinations': destinations,
             'signal_quality': signal_quality, 'metadata': metadata, 'malformed_metadata': malformed_metadata,
             'cancellation': cancellation, 'cli_workflow': cli_workflow, 'cli_cancellation': cli_cancellation,
             'encoding_failure': encoding_failure, 'wavpack_hybrid': wavpack_hybrid,
             'folder_import': folder_import}
try:
    started = time.monotonic()
    functions[scenario]()
    elapsed = time.monotonic() - started
    (root / 'outcome.json').write_text(json.dumps({'scenario': scenario, 'passed': True, 'elapsed_seconds': elapsed}, indent=2))
    print(f'PASS {scenario} ({elapsed:.2f}s); artifacts: {root}')
except Exception as error:
    (root / 'outcome.json').write_text(json.dumps({'scenario': scenario, 'passed': False, 'error': str(error)}, indent=2))
    raise
