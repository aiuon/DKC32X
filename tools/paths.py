"""Where the build tool finds its inputs (environment variables; defaults = the development box).
  DKC_DISASM   the DKC1 disassembly's DKC1/ folder (labels and file names only - no game data is
               read from it; every byte comes from your ROM)
  DKC_SURVEY   per-entrance SNES captures made from your ROM (survey/run.sh)
  DKC_HARNESS  the libretro capture harness (tools/harness/harness.c)
  DKC_TMP      scratch folder for caches and debug dumps"""
import os
DISASM = os.environ.get('DKC_DISASM', '/opt/ref/Yoshifanatic1_Donkey-Kong-Country-1-Disassembly/DKC1')
SURVEY = os.environ.get('DKC_SURVEY', '/opt/work/survey')
HARNESS = os.environ.get('DKC_HARNESS', '/opt/harness/harness')
import tempfile
TMP = os.environ.get('DKC_TMP', tempfile.gettempdir())
