#!/usr/bin/env python3
"""Минимальный генератор образа FAT32 (для проверки fat32.c на хосте): MBR + раздел, длинные имена, подкаталог,
   файл на несколько кластеров. Пишет независимо от читалки в ядре."""
import struct, sys
SPC, RES, NF = 1, 32, 2
TOTAL = 70000 + RES + 2*600           # кластеров > 65525
FATSZ = 600
img = bytearray(512 * (TOTAL + 2048))
PART = 2048
def sec(n): return PART*512 + n*512
bs = bytearray(512)
bs[0:3] = b'\xEB\x58\x90'; bs[3:11] = b'MSWIN4.1'
struct.pack_into('<HBHBHHBHHHII', bs, 11, 512, SPC, RES, NF, 0, 0, 0xF8, 0, 63, 255, 0, TOTAL)
struct.pack_into('<IHHIHH', bs, 36, FATSZ, 0, 0, 2, 1, 6)
bs[510:512] = b'\x55\xAA'
img[sec(0):sec(0)+512] = bs
fat = [0]*(FATSZ*128); fat[0]=0x0FFFFFF8; fat[1]=0x0FFFFFFF
data0 = RES + NF*FATSZ
nextfree = [2]
def alloc(n):
    cl = list(range(nextfree[0], nextfree[0]+n)); nextfree[0] += n
    for a,b in zip(cl, cl[1:]): fat[a]=b
    fat[cl[-1]] = 0x0FFFFFFF
    return cl
def wcl(c, data):
    o = sec(data0 + c - 2); img[o:o+len(data)] = data
def sfn_sum(n): 
    s=0
    for ch in n: s = (((s&1)<<7) + (s>>1) + ch) & 255
    return s
def entries(name, short, attr, cl, size):
    out = b''
    sh = short.ljust(11).encode('ascii')
    if name is not None:
        u = name.encode('utf-16le') + b'\x00\x00'
        u += b'\xff\xff' * ((13 - (len(u)//2) % 13) % 13)
        n = len(u)//26
        for i in range(n, 0, -1):
            ch = u[(i-1)*26:i*26]
            e = bytearray(32); e[0] = i | (0x40 if i == n else 0); e[11] = 0x0F; e[13] = sfn_sum(sh)
            for j,p in enumerate([1,3,5,7,9,14,16,18,20,22,24,28,30]): e[p:p+2] = ch[j*2:j*2+2]
            out += bytes(e)
    e = bytearray(32); e[0:11] = sh; e[11] = attr
    struct.pack_into('<H', e, 20, cl >> 16); struct.pack_into('<H', e, 26, cl & 0xFFFF); struct.pack_into('<I', e, 28, size)
    return out + bytes(e)
def addfile(content):
    n = max(1, (len(content) + 511)//512); cl = alloc(n)
    for i,c in enumerate(cl): wcl(c, content[i*512:(i+1)*512])
    return cl[0]
root = alloc(1)[0]
key = b'sk-or-v1-TESTKEYTESTKEYTESTKEY\n'
big = bytes((i*7+3) & 255 for i in range(5000))
sub = alloc(1)[0]
rootdata  = entries(None, 'KEY     TXT', 0x20, addfile(key), len(key))
rootdata += entries('Привет мир.txt', 'PRIVET~1TXT', 0x20, addfile('привет\n'.encode()), len('привет\n'.encode()))
rootdata += entries('Long Folder Name', 'LONGFO~1   ', 0x10, sub, 0)
rootdata += entries(None, 'BIG     BIN', 0x20, addfile(big), len(big))
wcl(root, rootdata)
wcl(sub, entries('.', '.          ', 0x10, sub, 0) + entries(None, 'KEYS    TXT', 0x20, addfile(b'inside\n'), 7))
fb = b''.join(struct.pack('<I', v) for v in fat)
for k in range(NF): img[sec(RES + k*FATSZ):sec(RES + k*FATSZ)+len(fb)] = fb
mbr = bytearray(512); mbr[446+4] = 0x0C; struct.pack_into('<II', mbr, 446+8, PART, TOTAL); mbr[510:512] = b'\x55\xAA'
img[0:512] = mbr
open(sys.argv[1], 'wb').write(img)
open(sys.argv[1] + '.big', 'wb').write(big)
