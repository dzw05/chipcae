#!/usr/bin/env python3
"""Generate owned ODB++ acceptance inputs, including corrupt binary cases."""
import gzip
import io
import random
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


def gzip_member(data, flags=0x1e):
    header = b'\x1f\x8b\x08' + bytes([flags]) + b'\0' * 4 + b'\0\xff'
    if flags & 4:
        header += struct.pack('<H', 4) + b'ABCD'
    if flags & 8:
        header += b'part.tar\0'
    if flags & 16:
        header += b'acceptance\0'
    if flags & 2:
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
    for name, members in [('selected', [('small', b'ok'), ('irrelevant', b'stop\n' + b'x' * (1024 * 1024))]),
                          ('nested-Z', [('payload.Z', b'compressed bytes')])]:
        (root / (name + '.tgz')).write_bytes(gzip.compress(tar_bytes(members)))
        with zipfile.ZipFile(root / (name + '.zip'), 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for path, data in members:
                archive.writestr(path, data)
    (root / 'sliding-match.gz').write_bytes(gzip.compress(b'x' * (1024 * 1024)))
    stop_files = [('small', b'ok'), ('large', b'stop\n' + b'x' * 200000)]
    (root / 'early-stop.tgz').write_bytes(gzip.compress(tar_bytes(stop_files), compresslevel=0))
    with zipfile.ZipFile(root / 'early-stop.zip', 'w', compression=zipfile.ZIP_STORED) as archive:
        for path, data in stop_files:
            archive.writestr(path, data)
    files = [(p.relative_to(fixture).as_posix(), p.read_bytes()) for p in sorted(fixture.rglob('*')) if p.is_file()]
    for path, data in files:
        destination = root / 'job' / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    for label, required in [('irrelevant-Z', None), ('matrix-Z', 'matrix/matrix'),
                            ('profile-Z', 'steps/pcb/profile')]:
        members = [(path + '.Z' if path == required else path,
                    lzw_literals(data) if path == required else data) for path, data in files]
        if required is None:
            members.append(('irrelevant.Z', lzw_literals(b'ignored')))
        (root / (label + '.tgz')).write_bytes(gzip.compress(tar_bytes(members)))
        with zipfile.ZipFile(root / (label + '.zip'), 'w') as archive:
            for path, data in members:
                archive.writestr(path, data)
    selected_job = files + [('irrelevant', b'x' * (1024 * 1024))]
    (root / 'selected-job.tgz').write_bytes(gzip.compress(tar_bytes(selected_job)))
    with zipfile.ZipFile(root / 'selected-job.zip', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for path, data in selected_job:
            archive.writestr(path, data)
    tar = tar_bytes(files)
    (root / 'job.tar').write_bytes(tar)
    (root / 'job.tgz').write_bytes(gzip.compress(tar))
    (root / 'JOB.TAR.GZ').write_bytes(gzip.compress(tar))
    for method, name in [(zipfile.ZIP_STORED, 'stored.zip'), (zipfile.ZIP_DEFLATED, 'job.zip')]:
        with zipfile.ZipFile(root / name, 'w', compression=method) as archive:
            for path, data in files:
                archive.writestr(path, data)
    for method, label in [(zipfile.ZIP_STORED, 'stored'), (zipfile.ZIP_DEFLATED, 'deflate')]:
        out = io.BytesIO()
        with zipfile.ZipFile(out, 'w', compression=method) as archive:
            archive.writestr('payload', b'nonempty')
        corrupt = bytearray(out.getvalue())
        central = corrupt.index(b'PK\x01\x02')
        struct.pack_into('<I', corrupt, 18, 0)
        struct.pack_into('<I', corrupt, central + 20, 0)
        (root / ('zero-compressed-' + label + '.zip')).write_bytes(corrupt)
    with zipfile.ZipFile(root / 'empty-entry.zip', 'w') as archive:
        archive.writestr('payload', b'')
    (root / 'framing-only.tgz').write_bytes(gzip.compress(tar_bytes([])))
    for optional in ['features', 'tools']:
        with zipfile.ZipFile(root / ('optional-' + optional + '.zip'), 'w') as archive:
            for path, data in files:
                if path != 'steps/pcb/layers/f.cu/' + optional:
                    archive.writestr(path, data)
            archive.writestr('steps/pcb/layers/f.cu/' + optional, b'unsupported',
                             compress_type=zipfile.ZIP_BZIP2)
    with zipfile.ZipFile(root / 'required-profile.zip', 'w') as archive:
        for path, data in files:
            archive.writestr(path, data, compress_type=(zipfile.ZIP_BZIP2
                             if path == 'steps/pcb/profile' else zipfile.ZIP_STORED))
    with zipfile.ZipFile(root / 'dirs-only.zip', 'w') as archive:
        for i in range(3):
            archive.writestr('dir%d/' % i, b'')
    with zipfile.ZipFile(root / 'unsupported-count.zip', 'w', compression=zipfile.ZIP_BZIP2) as archive:
        for i in range(3):
            archive.writestr('file%d' % i, b'x')
    oldgnu = bytearray(tar_bytes([('matrix/matrix', b'oldgnu')], tarfile.GNU_FORMAT))
    oldgnu[345:357] = b'00000000001\0'
    oldgnu[357:369] = b'00000000002\0'
    oldgnu[148:156] = b' ' * 8
    oldgnu[148:156] = ('%06o\0 ' % sum(oldgnu[:512])).encode()
    (root / 'oldgnu-times.tgz').write_bytes(gzip.compress(oldgnu))
    # Stored deflate blocks keep these line-limit fixtures below the ratio limit.
    for suffix, payload in [('boundary', b'#' * (1024 * 1024)),
                            ('boundary-crlf', b'#' * (1024 * 1024) + b'\r\n'),
                            ('boundary-cr', b'#' * (1024 * 1024) + b'\r'),
                            ('overflow-crlf', b'#' * (1024 * 1024 + 1) + b'\r\n'),
                            ('overflow', b'#' * (1024 * 1024 + 1)),
                            ('overflow-newline', b'#' * (1024 * 1024 + 1) + b'\n')]:
        entry = [('matrix/matrix', payload)]
        (root / ('line-' + suffix + '.tgz')).write_bytes(
            gzip.compress(tar_bytes(entry), compresslevel=0))
        with zipfile.ZipFile(root / ('line-' + suffix + '.zip'), 'w',
                             compression=zipfile.ZIP_STORED) as archive:
            archive.writestr('matrix/matrix', payload)
    # POSIX prefix layout with a space-terminated magic and explicit version.
    space_ustar = bytearray(tar_bytes([('prefix/' + 'f' * 99, b'space magic')]))
    space_ustar[262] = ord(' ')
    space_ustar[148:156] = b' ' * 8
    space_ustar[148:156] = ('%06o\0 ' % sum(space_ustar[:512])).encode()
    (root / 'space-ustar.tgz').write_bytes(gzip.compress(space_ustar))
    for label, path in [('colon', 'part:rev/file'), ('absolute', '/matrix/matrix'),
                        ('drive', 'C:/matrix/matrix'), ('relative-drive', 'C:matrix'),
                        ('traversal', '../matrix/matrix')]:
        with zipfile.ZipFile(root / ('path-' + label + '.zip'), 'w') as archive:
            archive.writestr(path, b'x')
        (root / ('path-' + label + '.tgz')).write_bytes(
            gzip.compress(tar_bytes([(path, b'x')])))
    cap_files = [('matrix/matrix', b'x' * 8192), ('steps/pcb/profile', b'y' * 8192)]
    with zipfile.ZipFile(root / 'caps.zip', 'w', compression=zipfile.ZIP_STORED) as archive:
        for path, data in cap_files:
            archive.writestr(path, data)
    for path, data in cap_files:
        destination = root / 'caps-dir' / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    long_path = '/'.join(['directory' * 10] * 3) + '/features'
    format_files = [('matrix/matrix', b'UNITS=MM\n'), (long_path, b'long path payload\0\xff')]
    # Ustar permits at most 155 prefix + 100 basename bytes. A longer complete path
    # cannot be represented; use its maximum legal split, and GNU/pax for >255.
    legal = 'p' * 155 + '/' + 'f' * 100
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
    encrypted = bytearray((root / 'stored.zip').read_bytes())
    # Set the encryption flag in every local and central header; payload is never read.
    for signature, flag_offset in [(b'PK\x03\x04', 6), (b'PK\x01\x02', 8)]:
        start = 0
        while (pos := encrypted.find(signature, start)) >= 0:
            encrypted[pos + flag_offset] |= 1
            start = pos + 4
    (root / 'encrypted.zip').write_bytes(encrypted)
    # Unsupported files and directories never enter payload safety accounting.
    for source in ['unsupported.zip', 'zip64.zip', 'encrypted.zip']:
        shutil.copyfile(root / source, root / ('ratio-' + source))
    with zipfile.ZipFile(root / 'directories.zip', 'w') as archive:
        archive.writestr('steps/pcb/', b'')
        archive.writestr('payload', b'x')
    # Stored deflate keeps each small entry below even the reduced 2:1 budget.
    (root / 'many-small.tgz').write_bytes(gzip.compress(tar_bytes(
        [('file%d' % i, b'x' * 1024) for i in range(100)]), compresslevel=0))
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
    # Stored deflate crosses the 32 KiB input boundary, including exact EOF alignment.
    for size in [0, 32745, 32746, 65513]:
        (root / ('boundary-%d.gz' % size)).write_bytes(gzip.compress(b'x' * size, compresslevel=0))
    for flag in [2, 4, 8, 16]:
        (root / ('flag-%d.gz' % flag)).write_bytes(gzip_member(b'flag payload', flag))
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
    with tarfile.open(root / 'types.tgz', 'w:gz') as archive:
        info = tarfile.TarInfo('matrix/matrix')
        info.size = 1
        archive.addfile(info, io.BytesIO(b'x'))
        for name, kind in [('hard', tarfile.LNKTYPE), ('sym', tarfile.SYMTYPE),
                           ('char', tarfile.CHRTYPE), ('block', tarfile.BLKTYPE),
                           ('fifo', tarfile.FIFOTYPE), ('contiguous', tarfile.CONTTYPE)]:
            info = tarfile.TarInfo(name)
            info.type = kind
            info.linkname = 'target'
            archive.addfile(info)
    limit_tar = tar_bytes([('matrix/matrix', b'x' * 8192), ('steps/pcb/profile', b'y' * 8192)])
    (root / 'limits.tgz').write_bytes(gzip.compress(limit_tar, compresslevel=0))
    (root / 'limits-ratio.tgz').write_bytes(gzip.compress(limit_tar))
    unknown = root / 'unknown'
    shutil.copytree(root / 'job', unknown)
    (unknown / 'steps/pcb/layers/f.cu/features').write_text('UNITS=MM\n$0 r100\nQ unknown\n')
    (root / 'empty').mkdir()
    (root / 'empty.tgz').write_bytes(gzip.compress(tar_bytes([])))
    (root / 'empty.zip').write_bytes(b'')
    for kind in [tarfile.LNKTYPE, tarfile.SYMTYPE, tarfile.CHRTYPE,
                 tarfile.BLKTYPE, tarfile.FIFOTYPE, tarfile.CONTTYPE, tarfile.DIRTYPE]:
        for regular_first in [False, True]:
            with tarfile.open(root / f'shared-path-{kind.decode()}-{regular_first}.tgz', 'w:gz') as archive:
                for entry_kind in ([tarfile.REGTYPE, kind] if regular_first
                                   else [kind, tarfile.REGTYPE]):
                    info = tarfile.TarInfo('shared' if entry_kind == tarfile.REGTYPE else './shared/')
                    info.type = entry_kind
                    if entry_kind == tarfile.REGTYPE:
                        info.size = 7
                        archive.addfile(info, io.BytesIO(b'payload'))
                    else:
                        info.linkname = 'target' if entry_kind in [tarfile.LNKTYPE, tarfile.SYMTYPE] else ''
                        archive.addfile(info)
    with tarfile.open(root / 'duplicate-directories.tgz', 'w:gz') as archive:
        for name in ['shared/', './shared/']:
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE
            archive.addfile(info)

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
    # Incompressible neighbors must not dilute a highly compressed entry.
    noise = random.Random(93).randbytes(256 * 1024)
    for order in ['before', 'after']:
        members = [('bomb', b'x' * (1024 * 1024))]
        members.insert(0 if order == 'before' else 1, ('noise', noise))
        (root / ('ratio-padded-' + order + '.tgz')).write_bytes(
            gzip.compress(tar_bytes(members)))
    # Empty deflate blocks after a payload must not subsidize its ratio budget.
    payload = b'x' * 65536
    framed = tar_bytes([('bomb', payload), ('neighbor', b'neighbor')])
    boundary = 512 + len(payload)
    compressor = zlib.compressobj(wbits=-15)
    raw = compressor.compress(framed[:boundary]) + compressor.flush(zlib.Z_FULL_FLUSH)
    raw += b'\x00\x00\x00\xff\xff' * 4096
    raw += compressor.compress(framed[boundary:]) + compressor.flush()
    wrapped = (b'\x1f\x8b\x08\x00' + b'\x00' * 6 + raw +
               struct.pack('<II', zlib.crc32(framed), len(framed)))
    assert gzip.decompress(wrapped) == framed
    (root / 'ratio-empty-blocks.tgz').write_bytes(wrapped)
    corrupt = root / 'corrupt'
    corrupt.mkdir()
    for name in ['truncated.tgz', 'bad-crc.zip', 'missing-matrix.tgz', 'missing-profile.tgz',
                 'traversal.tgz', 'absolute.tgz', 'drive.tgz', 'crc.tgz', 'checksum.tgz', 'isize.tgz',
                 'ratio-padded-before.tgz', 'ratio-padded-after.tgz']:
        (root / name).rename(corrupt / name)
    (root / 'unknown').rename(corrupt / 'unknown')



if __name__ == '__main__':
    main(Path(sys.argv[1]), Path(sys.argv[2]))
