import sys
from pathlib import Path
data=bytearray()
for index in range(72000*2):
    sample=(index*131%16777216)-8388608
    data.extend(sample.to_bytes(3,'little',signed=True))
Path(sys.argv[1]).write_bytes(data)
