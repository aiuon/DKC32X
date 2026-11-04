import re, sys
def find(s):
    out = []
    for m in re.finditer(r'\bwhile\s*\(', s):
        i = m.end(); d = 1
        while d:
            c = s[i]
            if c == '(': d += 1
            elif c == ')': d -= 1
            i += 1
        j = i
        while True:
            while s[j] in ' \t\n': j += 1
            if s.startswith('/*', j): j = s.index('*/', j) + 2; continue
            break
        if s[j] != ';': continue
        k = m.start() - 1
        while s[k] in ' \t\n': k -= 1
        if s[k] == '}':                   # do { } while (); - or the end of an earlier block
            d = 1; q = k - 1
            while d:
                if s[q] == '}': d += 1
                elif s[q] == '{': d -= 1
                q -= 1
            while s[q] in ' \t\n': q -= 1
            if s[q - 1:q + 1] == 'do' and not (s[q - 2].isalnum() or s[q - 2] == '_'): continue
        out.append((m.start(), i, j, s[m.start():i]))
    return out
if __name__ == '__main__':
    for f in sys.argv[1:]:
        s = open(f).read()
        for a, b, j, t in find(s):
            print('%s:%d: %s' % (f, s.count('\n', 0, a) + 1, ' '.join(t.split())))
