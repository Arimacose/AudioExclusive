from pathlib import Path
import hashlib, json, zipfile
root=Path(__file__).resolve().parent
payload=root/'generated/payload'
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
meta_path=payload/'bundle.json'
if not meta_path.exists():
 meta_path.write_bytes((root/'bundle-template.json').read_bytes())
meta=json.loads(meta_path.read_text('utf-8-sig'))
import re
startup=root/'Start-VerifiedAppleCompanion.ps1'
text=startup.read_text('utf-8-sig')
for name in ('am-exclusive-broker.exe','am-exclusive-hook.dll','am-exclusive-ui.dll'):
 text=re.sub(r"('"+re.escape(name)+r"'=')[A-Fa-f0-9]{64}(')",lambda m:m[1]+digest(payload/'AppleMusic'/name).upper()+m[2],text)
startup.write_text(text,'utf-8-sig')
(payload/'AppleMusic/Start-VerifiedAppleCompanion.ps1').write_bytes(startup.read_bytes())
for item in meta['files']:
 p=payload/item['path'];item.update(sha256=digest(p),bytes=p.stat().st_size)
for app_name,app in meta['applications'].items():
 for item in app['files']:item['sha256']=digest(payload/app_name/item['name'])
meta_path.write_text(json.dumps(meta,ensure_ascii=False,indent=2),'utf-8')
archive=root/'generated/payload.zip'
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
 for p in sorted(payload.rglob('*')):
  if p.is_file():z.write(p,p.relative_to(payload).as_posix())
(root/'generated/PayloadHashes.h').write_text('inline constexpr char kManagerSha256[]="'+digest(root/'Manage-AudioExclusive.ps1')+'";\ninline constexpr char kBundleSha256[]="'+digest(archive)+'";\n','utf-8')
print('Authenticated payload regenerated:',meta['build'])
