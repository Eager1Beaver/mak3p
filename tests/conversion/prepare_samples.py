"""Verify or download the optional conversion test samples."""

import argparse
import hashlib
import pathlib
import urllib.request


TAGLIB_SAMPLES = 'https://raw.githubusercontent.com/taglib/taglib/v2.3.1/tests/data/'
WAVPACK_SAMPLES = 'https://samples.ffmpeg.org/A-codecs/lossless/wavpack/test_suite/hybrid_bitrates/'
SAMPLES = {
    'mac-399.ape': (TAGLIB_SAMPLES + 'mac-399.ape', '8c851b4f52bbbd822262040ae8961b8c27f8f702d1be3fa50cca283d009aadea'),
    'hybrid.wv': (WAVPACK_SAMPLES + '128kbps.wv', '4930f7b2c136b4af405a313e261439bfae0a6afc90c0e8687480fcd6e726401b'),
    'hybrid.wvc': (WAVPACK_SAMPLES + 'wvc_files/128kbps.wvc', 'a32641ed33b9a0ac2ebc9d7189539820bf956683896a5deabb965a6e6e03012f'),
}


def verify_samples(directory, download=False):
    paths = []
    for name, (url, checksum) in SAMPLES.items():
        path = directory / name
        if not path.exists() and download:
            limit = 8 * 1024 * 1024
            with urllib.request.urlopen(url, timeout=40) as response:
                data = response.read(limit + 1)
            if len(data) > limit or hashlib.sha256(data).hexdigest() != checksum:
                raise RuntimeError(f'Invalid fixture download: {name}')
            path.write_bytes(data)
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise RuntimeError(f'Missing or changed sample {path}. Run prepare_samples.py with --download.')
        paths.append(path)
    return paths


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--download', action='store_true')
    args = parser.parse_args()
    if args.download:
        args.directory.mkdir(parents=True, exist_ok=True)
    for path in verify_samples(args.directory, args.download):
        print(f'{path.name}: SHA-256 verified')
