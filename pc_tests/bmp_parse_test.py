# BMP parser Python replica — verifies bmp_show.cpp parity with official A7 Bmp.ino
# Key: input_buffer = 150 bytes (multiple of 3 for 24-bit, 2 for 16-bit) -> no boundary over-read
import struct, io

def make_bmp(w, h, depth, fmt=0, bottom_up=True, palette=None, pixel_fn=None):
    if palette is None and depth <= 8:
        palette = [(0,0,0)]*(1<<depth)
    if depth < 8:
        row_size = ((w*depth + 8 - depth)//8 + 3) & ~3
    else:
        row_size = ((w*depth)//8 + 3) & ~3
    img_size = row_size * h
    header_size = 40 + (len(palette)*4 if palette else 0) if depth<=8 else 40
    if fmt == 3:
        header_size = 40 + 12
    off = 14 + header_size
    out = bytearray()
    out += b'BM'
    out += struct.pack('<I', off + img_size)
    out += struct.pack('<I', 0)
    out += struct.pack('<I', off)
    out += struct.pack('<I', header_size)
    out += struct.pack('<i', w)
    out += struct.pack('<i', h if bottom_up else -h)
    out += struct.pack('<H', 1)
    out += struct.pack('<H', depth)
    out += struct.pack('<I', fmt)
    out += struct.pack('<I', img_size)
    out += struct.pack('<i', 2835); out += struct.pack('<i', 2835)
    out += struct.pack('<I', 0); out += struct.pack('<I', 0)
    if fmt == 3:
        out += struct.pack('<I', 0xF800) + struct.pack('<I', 0x07E0) + struct.pack('<I', 0x001F)
    if palette:
        for (r,g,b) in palette:
            out += bytes((b,g,r,0))
    rows = []
    for ly in range(h):
        row = bytearray(row_size)
        for lx in range(w):
            r,g,b = pixel_fn(lx, ly)
            if depth == 24:
                row[lx*3:lx*3+3] = bytes((b,g,r))
            elif depth == 16:
                if fmt == 0:
                    v = ((r>>3)<<10)|((g>>3)<<5)|(b>>3)
                else:
                    v = ((r>>3)<<11)|((g>>2)<<5)|(b>>3)
                row[lx*2:lx*2+2] = struct.pack('<H', v)
            elif depth == 8:
                row[lx] = palette.index((r,g,b))
            elif depth == 4:
                v = palette.index((r,g,b))
                if lx % 2 == 0:
                    row[lx//2] = v << 4
                else:
                    row[lx//2] |= v
            elif depth == 1:
                bit = 1 if (r+g+b)//3 > 127 else 0
                if lx % 8 == 0:
                    row[lx//8] = 0
                row[lx//8] |= bit << (7 - lx%8)
        rows.append(row)
    if bottom_up:
        rows.reverse()
    out += b''.join(rows)
    return bytes(out)

def parse_c(data, W=296, H=128):
    f = io.BytesIO(data)
    def r16():
        b = f.read(2); return b[0] | (b[1]<<8)
    def r32():
        b = f.read(4); return b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24)
    if r16() != 0x4D42:
        return None
    r32(); r32()
    imageOffset = r32(); headerSize = r32()
    width = r32(); height = r32()
    if height >= 0x80000000:   # BMP 高度可为负（自顶向下），read32 无符号，需转有符号
        height -= 0x100000000
    planes = r16(); depth = r16(); fmt = r32()
    flip = True
    if height < 0:
        height = -height; flip = False
    w = width if width < W else W
    h = height if height < H else H
    if not (w > 0 and h > 0 and planes == 1 and fmt in (0,3) and depth in (1,4,8,16,24)):
        return None
    if depth < 8:
        row_size = ((width*depth + 8 - depth)//8 + 3) & ~3
    else:
        row_size = ((width*depth)//8 + 3) & ~3
    mono = [0]*40
    if depth <= 8:
        f.seek(imageOffset - (4 << depth))
        for pn in range(1 << depth):
            b = f.read(1)[0]; g = f.read(1)[0]; r = f.read(1)[0]; f.read(1)
            whitish = (r + g + b) > 0x180
            if pn % 8 == 0:
                mono[pn//8] = 0
            if whitish:
                mono[pn//8] |= 1 << (pn % 8)
    bitmask = 0xFF >> (8 - depth) if depth < 8 else 0xFF
    bitshift = 8 - depth
    buf = bytearray(150)
    blacks = []
    rowPos = imageOffset + (height - h) * row_size if flip else imageOffset
    for row in range(h):
        yrow = (h - row - 1) if flip else row
        in_remain = row_size
        in_idx = 0
        in_bytes = 0
        in_byte = 0
        in_bits = 0
        f.seek(rowPos)
        for col in range(w):
            if in_idx >= in_bytes:
                chunk = f.read(min(in_remain, 150))
                in_bytes = len(chunk)
                if in_bytes == 0:
                    break
                in_remain -= in_bytes
                for i in range(in_bytes):
                    buf[i] = chunk[i]
                in_idx = 0
            if depth == 24:
                blue = buf[in_idx]; green = buf[in_idx+1]; red = buf[in_idx+2]; in_idx += 3
                whitish = (red + green + blue) > 0x180
            elif depth == 16:
                lsb = buf[in_idx]; msb = buf[in_idx+1]; in_idx += 2
                if fmt == 0:
                    blue = (lsb & 0x1F) << 3; green = ((msb & 0x03) << 6) | ((lsb & 0xE0) >> 2); red = (msb & 0x7C) << 1
                else:
                    blue = (lsb & 0x1F) << 3; green = ((msb & 0x07) << 5) | ((lsb & 0xE0) >> 3); red = msb & 0xF8
                whitish = (red + green + blue) > 0x180
            else:
                if in_bits == 0:
                    in_byte = buf[in_idx]; in_idx += 1; in_bits = 8
                pn = (in_byte >> bitshift) & bitmask
                whitish = (mono[pn//8] >> (pn % 8)) & 1
                in_byte = (in_byte << depth) & 0xFF
                in_bits -= depth
            if not whitish:
                blacks.append((col, yrow))
        rowPos += row_size
    return set(blacks)

def pat(w,h,depth):
    def fn(x,y):
        if x < w//2: return (0,0,0)
        if x == w//2 and y == h//2: return (0,0,0)
        return (255,255,255)
    return fn

def check(name, data, exp):
    got = parse_c(data)
    assert got is not None, name+": parse failed"
    assert got == exp, name+": mismatch"
    print(name+": PASS ("+str(len(got))+" black)")

w,h=10,5
exp = {(x,y) for y in range(h) for x in range(w) if x < w//2 or (x==w//2 and y==h//2)}
check("24bit bottom-up", make_bmp(w,h,24,bottom_up=True,pixel_fn=pat(w,h,24)), exp)
check("24bit top-down", make_bmp(w,h,24,bottom_up=False,pixel_fn=pat(w,h,24)), exp)
pal=[(0,0,0),(255,255,255)]
check("1bit palette", make_bmp(w,h,1,palette=pal,pixel_fn=pat(w,h,1)), exp)
pal8=[(0,0,0)]+[(255,255,255)]*255
def p8(x,y):
    r,g,b=pat(w,h,8)(x,y); return (0,0,0) if (r,g,b)==(0,0,0) else (255,255,255)
check("8bit palette", make_bmp(w,h,8,palette=pal8,pixel_fn=p8), exp)
check("16bit 565", make_bmp(w,h,16,fmt=3,pixel_fn=pat(w,h,16)), exp)
check("16bit 555", make_bmp(w,h,16,fmt=0,pixel_fn=pat(w,h,16)), exp)
exp7={(x,y) for y in range(128) for x in range(296) if x<150}
check("clamp 300x200", make_bmp(300,200,24,pixel_fn=lambda x,y:(0,0,0) if x<150 else (255,255,255)), exp7)
pal4=[(0,0,0),(255,255,255)]*8
check("4bit palette", make_bmp(w,h,4,palette=pal4,pixel_fn=p8), exp)
exp9={(x,y) for y in range(5) for x in range(5) if x<2}
check("1bit w=5 border", make_bmp(5,5,1,palette=pal,pixel_fn=lambda x,y:(0,0,0) if x<2 else (255,255,255)), exp9)
assert parse_c(b'XX') is None
print("corrupt header: PASS")
print("ALL PASS")
