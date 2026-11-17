#!/usr/bin/env python3

import sys, os

manifestfile, cppfile, hfile = sys.argv[1:]

def sanitize_identifier(path):
    base = os.path.splitext(path.replace('\\', '/'))[0]
    sanitized = []
    for c in base:
        if c.isalnum() or c == '_':
            sanitized.append(c)
        else:
            sanitized.append('_')
    ident = ''.join(sanitized)
    if ident and ident[0].isdigit():
        ident = '_' + ident
    return ident

with open(manifestfile, 'r') as manifest:
    files = dict((x.strip(), None) for x in manifest.readlines() if x.strip() != '')

sourcepath = os.path.split(manifestfile)[0]
buildpath = os.path.split(cppfile)[0]
output_base = os.path.splitext(os.path.basename(cppfile))[0]
generate_bitmap_table = output_base == 'bitmap'

for k in files:
    sf = os.path.join(sourcepath, k)
    bf = os.path.join(buildpath, k)

    if os.path.isfile(sf):
        files[k] = sf
    elif os.path.isfile(bf):
        files[k] = bf
    else:
        print("{}: Failed to open '{}'".format(manifestfile, k))
        sys.exit(1)

entries = []
used_names = set()

with open(cppfile, 'w') as cpp:
    cpp.write('#include "libresrc.h"\n')
    with open(hfile, 'w') as h:
        h.write('#include <stddef.h>\n')

        for k in files:
            with open(files[k], 'rb') as f:
                data = [str(int(x)) for x in f.read()]

            datastr = ','.join(data)
            base_name = os.path.splitext(os.path.basename(k))[0]
            name = base_name
            if name in used_names:
                name = sanitize_identifier(k)
            used_names.add(name)
            entries.append((name, base_name, k))

            cpp.write('const unsigned char {}[] = {{{}}};\n'.format(name, datastr))
            h.write('extern const unsigned char {}[{}];\n'.format(name, len(data)))

            # Emit the size as a symbol rather than relying on sizeof() of the
            # extern declaration at each use site. A translation unit compiled
            # before this resource was regenerated would otherwise pass a stale
            # size and read the blob truncated, which breaks JSON parsing.
            if not generate_bitmap_table:
                cpp.write('const size_t {}_size = sizeof({});\n'.format(name, name))
                h.write('extern const size_t {}_size;\n'.format(name))

        if generate_bitmap_table:
            h.write('struct libresrc_bitmap { const char *name; const char *path; const unsigned char *data; size_t size; };\n')
            h.write('extern const libresrc_bitmap libresrc_bitmaps[];\n')
            h.write('extern const size_t libresrc_bitmaps_count;\n')

            cpp.write('const libresrc_bitmap libresrc_bitmaps[] = {\n')
            for (identifier, base_name, path) in entries:
                cpp.write('    {"%s", "%s", %s, sizeof(%s)},\n' % (base_name, path, identifier, identifier))
            cpp.write('};\n')
            cpp.write('const size_t libresrc_bitmaps_count = sizeof(libresrc_bitmaps) / sizeof(libresrc_bitmaps[0]);\n')
