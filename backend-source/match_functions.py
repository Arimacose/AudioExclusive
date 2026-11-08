from pathlib import Path
import pefile,capstone,re,difflib,json,sys
root=Path(r'C:\Program Files\WindowsApps');work=Path(__file__).parent
old=root/'AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa'
new=root/'AppleInc.AppleMusicWin_1.1792.39682.0_x64__nzyj5cx40ttqa'
cs=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def seq(pe,a,b):
    rows=[]
    for i in cs.disasm(pe.get_data(a,b-a),a):
        op=i.op_str
        op=re.sub(r'\[rip[^]]*\]','[rip]',op)
        if i.mnemonic in ['call','jmp'] or i.mnemonic.startswith('j'):op=re.sub(r'0x[0-9a-f]+','ADDR',op)
        rows.append((i.address,i.mnemonic+' '+op))
    return rows
results=[]
for module,targets in [('CoreMedia.dll',[0x6e2f10,0x8ea2c0]),('AMPLibraryAgent.exe',[0x40b9fa,0x82f6bc,0xb1e8ad,0xb1e972])]:
    p=pefile.PE(str(old/module));q=pefile.PE(str(new/module));memo={}
    for target in targets:
        s=next(x.struct for x in p.DIRECTORY_ENTRY_EXCEPTION if x.struct.BeginAddress<=target<x.struct.EndAddress)
        oa,ob=s.BeginAddress,s.EndAddress;oldseq=seq(p,oa,ob);tokens=[v for _,v in oldseq];rank=[]
        for entry in q.DIRECTORY_ENTRY_EXCEPTION:
            a,b=entry.struct.BeginAddress,entry.struct.EndAddress
            if not (0.65*(ob-oa)<b-a<1.6*(ob-oa)):continue
            if a not in memo:memo[a]=seq(q,a,b)
            ns=memo[a];nt=[v for _,v in ns]
            sm=difflib.SequenceMatcher(None,tokens,nt,autojunk=False)
            if sm.quick_ratio()<.65:continue
            score=sm.ratio()
            if len(rank)<5 or score>rank[-1][0]:rank=sorted(rank+[(score,a,b)],reverse=True)[:5]
        row=dict(module=module,target=hex(target),oldStart=hex(oa),candidates=[dict(score=s,start=hex(a),end=hex(b)) for s,a,b in rank]);results.append(row);print(json.dumps(row),flush=True)
        (work/f'{module}-{target:x}-old.asm').write_text('\n'.join(f'{a:08x} {v}' for a,v in oldseq))
        for _,a,b in rank[:2]:(work/f'{module}-{target:x}-new-{a:x}.asm').write_text('\n'.join(f'{z:08x} {v}' for z,v in memo[a]))
(work/'function-matches.json').write_text(json.dumps(results,indent=2))
