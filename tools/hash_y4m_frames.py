#!/usr/bin/env python3
"""Stream SHA-256 of every YUV4MPEG2 (8-bit 4:4:4) video frame.

This avoids storing a multi-gigabyte Y4M file when passed a FIFO (or '-'
for stdin). Hashes refer to decoded raw YUV planes, not the frame headers.

Usage:
  python3 tools/hash_y4m_frames.py frames.y4m frames.sha --frames 1800
  python3 tools/hash_y4m_frames.py frames.y4m frames.sha --compare baseline.sha
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('y4m', help='Y4M file, named pipe, or - for stdin')
    cli.add_argument('output', type=Path)
    cli.add_argument('--frames', type=int, help='require this many frames')
    cli.add_argument('--compare', type=Path, help='file with previous one-hash-per-frame baseline')
    args = cli.parse_args()
    if args.frames is not None and args.frames < 1:
        cli.error('--frames must be positive')

    stream = sys.stdin.buffer if args.y4m == '-' else open(args.y4m, 'rb')
    hashes = []
    try:
        header = stream.readline()
        if not header.startswith(b'YUV4MPEG2 '):
            raise ValueError('not a YUV4MPEG2 video')
        tags = header.split()
        width = next((int(t[1:]) for t in tags if re.fullmatch(rb'W\d+', t)), None)
        height = next((int(t[1:]) for t in tags if re.fullmatch(rb'H\d+', t)), None)
        chroma = next((t for t in tags if t.startswith(b'C')), b'C420')
        if not width or not height or chroma not in (b'C444', b'C444jpeg'):
            raise ValueError('only 8-bit Y4M 4:4:4 is supported')
        frame_size = width * height * 3
        while args.frames is None or len(hashes) < args.frames:
            frame_tag = stream.readline()
            if not frame_tag:
                break
            if not frame_tag.startswith(b'FRAME'):
                raise ValueError(f'invalid frame header at {len(hashes)}')
            data = stream.read(frame_size)
            if len(data) != frame_size:
                raise ValueError(f'truncated frame {len(hashes)}')
            hashes.append(hashlib.sha256(data).hexdigest())
        if args.frames is not None and len(hashes) != args.frames:
            raise ValueError(f'expected {args.frames}, saw {len(hashes)} frames')
    finally:
        if args.y4m != '-':
            stream.close()
    args.output.write_text('\n'.join(hashes) + '\n')
    print(f'{len(hashes)} frames fingerprinted')
    if args.compare:
        previous = args.compare.read_text().splitlines()
        if len(previous) != len(hashes):
            raise ValueError(f'baseline has {len(previous)} frames, new recording {len(hashes)}')
        changed = [i for i, (a, b) in enumerate(zip(previous, hashes)) if a != b]
        print(f'{len(changed)} different frames',
              f'(first {min(changed)}, last {max(changed)})' if changed else '')


if __name__ == '__main__':
    main()
