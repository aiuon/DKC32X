#!/usr/bin/env python3
"""DKC32X Builder window: pick (or drop onto the .exe) your Donkey Kong Country ROM, press Convert,
get the 32X ROM next to it.  Runs tools/convert.py and shows its progress."""
import os, queue, subprocess, sys, threading, time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import romcheck  # noqa: E402

TITLE = 'DKC32X Builder'
NSTEPS = 15          # '##' lines a full conversion prints
NOWIN = 0x08000000 if os.name == 'nt' else 0   # CREATE_NO_WINDOW: no console windows popping up


def python_cli():
    """python.exe next to pythonw.exe (the GUI runs under pythonw)"""
    d, f = os.path.split(sys.executable)
    p = os.path.join(d, f.replace('pythonw', 'python'))
    return p if os.path.exists(p) else sys.executable


class App:
    def __init__(self, root, start):
        self.root, self.proc, self.q, self.step = root, None, queue.Queue(), ''
        root.title(TITLE)
        root.minsize(560, 380)
        root.geometry('640x460')
        f = ttk.Frame(root, padding=12)
        f.pack(fill='both', expand=True)
        ttk.Label(f, text='Donkey Kong Country  \u2192  Sega 32X', font=('Segoe UI', 14, 'bold')).grid(
            row=0, column=0, columnspan=3, sticky='w', pady=(0, 8))
        ttk.Label(f, text='Your Donkey Kong Country (SNES) ROM file:').grid(row=1, column=0, columnspan=3, sticky='w')
        self.rom = tk.StringVar(value=start or '')
        e = ttk.Entry(f, textvariable=self.rom)
        e.grid(row=2, column=0, columnspan=2, sticky='ew', pady=4)
        ttk.Button(f, text='Browse…', command=self.browse).grid(row=2, column=2, padx=(6, 0))
        self.info = ttk.Label(f, text='', foreground='#555', wraplength=560, justify='left')
        self.info.grid(row=3, column=0, columnspan=3, sticky='w')
        self.go = ttk.Button(f, text='Convert', command=self.convert)
        self.go.grid(row=4, column=0, sticky='w', pady=8)
        self.status = ttk.Label(f, text='')
        self.status.grid(row=4, column=1, columnspan=2, sticky='w', padx=8)
        self.bar = ttk.Progressbar(f, maximum=NSTEPS)
        self.bar.grid(row=5, column=0, columnspan=3, sticky='ew')
        self.log = tk.Text(f, height=12, wrap='word', state='disabled', font=('Consolas', 9))
        self.log.grid(row=6, column=0, columnspan=3, sticky='nsew', pady=(8, 0))
        sb = ttk.Scrollbar(f, command=self.log.yview)
        sb.grid(row=6, column=3, sticky='ns', pady=(8, 0))
        self.log['yscrollcommand'] = sb.set
        bf = ttk.Frame(f)
        bf.grid(row=7, column=0, columnspan=4, sticky='ew', pady=(8, 0))
        self.open = ttk.Button(bf, text='Show the 32X file', command=self.show, state='disabled')
        self.open.pack(side='left')
        self.details = tk.BooleanVar(value=False)
        ttk.Checkbutton(bf, text='Show details', variable=self.details, command=self.redraw).pack(side='left', padx=12)
        self.lines = []          # (is_step, text)
        ttk.Label(bf, text='Uses only your own ROM. Nothing is downloaded.', foreground='#777').pack(side='right')
        f.columnconfigure(1, weight=1)
        f.rowconfigure(6, weight=1)
        self.out = None
        self.rom.trace_add('write', lambda *a: self.check())
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.check()
        if start and self.ok:
            root.after(300, self.convert)

    def say(self, s, step=True):
        s = s.rstrip('\n')
        self.lines.append((step, s))
        if step or self.details.get():
            self.log['state'] = 'normal'
            self.log.insert('end', s + '\n')
            self.log.see('end')
            self.log['state'] = 'disabled'

    def redraw(self):
        self.log['state'] = 'normal'
        self.log.delete('1.0', 'end')
        for step, s in self.lines:
            if step or self.details.get():
                self.log.insert('end', s + '\n')
        self.log.see('end')
        self.log['state'] = 'disabled'

    def browse(self):
        p = filedialog.askopenfilename(title='Choose your Donkey Kong Country ROM', filetypes=[
            ('SNES ROM', '*.sfc *.smc *.swc *.fig *.bin *.zip'), ('All files', '*.*')])
        if p:
            self.rom.set(os.path.normpath(p))

    def check(self):
        p = self.rom.get().strip().strip('"')
        self.ok = False
        if not p:
            self.info['text'] = 'Choose a file with Browse…, or drop it onto "DKC32X Builder.exe".'
            return
        if not os.path.isfile(p):
            self.info['text'] = 'File not found.'
            return
        try:
            data, info = romcheck.identify(romcheck.read_input(p))
        except SystemExit as e:
            self.info['text'] = str(e)
            return
        except Exception as e:
            self.info['text'] = 'Could not read this file: %s' % e
            return
        name = '%s, %s, revision %s' % (info['title'], info['region'], info['revision'])
        ver = romcheck.SUPPORTED.get(info['md5'])
        if ver:
            self.info['text'] = 'Recognised: %s — supported.' % name
            self.ok = True
        else:
            self.info['text'] = 'Recognised: %s — this version is not supported yet (md5 %s).' % (name, info['md5'][:12])
        if not info['checksum_ok']:
            self.info['text'] += '  Warning: checksum mismatch, the dump may be damaged.'

    def convert(self):
        if self.proc:
            return
        self.check()
        if not self.ok:
            messagebox.showerror(TITLE, self.info['text'] or 'Choose your ROM first.')
            return
        rom = os.path.abspath(self.rom.get().strip().strip('"'))
        self.out = os.path.splitext(rom)[0] + ' (32X).32x'
        if os.path.exists(self.out) and not messagebox.askyesno(TITLE, '%s already exists. Replace it?' % os.path.basename(self.out)):
            return
        self.go['state'] = 'disabled'
        self.open['state'] = 'disabled'
        self.bar['value'] = 0
        self.t0 = time.time()
        self.lines = []
        self.redraw()
        self.say('Converting %s' % rom)
        self.proc = subprocess.Popen([python_cli(), '-u', os.path.join(ROOT, 'tools', 'convert.py'), rom, self.out],
                                     cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding='utf-8', errors='replace', creationflags=NOWIN)
        threading.Thread(target=self.reader, daemon=True).start()
        self.root.after(100, self.poll)

    def reader(self):
        for line in self.proc.stdout:
            self.q.put(line)
        self.q.put(None)

    def poll(self):
        try:
            while True:
                line = self.q.get_nowait()
                if line is None:
                    return self.finished()
                if line.startswith('##p '):
                    self.status['text'] = self.step + ' (' + line[4:].strip() + ')'
                    continue
                if line.startswith('## '):
                    self.step = line[3:].strip()
                    self.status['text'] = self.step
                    self.bar['value'] = min(NSTEPS, self.bar['value'] + 1)
                    self.say(self.step)
                elif line.strip():
                    self.say('   ' + line, step=False)
        except queue.Empty:
            pass
        self.status['text'] = self.status['text'].split('  [')[0] + '  [%d s]' % (time.time() - self.t0)
        self.root.after(100, self.poll)

    def finished(self):
        rc = self.proc.wait()
        self.proc = None
        try:                                    # the full log, for bug reports
            with open(os.path.join(os.path.dirname(ROOT), 'last conversion log.txt'), 'w', encoding='utf-8') as f:
                f.write('\n'.join(t for _, t in self.lines) + '\nexit code %d\n' % rc)
        except OSError:
            pass
        self.go['state'] = 'normal'
        if rc == 0 and os.path.exists(self.out):
            self.bar['value'] = NSTEPS
            self.status['text'] = 'Done in %d s.' % (time.time() - self.t0)
            self.say('Your 32X ROM: %s' % self.out)
            self.open['state'] = 'normal'
        else:
            self.status['text'] = 'Failed — see the messages below.'
            if not self.details.get():          # show what went wrong
                for step, t in [x for x in self.lines if not x[0]][-25:]:
                    self.log['state'] = 'normal'
                    self.log.insert('end', t + '\n')
                    self.log['state'] = 'disabled'
                self.log.see('end')
            messagebox.showerror(TITLE, 'The conversion failed. The window shows what went wrong; the whole log is '
                                        'in "last conversion log.txt" in the builder folder.')

    def show(self):
        if self.out and os.name == 'nt':
            subprocess.Popen(['explorer', '/select,', self.out])

    def close(self):
        if self.proc and not messagebox.askyesno(TITLE, 'A conversion is running. Stop it and quit?'):
            return
        if self.proc:
            self.proc.kill()
        self.root.destroy()


def main():
    start = ' '.join(sys.argv[1:]).strip().strip('"') or None
    root = tk.Tk()
    App(root, start)
    root.mainloop()


if __name__ == '__main__':
    main()
