"""Usage validation must precede optional JSON/ECAD support checks."""
import subprocess
from assembly_test_support import BUILD, check

binary = str(BUILD / 'kernel/chipcae-import')
for args in (
        ['--assembly', 'assembly.json'],
        ['--assembly', 'assembly.json', '-o', 'out.json', '--accept', 'accept.json',
         '--accept-b-tier'],
        ['--accept-b-tier', '--accept', 'accept.json', '--assembly', 'assembly.json',
         '-o', 'out.json'],
        ['--assembly', 'assembly.json', '-o', 'out.json', '--t-min', '1e-6'],
        ['--assembly', 'assembly.json', '-o', 'out.json', '--material-map', 'map.json'],
        ['input.xml', '--assembly', 'assembly.json', '-o', 'out.json']):
    check(subprocess.run([binary, *args], capture_output=True, text=True, timeout=25), 2)
