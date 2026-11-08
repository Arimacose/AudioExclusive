from pathlib import Path
import pefile,capstone,struct,re,json,hashlib
root=Path(r'C:\Program Files\WindowsApps')
old=root/'AppleInc.AppleMusicWin_1.1540.23042.0_x64__nzyj5cx40ttqa'
new=root/'AppleInc.AppleMusicWin_1.1792.39682.0_x64__nzyj5cx40ttqa'
work=Path(__file__).parent
cs=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64);cs.detail=True
def fun(pe,rva):
    for x in pe.DIRECTORY_ENTRY_EXCEPTION:
        s=x.struct
        if s.BeginAddress<=rva<s.EndAddress:return s.BeginAddress,s.EndAddress
    return rva,rva+128
def signature(pe,a,b):
    data=pe.get_data(a,b-a);mask=bytearray(b'\xff'*len(data))
    for i in cs.disasm(data,a):
        z=i.address-a
        if i.group(capstone.CS_GRP_CALL) or i.group(capstone.CS_GRP_JUMP):
            if i.imm_size:mask[z+i.imm_offset:z+i.imm_offset+i.imm_size]=b'\0'*i.imm_size
        if any(o.type==capstone.x86.X86_OP_MEM and o.mem.base==capstone.x86.X86_REG_RIP for o in i.operands):
            mask[z+i.disp_offset:z+i.disp_offset+i.disp_size]=b'\0'*i.disp_size
    return b''.join(re.escape(bytes([v])) if m else b'.' for v,m in zip(data,mask))
result=[]
for module,targets in [('CoreMedia.dll',[0x6ca8e0,0x6e2f10,0x8ea2c0]),('AMPLibraryAgent.exe',[0x40b9fa,0x82f6bc,0xb1e8ad,0xb1e972])]:
    p=pefile.PE(str(old/module));q=pefile.PE(str(new/module));mem=q.get_memory_mapped_image()
    for target in targets:
        a,b=fun(p,target);pat=signature(p,a,b);hits=list(re.finditer(pat,mem,re.DOTALL));full=bool(hits)
        if not hits: hits=list(re.finditer(signature(p,a,min(b,a+96)),mem,re.DOTALL))
        row=dict(module=module,oldTarget=hex(target),oldFunction=[hex(a),hex(b)],fullFunctionMatch=full,candidates=[hex(x.start()+target-a) for x in hits])
        print(json.dumps(row));result.append(row)
    op={s.name.decode():s.address for s in p.DIRECTORY_ENTRY_EXPORT.symbols if s.name} if hasattr(p,'DIRECTORY_ENTRY_EXPORT') else {}
    np={s.name.decode():s.address for s in q.DIRECTORY_ENTRY_EXPORT.symbols if s.name} if hasattr(q,'DIRECTORY_ENTRY_EXPORT') else {}
    if module=='CoreMedia.dll':
        for name in ['FigSimpleAlternateFilterCreate','FigAlternateFilterTreeCreate','FigAlternateAllowableMediaSubtypeFilterCreate','FigAlternateFilterMonitorCreateForPlaybackBitrate']:
            for label,pe,rva in [('old',p,op[name]),('new',q,np[name])]:
                a,b=fun(pe,rva)
                text='\n'.join(f'{i.address:08x} {i.mnemonic} {i.op_str}' for i in cs.disasm(pe.get_data(a,b-a),a))
                (work/f'{name}-{label}.asm').write_text(text)
(work/'abi-mapping.json').write_text(json.dumps(result,indent=2))
