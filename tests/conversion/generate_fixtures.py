import array
import hashlib
import json
import math
import pathlib
import shutil
import struct
import subprocess
import sys

ffmpeg, ffprobe, destination, sample_directory = sys.argv[1:]
root = pathlib.Path(destination)
root.mkdir(parents=True, exist_ok=True)
if sample_directory:
    prepare_samples = pathlib.Path(__file__).with_name('prepare_samples.py')
    subprocess.run([sys.executable, str(prepare_samples), sample_directory], check=True)
media = []
commands = []


def command(arguments):
    full = [ffmpeg, '-nostdin', '-hide_banner', '-v', 'error', '-y', '-threads', '1', *arguments]
    result = subprocess.run(full, capture_output=True, timeout=40)
    if result.returncode:
        raise RuntimeError(f'{full!r}: {result.stderr.decode(errors="replace")}')
    commands.append(full)
    return result.stdout


def probe(path):
    return json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_format',
                                               '-show_streams', '-of', 'json', str(path)]))


def register(name, family, signal='sine', origin='synthetic FFmpeg'):
    path = root / name
    info = probe(path)
    audio = next(stream for stream in info['streams'] if stream['codec_type'] == 'audio')
    rate = int(audio['sample_rate'])
    family_441 = rate in (11025, 22050, 44100, 88200, 176400, 352800)
    family_480 = rate in (8000, 12000, 16000, 24000, 32000, 48000, 96000, 192000, 384000)
    media.append({'name': name, 'family': family, 'signal': signal, 'origin': origin,
                  'bytes': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                  'codec': audio['codec_name'], 'channels': audio['channels'], 'rate': rate,
                  'output_rate': 44100 if family_441 else 48000 if family_480 else
                      44100 if abs(rate - 44100) < abs(rate - 48000) else 48000,
                  'duration': float(info['format']['duration'])})


def audio(name, family, codec, rate=44100, channels=2, signal='sine', options=(), duration=1.1):
    source = f'sine=frequency=997:sample_rate={rate}' if signal == 'sine' else \
        f'anoisesrc=color={signal}:seed=871:amplitude=0.15:sample_rate={rate}'
    command(['-f', 'lavfi', '-i', source, '-t', str(duration), '-ac', str(channels),
             '-c:a', codec, '-threads:a', '1', *options, str(root / name)])
    register(name, family, signal)


audio('sine441_mono.wav', 'wav', 'pcm_s16le', channels=1)
audio('white192_stereo.wav', 'wav', 'pcm_s24le', 192000, 2, 'white')
audio('pink92_float.wav', 'wav', 'pcm_f32le', 92000, 2, 'pink')
audio('sine8_mono.wav', 'wav', 'pcm_s16le', 8000, 1)
audio('blue16_mono.wav', 'wav', 'pcm_s16le', 16000, 1, 'blue')
audio('pink44_mono.flac', 'flac', 'flac', 44100, 1, 'pink')
audio('white192.flac', 'flac', 'flac', 192000, 2, 'white')
audio('brown92.flac', 'flac', 'flac', 92000, 2, 'brown')
audio('violet96_6ch.flac', 'flac', 'flac', 96000, 6, 'violet')
audio('sine88.wv', 'wv', 'wavpack', 88200, 2)
audio('blue192.wv', 'wv', 'wavpack', 192000, 2, 'blue')
audio('sine96.aiff', 'aiff', 'pcm_s24be', 96000, 2)
audio('pink22.aif', 'aiff', 'pcm_s16be', 22050, 1, 'pink')
audio('pink44_aac.m4a', 'm4a', 'aac', 44100, 2, 'pink', ('-b:a', '256k'))
audio('white192_alac.m4a', 'm4a', 'alac', 192000, 2, 'white')
audio('brown48.ogg', 'ogg', 'libvorbis', 48000, 2, 'brown', ('-q:a', '6'))
audio('pink48.opus', 'opus', 'libopus', 48000, 2, 'pink', ('-b:a', '96k'))
audio('sine22_wma1.wma', 'wma', 'wmav1', 22050, 1, 'sine', ('-b:a', '64k'))
audio('blue44_wma2.wma', 'wma', 'wmav2', 44100, 2, 'blue', ('-b:a', '192k'))
audio('pink48_6ch.ac3', 'ac3', 'ac3', 48000, 6, 'pink', ('-b:a', '448k'))
audio('sine320.mp3', 'mp3', 'libmp3lame', 44100, 2, 'sine', ('-b:a', '320k'))
audio('pink_vbr.mp3', 'mp3', 'libmp3lame', 48000, 2, 'pink', ('-q:a', '4'))
command(['-f', 'lavfi', '-i', 'color=black:s=32x32:r=10', '-f', 'lavfi', '-i',
         'sine=frequency=997:sample_rate=48000', '-t', '1.1', '-map', '0:v', '-map', '1:a',
         '-c:v', 'mpeg4', '-threads:v', '1', '-c:a', 'aac', '-b:a', '256k', str(root / 'video.mp4')])
register('video.mp4', 'mp4')

# PCM fixtures with different tones in each channel expose channel dropping.
command(['-f', 'lavfi', '-i', 'aevalsrc=0.08*sin(2*PI*401*t)|0.08*sin(2*PI*607*t)|'
         '0.08*sin(2*PI*809*t)|0|0.08*sin(2*PI*1013*t)|0.08*sin(2*PI*1201*t):s=48000:c=5.1',
         '-t', '1.1', '-c:a', 'pcm_s24le', str(root / 'tones_6ch.wav')])
register('tones_6ch.wav', 'wav', 'multitone')

# Convert a PCM sine to DSD using first-order pulse-density modulation.
pcm = command(['-f', 'lavfi', '-i', 'sine=frequency=997:sample_rate=176400', '-t', '0.5',
               '-c:a', 'pcm_f32le', '-f', 'f32le', 'pipe:1'])
samples = array.array('f', pcm)
if sys.byteorder != 'little':
    samples.byteswap()
payload = bytearray()
error = 0.0
byte = bit = 0
for sample in samples:
    for _ in range(16):
        error += sample
        positive = error >= 0
        error -= 1 if positive else -1
        byte |= int(positive) << bit
        bit += 1
        if bit == 8:
            payload.append(byte)
            byte = bit = 0
sample_count = len(samples) * 16
block_size = 4096
payload.extend(b'\x69' * (-len(payload) % block_size))
header = struct.pack('<4sQQQ', b'DSD ', 28, 92 + len(payload), 0)
header += struct.pack('<4sQIIIIIIQII', b'fmt ', 52, 1, 0, 1, 1, 2822400, 1, sample_count, block_size, 0)
header += struct.pack('<4sQ', b'data', 12 + len(payload))
(root / 'sine_dsd64.dsf').write_bytes(header + payload)
register('sine_dsd64.dsf', 'dsf')

if sample_directory:
    for name in ('mac-399.ape', 'hybrid.wv', 'hybrid.wvc'):
        shutil.copyfile(pathlib.Path(sample_directory) / name, root / name)
    register('mac-399.ape', 'ape', 'external', 'TagLib 2.3.1 tests/data/mac-399.ape')
    register('hybrid.wv', 'wv', 'external', 'FFmpeg WavPack test_suite/hybrid_bitrates/128kbps.wv')

# Supplemental short files drive policy, failure, metadata, and queue workflows.
for bitrate in (32, 64, 112, 128, 160, 192, 224, 256):
    command(['-f', 'lavfi', '-i', 'sine=frequency=733:sample_rate=44100', '-t', '0.9',
             '-ac', '2', '-c:a', 'libmp3lame', '-b:a', f'{bitrate}k', str(root / f'cbr{bitrate}.mp3')])
command(['-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=mono', '-t', '3', '-c:a', 'flac',
         str(root / 'below_minimum.flac')])
command(['-f', 'lavfi', '-i', 'color=c=blue:s=24x24', '-frames:v', '1', '-threads:v', '1',
         '-c:v', 'png', '-update', '1', str(root / 'cover.png')])
command(['-f', 'lavfi', '-i', 'color=black:s=16x16:r=10', '-t', '0.5', '-an',
         '-c:v', 'mpeg4', '-threads:v', '1', str(root / 'no_audio.mp4')])
command(['-f', 'lavfi', '-i', 'sine=frequency=997:sample_rate=44100', '-t', '1.1',
         '-c:a', 'flac', '-metadata', 'title=Étude 日本語', '-metadata', 'artist=Validation Artist',
         '-metadata', 'album=Test Album', '-metadata', 'track=3/12', '-metadata', 'disc=1/2',
         '-metadata', 'date=2001-02-03', '-metadata', 'composer=Composer',
         '-metadata', 'comment=Preserve this comment', '-metadata', 'PRIVATE_FIELD=omit me',
         str(root / 'metadata.flac')])
command(['-i', str(root / 'metadata.flac'), '-i', str(root / 'cover.png'), '-map', '0:a', '-map', '1:v',
         '-c', 'copy', '-disposition:v', 'attached_pic', str(root / 'artwork.flac')])
command(['-f', 'lavfi', '-i', 'sine=frequency=997:sample_rate=44100', '-t', '1.1', '-c:a', 'libmp3lame',
         '-b:a', '320k', '-metadata', 'title=Original ID3 title', str(root / 'metadata.mp3')])
(root / 'invalid.mp3').write_bytes(b'not audio\x00\xff\n')
(root / 'unsupported.wav').write_bytes(b'RIFF' + (32).to_bytes(4, 'little') + b'WAVEfmt ' + b'\xff' * 32)
truncated = (root / 'white192.flac').read_bytes()[:12000]
(root / 'truncated.flac').write_bytes(truncated)
(root / 'late_corruption.flac').write_bytes((root / 'white192.flac').read_bytes()[:
    (root / 'white192.flac').stat().st_size // 2 + 100])
(root / 'truncated.mp3').write_bytes((root / 'sine320.mp3').read_bytes()[:250])

manifest = {'media': media, 'commands': commands, 'total_matrix_bytes': sum(item['bytes'] for item in media)}
(root / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
print(f'Generated {len(media)} matrix fixtures, {manifest["total_matrix_bytes"]} bytes, under {root}')
