import re, os, glob

src_dir = r'E:/ESP/dlna/components/lvgl_port/ui_ref'
for f in glob.glob(os.path.join(src_dir, 'ui_img_*.c')):
    with open(f, 'r') as fh:
        content = fh.read()

    match = re.search(r'(\w+)_data\[\] = \{(.*?)\};', content, re.DOTALL)
    if not match:
        continue
    name = match.group(1)
    hex_str = match.group(2)

    # Parse bytes
    bytes_list = [int(b.strip(), 16) for b in hex_str.replace('\n', '').split(',') if b.strip()]

    # Check if already converted (data size is even)
    if len(bytes_list) % 2 == 0 and len(bytes_list) < len(bytes_list) * 0.75:
        print(f'{os.path.basename(f)}: already RGB565? ({len(bytes_list)} bytes)')
        # Already converted, skip
        continue

    # Convert RGBA8888 to RGB565 (skip alpha)
    rgb565 = []
    for i in range(0, len(bytes_list) - 3, 4):
        r, g, b, a = bytes_list[i], bytes_list[i+1], bytes_list[i+2], bytes_list[i+3]
        val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb565.append(val & 0xFF)
        rgb565.append((val >> 8) & 0xFF)

    # Read original header to get dimensions
    w_match = re.search(r'\.header\.w\s*=\s*(\d+)', content)
    h_match = re.search(r'\.header\.h\s*=\s*(\d+)', content)
    w = w_match.group(1) if w_match else '?'
    h = h_match.group(1) if h_match else '?'

    new_content = f'#include "lvgl.h"\n\n'
    new_content += f'const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_data[] = {{\n'
    for i in range(0, len(rgb565), 16):
        line = ', '.join(f'0x{b:02X}' for b in rgb565[i:i+16])
        if i + 16 < len(rgb565):
            new_content += f'    {line},\n'
        else:
            new_content += f'    {line}\n'
    new_content += '};\n\n'
    new_content += f'const lv_img_dsc_t {name} = {{\n'
    new_content += '    .header.always_zero = 0,\n'
    new_content += f'    .header.w = {w},\n'
    new_content += f'    .header.h = {h},\n'
    new_content += f'    .data_size = sizeof({name}_data),\n'
    new_content += '    .header.cf = LV_IMG_CF_TRUE_COLOR,\n'
    new_content += f'    .data = {name}_data\n'
    new_content += '};\n'

    with open(f, 'w') as fh:
        fh.write(new_content)

    pixels = len(bytes_list) // 4
    print(f'{os.path.basename(f)}: {w}x{h} {pixels}px -> {len(rgb565)} bytes RGB565 ✓')