#!/usr/bin/env python3
"""Generate C header files with embedded font binary data."""

import sys
import os

def embed_font(font_path, output_path, var_name='font_data', font_name='Font'):
    """Convert font binary to C hex array."""
    if not os.path.exists(font_path):
        print(f'Error: Font file not found: {font_path}')
        return False
        
    with open(font_path, 'rb') as f:
        data = f.read()
    
    # Generate header guard from var name
    header_guard = f'{var_name.upper()}_H'
    
    # Generate C code
    lines = [
        f'// Auto-generated font data - {len(data)} bytes',
        f'// {font_name} font from google fonts',
        f'#ifndef {header_guard}',
        f'#define {header_guard}',
        f'',
        f'static const unsigned char {var_name}[] = {{',
    ]
    
    # Write hex bytes in rows of 16
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join(f'0x{b:02X}' for b in chunk)
        lines.append(f'    {hex_str},')
    
    lines.extend([
        '};',
        f'',
        f'static const unsigned int {var_name}_size = {len(data)};',
        f'',
        f'#endif // {header_guard}',
        '',
    ])
    
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))
    
    print(f'Generated {output_path}: {len(data)} bytes embedded')
    return True

if __name__ == '__main__':
    output_dir = os.path.join('misrc_tools', 'misrc_gui', 'assets')
    os.makedirs(output_dir, exist_ok=True)

    # Generate Inter font header
    inter_font_path = os.path.join('assets', 'fonts', 'static', 'Inter_18pt-Regular.ttf')
    inter_output_path = os.path.join(output_dir, 'inter_font_data.h')
    embed_font(inter_font_path, inter_output_path, 'inter_font_data', 'Inter')
    
    # Generate Space Mono font header
    space_mono_path = os.path.join('assets', 'fonts', 'SpaceMono-Regular.ttf')
    space_mono_output_path = os.path.join(output_dir, 'space_mono_font_data.h')
    embed_font(space_mono_path, space_mono_output_path, 'space_mono_font_data', 'Space Mono')
