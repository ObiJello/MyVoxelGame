import sys
from srctools.dmx import Element, Attribute, ValueType

with open(sys.argv[1], 'rb') as f:
    root, fmt_name, fmt_ver = Element.parse(f)

seen = set()

def walk(e, depth=0):
    if id(e) in seen:
        return
    seen.add(id(e))

    name = e.name if e.name else '<unnamed>'
    cls = e.type
    print(' ' * depth + f'[{cls}] {name}')

    for attr_name, attr in e.items():
        t = attr.type

        # If it's an array of elements, iterate
        if t is ValueType.ELEMENT and getattr(attr, 'is_array', False):
            try:
                print(' ' * (depth + 2) + f'{attr_name} -> [{len(attr)} elements]:')
                for child in attr.iter_elem():
                    walk(child, depth + 4)
                continue
            except Exception:
                pass

        # If it's a single nested element, recurse
        if t is ValueType.ELEMENT:
            try:
                child = attr.val_elem
                if child is not None:
                    print(' ' * (depth + 2) + f'{attr_name} -> element:')
                    walk(child, depth + 4)
                    continue
            except Exception:
                pass

        # Otherwise, print as a scalar value
        try:
            if getattr(attr, 'is_array', False):
                val = '[' + ', '.join(str(v) for v in attr) + ']'
            else:
                val = str(attr.val_str) if hasattr(attr, 'val_str') else repr(attr)
        except Exception as e2:
            val = f'<unreadable: {e2}>'
        print(' ' * (depth + 2) + f'{attr_name} = {val}')

walk(root)
