import subprocess,sys
srcp,outdir=sys.argv[1],sys.argv[2]
tags=subprocess.run(['ctags','-x','--language-force=c++','--kinds-c++=f','--_xformat=%{typeref}|%N%{signature}|%n',srcp],capture_output=True,text=True).stdout.strip().split('\n')
src=open(srcp).read().split('\n')
rows=[l.split('|') for l in tags]
first=min(int(r[2]) for r in rows)
pr=[f"{t.replace('typename:','')} {sig};" for t,sig,n in rows if not sig.startswith(('setup(','loop('))]
open(outdir+'/main.cpp','w').write('\n'.join(['#include <Arduino.h>']+src[:first-1]+pr+[f'#line {first}']+src[first-1:]))
open(outdir+'/synrg.ino','w').write('\n')
