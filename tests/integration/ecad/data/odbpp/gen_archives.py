#!/usr/bin/env python3
"""Generate owned ODB++ acceptance inputs, including corrupt binary cases."""
import gzip
import io
import shutil
import struct
import sys
import tarfile
import zipfile
import zlib
from pathlib import Path


def tar_bytes(files, fmt=tarfile.USTAR_FORMAT):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=fmt) as archive:
        for name, data in files:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return out.getvalue()


def gzip_member(data):
    header = b'\x1f\x8b\x08\x1e' + b'\0' * 4 + b'\0\xff'
    header += struct.pack('<H', 4) + b'ABCD' + b'part.tar\0' + b'acceptance\0'
    header += struct.pack('<H', zlib.crc32(header) & 0xffff)
    compressor = zlib.compressobj(wbits=-15)
    return header + compressor.compress(data) + compressor.flush() + struct.pack('<II', zlib.crc32(data), len(data))


def lzw_literals(data):
    # Block-mode .Z: clear before the dictionary reaches the 9-bit boundary.
    # A clear pads to the next group of eight codes, as required by compress.
    out = bytearray(b'\x1f\x9d\x90')
    for offset in range(0, len(data), 200):
        chunk = data[offset:offset + 200]
        codes = list(chunk)
        more = offset + len(chunk) < len(data)
        if more:
            codes.append(256)
        bits = sum(code << (9 * index) for index, code in enumerate(codes))
        count = ((len(codes) + 7) // 8) * 9 if more else (len(codes) * 9 + 7) // 8
        out.extend(bits.to_bytes(count, 'little'))
    return bytes(out)



def main(root, fixture):
    root.mkdir(parents=True, exist_ok=True)
    files = [(p.relative_to(fixture).as_posix(), p.read_bytes()) for p in sorted(fixture.rglob('*')) if p.is_file()]
    shutil.copytree(fixture, root / 'job')
    tar = tar_bytes(files)
    (root / 'job.tar').write_bytes(tar)
    (root / 'job.tgz').write_bytes(gzip.compress(tar))
    (root / 'JOB.TAR.GZ').write_bytes(gzip.compress(tar))
    for method, name in [(zipfile.ZIP_STORED, 'stored.zip'), (zipfile.ZIP_DEFLATED, 'job.zip')]:
        with zipfile.ZipFile(root / name, 'w', compression=method) as archive:
            for path, data in files:
                archive.writestr(path, data)
    # Stored deflate blocks keep these line-limit fixtures below the ratio limit.
    for suffix, payload in [('boundary', b'#' * (1024 * 1024)),
                            ('overflow', b'#' * (1024 * 1024 + 1)),
                            ('overflow-newline', b'#' * (1024 * 1024 + 1) + b'\n')]:
        entry = [('matrix/matrix', payload)]
        (root / ('line-' + suffix + '.tgz')).write_bytes(
            gzip.compress(tar_bytes(entry), compresslevel=0))
        with zipfile.ZipFile(root / ('line-' + suffix + '.zip'), 'w',
                             compression=zipfile.ZIP_STORED) as archive:
            archive.writestr('matrix/matrix', payload)
    long_path = '/'.join(['directory' * 10] * 3) + '/features'
    format_files = [('matrix/matrix', b'UNITS=MM\n'), (long_path, b'long path payload\0\xff')]
    # Ustar permits at most 155 prefix + 100 basename bytes. A longer complete path
    # cannot be represented; use its maximum legal split, and GNU/pax for >255.
    legal = 'p' * 150 + '/' + 'f' * 99
    for fmt, name in [(tarfile.USTAR_FORMAT, 'ustar'), (tarfile.GNU_FORMAT, 'gnu'), (tarfile.PAX_FORMAT, 'pax')]:
        (root / (name + '.tgz')).write_bytes(gzip.compress(tar_bytes([('matrix/matrix', b'UNITS=MM\n'), (legal, b'same bytes')], fmt)))
    for fmt, name in [(tarfile.GNU_FORMAT, 'gnu-long'), (tarfile.PAX_FORMAT, 'pax-long')]:
        (root / (name + '.tgz')).write_bytes(gzip.compress(tar_bytes(format_files, fmt)))
    shutil.copyfile(root / 'job.zip', root / 'JOB.ZIP')
    with zipfile.ZipFile(root / 'unsupported.zip', 'w', compression=zipfile.ZIP_BZIP2) as archive:
        archive.writestr('matrix/matrix', b'unsupported method')
    with zipfile.ZipFile(root / 'zip64.zip', 'w') as archive:
        with archive.open('matrix/matrix', 'w', force_zip64=True) as entry:
            entry.write(b'local zip64 header')
    # Exercise pax size overriding the following ordinary header's size.
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.PAX_FORMAT) as archive:
        info = tarfile.TarInfo('original')
        info.size = 4
        info.pax_headers = {'path': 'matrix/matrix', 'size': '4', 'ignored': 'value'}
        archive.addfile(info, io.BytesIO(b'pax!'))
    data = bytearray(out.getvalue())
    # The ordinary header follows the pax header and its single data block.
    data[1024 + 124:1024 + 136] = b'00000000001\0'
    data[1024 + 148:1024 + 156] = b'        '
    checksum = sum(data[1024:1536])
    data[1024 + 148:1024 + 156] = ('%06o\0 ' % checksum).encode()
    (root / 'pax-size.tgz').write_bytes(gzip.compress(data))
    cut = len(tar) // 2
    multi = gzip_member(tar[:cut]) + gzip_member(tar[cut:])
    (root / 'multi.tgz').write_bytes(multi)
    bad = bytearray(multi)
    bad[-8] ^= 1
    (root / 'crc.tgz').write_bytes(bad)
    (root / 'job.tar.Z').write_bytes(lzw_literals(tar))
    bad_header = bytearray(multi)
    # Header ends after the optional strings and two header-CRC bytes.
    header_end = multi.index(b'acceptance\0') + len(b'acceptance\0')
    bad_header[header_end] ^= 1
    (root / 'header-crc.tgz').write_bytes(bad_header)
    (root / 'duplicate.tgz').write_bytes(gzip.compress(tar_bytes([('matrix/matrix', b'a'), ('./matrix/matrix', b'b')])))
    for name, path in [('absolute', '/matrix/matrix'), ('drive', 'C:/matrix/matrix'), ('traversal', '../matrix/matrix')]:
        (root / (name + '.tgz')).write_bytes(gzip.compress(tar_bytes([(path, b'x')])))
    (root / 'truncated.tgz').write_bytes(gzip.compress(tar)[:20])
    for name, omitted in [('missing-matrix', 'matrix/matrix'), ('missing-profile', 'steps/pcb/profile')]:
        (root / (name + '.tgz')).write_bytes(gzip.compress(tar_bytes([(p, d) for p, d in files if p != omitted])))
    with zipfile.ZipFile(root / 'bad-crc.zip', 'w', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('matrix/matrix', b'UNIQUE_PAYLOAD')
    bad = (root / 'bad-crc.zip').read_bytes().replace(b'UNIQUE_PAYLOAD', b'BROKEN_PAYLOAD')
    (root / 'bad-crc.zip').write_bytes(bad)
    with tarfile.open(root / 'link.tgz', 'w:gz') as archive:
        info = tarfile.TarInfo('matrix/matrix')
        info.size = 1
        archive.addfile(info, io.BytesIO(b'x'))
        info = tarfile.TarInfo('outside-link')
        info.type = tarfile.SYMTYPE
        info.linkname = '/etc/passwd'
        archive.addfile(info)
    (root / 'limits.tgz').write_bytes(gzip.compress(tar_bytes([('matrix/matrix', b'x' * 8192), ('steps/pcb/profile', b'y' * 8192)])))
    unknown = root / 'unknown'
    shutil.copytree(fixture, unknown)
    (unknown / 'steps/pcb/layers/f.cu/features').write_text('UNITS=MM\n$0 r100\nQ unknown\n')
    (root / 'empty').mkdir()
    (root / 'empty.tgz').write_bytes(gzip.compress(tar_bytes([])))
    (root / 'empty.zip').write_bytes(b'')
    with zipfile.ZipFile(root / 'duplicate.zip', 'w') as archive:
        archive.writestr('matrix/matrix', b'a')
        archive.writestr('./matrix/matrix', b'b')
    (root / 'normalized.tgz').write_bytes(gzip.compress(tar_bytes([('././matrix\\matrix', b'normalized')])) )
    bad = bytearray(tar)
    bad[0] ^= 1
    (root / 'checksum.tgz').write_bytes(gzip.compress(bad))
    bad = bytearray(gzip.compress(tar))
    bad[-4] ^= 1
    (root / 'isize.tgz').write_bytes(bad)
    corrupt = root / 'corrupt'
    corrupt.mkdir()
    for name in ['truncated.tgz', 'bad-crc.zip', 'missing-matrix.tgz', 'missing-profile.tgz',
                 'traversal.tgz', 'absolute.tgz', 'drive.tgz', 'crc.tgz', 'checksum.tgz', 'isize.tgz']:
        (root / name).rename(corrupt / name)
    (root / 'unknown').rename(corrupt / 'unknown')



if __name__ == '__main__':
    main(Path(sys.argv[1]), Path(sys.argv[2]))
