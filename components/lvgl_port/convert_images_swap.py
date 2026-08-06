import re, os, glob

src_dir = r'E:/ESP/dlna/components/lvgl_port/ui_ref'
for f in sorted(glob.glob(os.path.join(src_dir, 'ui_img_*.c'))):
    with open(f, 'r') as fh:
        content = fh.read()

    match = re.search(r'(\w+)_data\[\]\s*=\s*\{(.*?)\};', content, re.DOTALL)
    if not match:
        print(f'{os.path.basename(f)}: no data array found, skip')
        continue

    name = match.group(1)
    hex_str = match.group(2)

    bytes_list = [int(b.strip(), 16) for b in hex_str.replace('\n', '').split(',') if b.strip()]

    if len(bytes_list) == 0:
        print(f'{os.path.basename(f)}: empty data, skip')
        continue

    if 'TRUE_COLOR_ALPHA' in content:
        bpp = 3
    elif 'TRUE_COLOR' in content:
        bpp = 2
    else:
        print(f'{os.path.basename(f)}: unknown format, skip')
        continue

    converted = []
    for i in range(0, len(bytes_list), bpp):
        pixel = bytes_list[i:i+bpp]
        if len(pixel) == bpp:
            if bpp == 3:
                converted.extend([pixel[1], pixel[0], pixel[2]])
            else:
                converted.extend([pixel[1], pixel[0]])
        else:
            converted.extend(pixel)

    new_data_parts = []
    for i in range(0, len(converted), 16):
        line = ', '.join(f'0x{b:02X}' for b in converted[i:i+16])
        if i + 16 < len(converted):
            new_data_parts.append(f'    {line},')
        else:
            new_data_parts.append(f'    {line}')

    new_data_str = '\n'.join(new_data_parts)

    new_content = content.replace(
        match.group(0),
        f'{name}_data[] = {{\n{new_data_str}\n}};'
    )

    with open(f, 'w') as fh:
        fh.write(new_content)

    pixels = len(bytes_list) // bpp
    print(f'{os.path.basename(f)}: {pixels}px, {len(bytes_list)}B -> {len(converted)}B, swapped OK')

print('Done!')