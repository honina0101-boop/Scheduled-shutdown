import urllib.request, json, pathlib, hashlib, subprocess
root=pathlib.Path(__file__).resolve().parents[1]
api=json.load(urllib.request.urlopen('https://api.github.com/repos/skeeto/w64devkit/releases/tags/v2.10.0'))
a=next(x for x in api['assets'] if x['name']=='w64devkit-x64-2.10.0.7z.exe')
out=root/'.tools'/a['name']
if not out.exists():
    urllib.request.urlretrieve(a['browser_download_url'],out)
sha=hashlib.sha256(out.read_bytes()).hexdigest()
if sha!='18d0a4c71a166f8401ab6305781bec5882b40b5e06ba9807c61cb5f3b3c6325e': raise RuntimeError('pinned toolchain hash mismatch')
if a.get('digest') and a['digest']!='sha256:'+sha: raise RuntimeError('toolchain digest mismatch')
print('Toolchain SHA256:',sha,flush=True)
if not (root/'.tools/w64devkit/bin/g++.exe').exists():
    subprocess.run([str(out),'-y','-o'+str(root/'.tools')],check=True,stdout=subprocess.DEVNULL)
for name,url in [('json.hpp','https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp'),('JSON-LICENSE.txt','https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT')]:
    target=root/'third_party'/name
    urllib.request.urlretrieve(url,target)
    print(name,hashlib.sha256(target.read_bytes()).hexdigest(),flush=True)
print('Portable toolchain ready.',flush=True)