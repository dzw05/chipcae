"""Local simulator: CAELOG1 supported payloads plus explicit mock RunEvent JSONL."""
import argparse
import json
import math
from pathlib import Path
import struct
import time
import warnings
import zlib

MAGIC = b'CAELOG1\0'
# Frozen v1 kinds implemented by io/event_log.hpp, in proto field order.
LAYOUT = {
    'progress': (10, [('window','Q'),('time','d'),('dt','d'),('fraction','d'),('message','s')]),
    'residual': (11, [('solver','s'),('field','s'),('window','Q'),('iteration','I'),('norm','d'),('flags','I'),('code','s'),('error_code','I')]),
    'log_line': (20, [('level','i'),('rank','i'),('message','s'),('code','s'),('error_code','I')]),
    'state': (21, [('value','B'),('window','Q'),('time','d')]),
    'output_available': (22, [('window','Q'),('time','d'),('path','s'),('checksum','s')]),
}


def require_format(event_log_format):
    if event_log_format != 1:
        raise ValueError(f'unsupported event_log_format={event_log_format}')


def record(seq, kind, payload):
    code, fields = LAYOUT[kind]
    body = struct.pack('<QqB',seq,1700000000000000000+seq,code)
    for name,fmt in fields:
        value = payload.get(name, '' if fmt == 's' else 0)
        if fmt == 's':
            value = value.encode('utf-8'); body += struct.pack('<I',len(value))+value
        else: body += struct.pack('<'+fmt,value)
    return MAGIC + struct.pack('<II',len(body),zlib.crc32(body)) + body


def read_log(path, event_log_format=1, from_seq=0):
    """Inclusive seq; return records and last validated offset, stop at torn/bad CRC tail."""
    require_format(event_log_format)
    data = Path(path).read_bytes(); offset = 0; events = []
    while len(data)-offset >= 16:
        if data[offset:offset+8] != MAGIC: raise ValueError('invalid event log magic')
        length,crc = struct.unpack_from('<II',data,offset+8)
        if length > 64*1024*1024 or len(data)-offset-16 < length: break
        body = data[offset+16:offset+16+length]
        if zlib.crc32(body) != crc: break
        seq,wall,code = struct.unpack_from('<QqB',body)
        matching = [(k,fields) for k,(c,fields) in LAYOUT.items() if c == code]
        if not matching: raise ValueError('unsupported payload kind')
        kind,fields = matching[0]; p = 17; payload = {}
        for name,fmt in fields:
            if fmt == 's':
                n = struct.unpack_from('<I',body,p)[0]; p += 4
                if p+n > len(body): raise ValueError('truncated string')
                payload[name] = body[p:p+n].decode(); p += n
            else:
                payload[name] = struct.unpack_from('<'+fmt,body,p)[0]; p += struct.calcsize('<'+fmt)
        if p != len(body): raise ValueError('trailing payload bytes')
        if seq >= from_seq: events.append(dict(seq=seq,wall_time_unix_ns=wall,**{kind:payload}))
        offset += 16+length
    return events,offset


class ControlReader:
    def __init__(self,path):
        self.path = Path(path); self.offset = 0; self.seq = 0

    def poll(self):
        if not self.path.exists(): return []
        data = self.path.read_bytes(); result = []
        while True:
            end = data.find(b'\n',self.offset)
            if end < 0: break
            line = data[self.offset:end]; self.offset = end+1
            try:
                c = json.loads(line)
                if not isinstance(c,dict): raise ValueError('command must be an object')
                fields = {'cmd_seq','client_request_id','action','issued_at',
                          'output_interval_time','output_interval_steps'}
                aliases = {'cmdSeq':'cmd_seq','clientRequestId':'client_request_id',
                           'issuedAt':'issued_at','outputIntervalTime':'output_interval_time',
                           'outputIntervalSteps':'output_interval_steps'}
                normalized = {}
                for key,value in c.items():
                    name = aliases.get(key,key)
                    if name not in fields or name in normalized:
                        raise ValueError('unknown or duplicate control field')
                    normalized[name] = value
                c = normalized
                if 'issued_at' in c:
                    from google.protobuf.timestamp_pb2 import Timestamp
                    Timestamp().FromJsonString(c['issued_at'])
                if 'output_interval_time' in c and 'output_interval_steps' in c:
                    raise ValueError('multiple oneof arguments')
                if 'output_interval_time' in c:
                    v = c['output_interval_time']
                    if isinstance(v,bool) or not isinstance(v,(int,float)) or not math.isfinite(v):
                        raise ValueError('invalid interval time')
                if 'output_interval_steps' in c:
                    v = c['output_interval_steps']
                    if not isinstance(v,str) or not v.isdecimal() or not 0 <= int(v) < 2**64:
                        raise ValueError('invalid interval steps')
                # Accept canonical proto JSON camelCase and proto field spelling.
                seq = c.get('cmdSeq',c.get('cmd_seq'))
                if not isinstance(seq,str) or not seq.isdecimal() or not 0 < int(seq) < 2**64:
                    raise ValueError('cmd_seq must be a positive uint64 string')
                action = c.get('action')
                if action not in ('PAUSE','RESUME','STOP','SET_OUTPUT_INTERVAL','WRITE_OUTPUT_NOW','CHECKPOINT_AND_STOP'):
                    raise ValueError('invalid action')
                request = c.get('clientRequestId',c.get('client_request_id',''))
                if not isinstance(request,str): raise ValueError('invalid client_request_id')
                seq = int(seq)
                if seq <= self.seq: continue
                self.seq = seq
                result.append(dict(cmd_seq=seq,client_request_id=request,action=action))
            except (ValueError,TypeError,AttributeError) as exc:
                warnings.warn(f'invalid control line: {exc}',RuntimeWarning)
        return result


def run(path, windows=3, delay=0.):
    path = Path(path); path.mkdir(parents=True,exist_ok=True)
    controls = ControlReader(path/'control.jsonl')
    # Do not truncate a control file populated by the caller.
    # The launcher broker owns control.jsonl; the reader tolerates a missing file.
    with (path/'events.caelog').open('wb') as wire, (path/'mock-events.jsonl').open('w') as stream:
        seq = 0
        def emit(kind,payload):
            nonlocal seq
            seq += 1
            event = dict(seq=str(seq),wall_time_unix_ns=str(1700000000000000000+seq),**{kind:payload})
            stream.write(json.dumps(event)+'\n'); stream.flush()
            if kind in LAYOUT: wire.write(record(seq,kind,payload)); wire.flush()
        emit('state',dict(value=1,window=0,time=0.))
        emit('state',dict(value=2,window=0,time=0.))
        stopped = False
        for n in range(1,windows+1):
            if delay: time.sleep(delay)
            emit('progress',dict(window=n,time=n*.5,dt=.5,fraction=n/windows,message='mock'))
            emit('residual',dict(solver='mechanics',field='U',window=n,iteration=1,norm=1e-5,flags=1))
            emit('residual_batch',dict(window=n,time=n*.5,points=[dict(solver='mechanics',field='U',iteration=2,norm=1e-8,flags=2,linear_iterations=2,window_iterations=2)],dropped_intermediate=0))
            emit('log_line',dict(level=1,rank=0,message='mock window complete'))
            # Advertise only a registered, complete result if there is one.
            manifest = path/'manifest.json'
            if manifest.exists():
                m = json.loads(manifest.read_text())
                for s in m['steps']:
                    if s['n'] == n-1 and s.get('complete',True):
                        emit('output_available',dict(window=n,time=s['time'],path=s['file'],checksum=s['checksum']))
            for c in controls.poll():
                applied = c['action'] == 'STOP'
                emit('control_ack',dict(cmd_seq=str(c['cmd_seq']),client_request_id=c['client_request_id'],
                     status='APPLIED' if applied else 'REJECTED',effective_step=n,effective_time=n*.5,
                     message='mock boundary STOP' if applied else 'mock supports STOP only'))
                stopped |= applied
            if stopped: break
        emit('state',dict(value=4,window=n,time=n*.5))
        emit('exit',dict(exit_code=0,last_completed_window=n,reason='user_stop' if stopped else 'end_reached'))


def fixtures(path):
    path = Path(path); path.mkdir(parents=True,exist_ok=True)
    a = record(1,'state',dict(value=1)); b = record(2,'progress',dict(window=1,time=.5,dt=.5,fraction=.5))
    (path/'valid.caelog').write_bytes(a+b)
    (path/'half.caelog').write_bytes(a+b[:-7])
    bad = bytearray(b); bad[-1] ^= 1
    (path/'bad-crc.caelog').write_bytes(a+bad)
    atomic = {'event_log_format':1,'valid_first_record_bytes':len(a),'valid_bytes':len(a+b),
              'note':'repeatable mock fixture; B3 writer golden remains external'}
    (path/'fixture.json').write_text(json.dumps(atomic,indent=2)+'\n')

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run-dir',type=Path); p.add_argument('--case',type=Path)
    p.add_argument('--profile',type=Path); p.add_argument('--out',type=Path); p.add_argument('--windows',type=int,default=3)
    p.add_argument('--delay',type=float,default=0.)
    a = p.parse_args()
    if a.windows < 1: p.error('--windows must be positive')
    output = a.out or a.run_dir
    if output is None: p.error('--run-dir or --out is required')
    if a.out:
        if not a.case or not a.profile: p.error('--out requires --case and --profile')
        import sys, os
        output.mkdir(parents=True,exist_ok=True)
        (output/'mock-argv.json').write_text(json.dumps(dict(argv=sys.argv[1:], omp=os.environ.get('OMP_NUM_THREADS'))))
        profile = json.loads(a.profile.read_text())
        a.windows = profile.get('x-mock-windows', a.windows)
        a.delay = profile.get('x-mock-delay', a.delay)
        if profile.get('x-mock-exit'): sys.exit(profile['x-mock-exit'])
        if profile.get('x-mock-missing-exit'): sys.exit(0)
    run(output,a.windows,a.delay)
