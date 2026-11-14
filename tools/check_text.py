#!/usr/bin/env python3
"""Text hygiene check.

Every text file: no control, format or invisible characters except tab and newline.
Comments in C/C++ and .kum files: only ASCII printable characters, Russian letters and №.

Usage: check_text.py [--staged] [FILE...]
Without arguments all tracked files are checked; --staged checks files staged for commit.
"""

import os
import subprocess
import sys
import unicodedata

C_EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp')
KUM_EXTENSIONS = ('.kum',)

# Invisible or indistinguishable from ASCII, yet not in the Cc/Cf/Z* categories.
EXTRA_INVISIBLE = {0x034F, 0x115F, 0x1160, 0x2011, 0x3164, 0xFFA0}


def is_invisible(ch):
    if ch in '\t\n':
        return False
    code = ord(ch)
    if 0xFE00 <= code <= 0xFE0F or 0xE0100 <= code <= 0xE01EF or code in EXTRA_INVISIBLE:
        return True
    category = unicodedata.category(ch)
    if category == 'Zs':
        return ch != ' '
    return category in ('Cc', 'Cf', 'Co', 'Zl', 'Zp')


def is_comment_char(ch):
    return ' ' <= ch <= '~' or ch in '\t\n' or 'А' <= ch <= 'я' or ch in 'ёЁ№'


def skip_c_literal(text, i):
    start = i
    while start > 0 and (text[start - 1].isalnum() or text[start - 1] in "_'."):
        start -= 1
    prefix = text[start:i]
    if text[i] == "'" and prefix[:1].isdigit():
        return i + 1  # digit separator: 1'000'000
    if text[i] == '"' and prefix in ('R', 'u8R', 'uR', 'UR', 'LR'):
        paren = text.find('(', i)
        if paren >= 0:
            closing = ')' + text[i + 1:paren] + '"'
            end = text.find(closing, paren)
            return len(text) if end < 0 else end + len(closing)
    quote = text[i]
    j = i + 1
    while j < len(text) and text[j] != quote and text[j] != '\n':
        j += 2 if text[j] == '\\' else 1
    return j + 1


def c_comments(text):
    i = 0
    while i < len(text):
        if text.startswith('//', i):
            end = text.find('\n', i)
            end = len(text) if end < 0 else end
            yield i, end
            i = end
        elif text.startswith('/*', i):
            end = text.find('*/', i + 2)
            end = len(text) if end < 0 else end + 2
            yield i, end
            i = end
        elif text[i] in '"\'':
            i = skip_c_literal(text, i)
        else:
            i += 1


def kum_comments(text):
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '|':
            end = text.find('\n', i)
            end = len(text) if end < 0 else end
            yield i, end
            i = end
        elif ch in '"\'':
            # The lexer ends a string at the first matching quote, backslash included.
            end = text.find(ch, i + 1)
            i = len(text) if end < 0 else end + 1
        else:
            i += 1


def comment_spans(path, text):
    if path.endswith(C_EXTENSIONS):
        return c_comments(text)
    if path.endswith(KUM_EXTENSIONS):
        return kum_comments(text)
    return ()


def check_file(path):
    with open(path, 'rb') as f:
        data = f.read()
    if b'\0' in data:
        return []
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError as e:
        return [f'{path}: not valid UTF-8 at byte {e.start}']

    bad = {}
    for pos, ch in enumerate(text):
        if is_invisible(ch):
            bad[pos] = 'invisible character'
    for start, end in comment_spans(path, text):
        for pos in range(start, end):
            if pos not in bad and not is_comment_char(text[pos]):
                bad[pos] = 'character not allowed in comment'

    errors = []
    seen = set()
    for pos in sorted(bad):
        ch = text[pos]
        line = text.count('\n', 0, pos) + 1
        if (line, ch) in seen:
            continue
        seen.add((line, ch))
        column = pos - text.rfind('\n', 0, pos)
        name = unicodedata.name(ch, 'unnamed')
        errors.append(f'{path}:{line}:{column}: {bad[pos]} U+{ord(ch):04X} {name}')
    return errors


def git_files(args):
    output = subprocess.run(['git', *args, '-z'], check=True, capture_output=True).stdout
    return [f for f in output.decode('utf-8').split('\0') if f]


def main(argv):
    if argv == ['--staged']:
        files = git_files(['diff', '--cached', '--name-only', '--diff-filter=ACMR'])
    elif argv:
        files = argv
    else:
        files = git_files(['ls-files'])

    errors = []
    for path in files:
        if os.path.isfile(path):
            errors.extend(check_file(path))
    for error in errors:
        print(error)
    if errors:
        print(f'{len(errors)} problem(s); comments allow only ASCII, Russian letters and №,'
              ' invisible characters are forbidden everywhere')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
