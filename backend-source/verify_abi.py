from pathlib import Path
import pefile,capstone,hashlib,json,struct
root=Path(r'C:\Program Files\WindowsApps');work=Path(__file__).parent
cs=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
reports=[]
for version,stamp,size,simple,tree,f0,f1,f2 in [
 ('1.1540.23042.0',0x69b9c1b1,0x1498000,0x6ca8e0,0x6e2f10,0x40b9fa,0x82f6bc,[0xb1e8ad,0xb1e972]),
 ('1.1792.39682.0',0x6ac9c301,0x16b7000,0x7dc3a0,0x7f4a60,0x435fe6,0x8669ab,[0xb5bca1,0xb5bd64])]:
 folder=root/f'AppleInc.AppleMusicWin_{version}_x64__nzyj5cx40ttqa'
 media=pefile.PE(str(folder/'CoreMedia.dll'));agent=pefile.PE(str(folder/'AMPLibraryAgent.exe'))
 assert media.FILE_HEADER.TimeDateStamp==stamp and media.OPTIONAL_HEADER.SizeOfImage==size
 exports={x.name.decode():x.address for x in media.DIRECTORY_ENTRY_EXPORT.symbols if x.name}
 for name in ['FigSimpleAlternateFilterCreate','FigAlternateFilterTreeCreate','FigAlternateAllowableMediaSubtypeFilterCreate','FigAlternateGetMaxAudioSampleRate','FigAlternateEligibleLosslessAudioFilterCreate']:assert name in exports
 memory=media.get_memory_mapped_image()
 refs={}
 for name,address in [('simple',simple),('tree',tree)]:
  pointer=struct.pack('<Q',media.OPTIONAL_HEADER.ImageBase+address)
  count=memory.count(pointer);assert count>0
  refs[name]=dict(rva=hex(address),classMethodTableReferences=count)
 def start_at(rva):return next(x.struct.BeginAddress for x in agent.DIRECTORY_ENTRY_EXCEPTION if x.struct.BeginAddress<=rva<x.struct.EndAddress)
 def call_before(ret):
  ins=next(cs.disasm(agent.get_data(ret-5,5),ret-5));assert ins.mnemonic=='call' and ins.size==5
  return int(ins.op_str,16)
 assert call_before(f1)==start_at(f0)
 for ret in f2:assert call_before(ret)==start_at(f1)
 thunk=call_before(f0);ins=next(cs.disasm(agent.get_data(thunk,6),thunk));assert ins.mnemonic=='jmp'
 # Import thunk must resolve to the expected reset API, not just a similar body.
 disp=struct.unpack('<i',ins.bytes[2:6])[0];iat=agent.OPTIONAL_HEADER.ImageBase+thunk+6+disp
 imported=next(i for dll in agent.DIRECTORY_ENTRY_IMPORT for i in dll.imports if i.address==iat)
 assert imported.name==b'AudioConverterReset'
 reports.append(dict(version=version,verified=True,mediaSHA256=hashlib.sha256((folder/'CoreMedia.dll').read_bytes()).hexdigest(),methodTables=refs,localSeekResetChain=[hex(f0),hex(f1),*[hex(x) for x in f2]],resetImport=imported.name.decode()))
(work/'abi-verification.json').write_text(json.dumps(reports,indent=2),'utf-8')
print(json.dumps(reports,indent=2))
