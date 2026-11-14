#!/usr/bin/env python3
"""Commit message check.

Subject and body: only ASCII printable characters, Russian letters and №.
Co-authored-by trailers naming a bot or an AI assistant are forbidden, and so
are commit authors and committers that name one.

Usage: check_commits.py REVISION_RANGE
       check_commits.py --message-file FILE
"""

import re
import subprocess
import sys
import unicodedata

from check_text import is_comment_char

CO_AUTHOR = re.compile(r'^\s*co-authored-by:(.*)$', re.IGNORECASE | re.MULTILINE)
BOT = re.compile(r'\[bot\]|\bbot\b|anthropic|claude|copilot|cursor|codex|openai|chatgpt|gemini|devin|aider',
                 re.IGNORECASE)


def check_message(name, message):
    errors = []
    seen = set()
    for line_no, line in enumerate(message.split('\n'), 1):
        for column, ch in enumerate(line, 1):
            if not is_comment_char(ch) and ch not in seen:
                seen.add(ch)
                errors.append(f'{name}:{line_no}:{column}: character not allowed'
                              f' U+{ord(ch):04X} {unicodedata.name(ch, "unnamed")}')
    for match in CO_AUTHOR.finditer(message):
        if BOT.search(match.group(1)):
            errors.append(f'{name}: bot co-authorship not allowed:{match.group(1)}')
    return errors


def check_identity(name, role, identity):
    if BOT.search(identity):
        return [f'{name}: bot {role} not allowed: {identity}']
    return []


def git(*args):
    return subprocess.run(['git', *args], check=True, capture_output=True).stdout.decode('utf-8')


def main(argv):
    if len(argv) == 2 and argv[0] == '--message-file':
        with open(argv[1], encoding='utf-8') as f:
            lines = [l for l in f.read().split('\n') if not l.startswith('#')]
        errors = check_message('commit message', '\n'.join(lines).strip())
        # In a commit-msg hook git exports the identities of the commit being made.
        for role, var in (('author', 'GIT_AUTHOR_IDENT'), ('committer', 'GIT_COMMITTER_IDENT')):
            identity = git('var', var).strip().rsplit('>', 1)[0] + '>'
            errors.extend(check_identity('commit', role, identity))
    elif len(argv) == 1:
        errors = []
        for sha in git('rev-list', argv[0]).split():
            errors.extend(check_message(sha[:12], git('log', '-1', '--format=%B', sha).strip()))
            author, committer = git('log', '-1', '--format=%an <%ae>%n%cn <%ce>', sha).strip().split('\n')
            errors.extend(check_identity(sha[:12], 'author', author))
            errors.extend(check_identity(sha[:12], 'committer', committer))
    else:
        print(__doc__)
        return 2

    for error in errors:
        print(error)
    if errors:
        print(f'{len(errors)} problem(s); commit messages allow only ASCII, Russian letters and №,'
              ' bot authors, committers and co-authors are forbidden')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
