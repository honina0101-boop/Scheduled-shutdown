"""Isolated real-process IPC checks; every engine is forced into simulation."""
from pathlib import Path
import csv,ctypes,hashlib,json,os,struct,subprocess,sys,time
from ctypes import wintypes

kernel=ctypes.WinDLL('kernel32',use_last_error=True)
kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
kernel.CreateFileW.restype=wintypes.HANDLE
kernel.WaitNamedPipeW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD]
kernel.CloseHandle.argtypes=[wintypes.HANDLE]
kernel.ReadFile.argtypes=[wintypes.HANDLE,ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD),ctypes.c_void_p]
kernel.WriteFile.argtypes=kernel.ReadFile.argtypes
SID=list(csv.reader([subprocess.check_output(['whoami','/user','/fo','csv','/nh'],text=True).strip()]))[0][1]
STARTUP=subprocess.STARTUPINFO();STARTUP.dwFlags|=subprocess.STARTF_USESHOWWINDOW;STARTUP.wShowWindow=0

def pipe_name(data):
    identity=hashlib.sha256(str(Path(data).resolve()).lower().encode('utf-16le')).hexdigest()
    return r'\\.\pipe\Evenfall.'+SID+'.'+identity

class Client:
    def __init__(self,data):self.data=Path(data)
    def call(self,op='snapshot',version=None,**values):
        name=pipe_name(self.data)
        if not kernel.WaitNamedPipeW(name,1500):raise OSError(ctypes.get_last_error(),'pipe unavailable')
        h=kernel.CreateFileW(name,0xC0000000,0,None,3,0,None)
        if h==ctypes.c_void_p(-1).value:raise ctypes.WinError(ctypes.get_last_error())
        try:
            message={'protocol':1,'release':'1.1.0','requestId':str(time.time_ns()),'op':op,**values}
            if version is not None:message['version']=version
            payload=json.dumps(message,ensure_ascii=False).encode('utf-8')
            packet=struct.pack('<I',len(payload))+payload
            count=wintypes.DWORD()
            if not kernel.WriteFile(h,packet,len(packet),ctypes.byref(count),None):raise ctypes.WinError(ctypes.get_last_error())
            def read(n):
                out=b''
                while len(out)<n:
                    buf=ctypes.create_string_buffer(n-len(out))
                    if not kernel.ReadFile(h,buf,len(buf),ctypes.byref(count),None) or not count.value:raise OSError('disconnected')
                    out+=buf.raw[:count.value]
                return out
            size=struct.unpack('<I',read(4))[0];assert 0<size<=4*1024*1024
            reply=json.loads(read(size))
            ack=b'{"ack":true}';packet=struct.pack('<I',len(ack))+ack
            kernel.WriteFile(h,packet,len(packet),ctypes.byref(count),None)
            return reply
        finally:kernel.CloseHandle(h)
    def server_pid(self):
        name=pipe_name(self.data)
        if not kernel.WaitNamedPipeW(name,1500):raise OSError('pipe unavailable')
        h=kernel.CreateFileW(name,0xC0000000,0,None,3,0,None)
        if h==ctypes.c_void_p(-1).value:raise ctypes.WinError(ctypes.get_last_error())
        try:
            pid=wintypes.ULONG()
            kernel.GetNamedPipeServerProcessId.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.ULONG)]
            assert kernel.GetNamedPipeServerProcessId(h,ctypes.byref(pid))
            return pid.value
        finally:kernel.CloseHandle(h)
    def verify_acl(self):
        name=pipe_name(self.data);assert kernel.WaitNamedPipeW(name,1500)
        h=kernel.CreateFileW(name,0xC0000000,0,None,3,0,None)
        if h==ctypes.c_void_p(-1).value:raise ctypes.WinError(ctypes.get_last_error())
        try:
            advapi=ctypes.WinDLL('advapi32',use_last_error=True)
            advapi.GetKernelObjectSecurity.argtypes=[wintypes.HANDLE,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD)]
            needed=wintypes.DWORD();advapi.GetKernelObjectSecurity(h,4,None,0,ctypes.byref(needed))
            buffer=ctypes.create_string_buffer(needed.value)
            assert advapi.GetKernelObjectSecurity(h,4,buffer,len(buffer),ctypes.byref(needed))
            advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes=[ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,ctypes.POINTER(wintypes.LPWSTR),ctypes.POINTER(wintypes.DWORD)]
            text=wintypes.LPWSTR()
            assert advapi.ConvertSecurityDescriptorToStringSecurityDescriptorW(buffer,1,4,ctypes.byref(text),None)
            descriptor=text.value
            kernel.LocalFree.argtypes=[ctypes.c_void_p];kernel.LocalFree(ctypes.cast(text,ctypes.c_void_p))
            return SID in descriptor and ';;;WD)' not in descriptor and ';;;AN)' not in descriptor
        finally:kernel.CloseHandle(h)
    def anonymous_denied(self):
        name=pipe_name(self.data)
        advapi=ctypes.WinDLL('advapi32',use_last_error=True)
        advapi.ImpersonateAnonymousToken.argtypes=[wintypes.HANDLE]
        kernel.GetCurrentThread.restype=wintypes.HANDLE
        assert advapi.ImpersonateAnonymousToken(kernel.GetCurrentThread())
        try:
            h=kernel.CreateFileW(name,0xC0000000,0,None,3,0,None)
            denied=h==ctypes.c_void_p(-1).value
            if not denied:kernel.CloseHandle(h)
            return denied
        finally:assert advapi.RevertToSelf()
    def invalid_frame_rejected(self,length):
        name=pipe_name(self.data);assert kernel.WaitNamedPipeW(name,1500)
        h=kernel.CreateFileW(name,0xC0000000,0,None,3,0,None)
        if h==ctypes.c_void_p(-1).value:raise ctypes.WinError(ctypes.get_last_error())
        try:
            count=wintypes.DWORD();packet=struct.pack('<I',length)
            assert kernel.WriteFile(h,packet,len(packet),ctypes.byref(count),None)
            buffer=ctypes.create_string_buffer(4)
            return not kernel.ReadFile(h,buffer,4,ctypes.byref(count),None)
        finally:kernel.CloseHandle(h)
    def state(self):
        reply=self.call();assert reply['ok'],reply
        return reply['snapshot']
    def mutate(self,op,**values):
        return self.call(op,version=self.state()['state']['version'],**values)

def wait_ready(client,process=None):
    for _ in range(100):
        if process is not None and process.poll() is not None:raise RuntimeError('engine exited '+str(process.returncode))
        try:return client.state()
        except OSError:time.sleep(.1)
    raise RuntimeError('engine unavailable')

def run(exe,root):
    root=Path(root).resolve()/('ipc-'+str(time.time_ns()));root.mkdir(parents=True)
    client=Client(root)
    processes=[];checks=0
    def check(value,label):
        nonlocal checks
        checks+=1
        if not value:raise AssertionError(label)
    def start(*args):
        p=subprocess.Popen([str(exe),*map(str,args)],startupinfo=STARTUP)
        processes.append(p);return p
    def task(id,due):
        return {'id':id,'name':'跨进程测试 '+id,'revision':1,'kind':'countdown','enabled':True,'completed':False,
                'day':20000,'minute':0,'weekdays':127,'minutes':1,'lastHandledDay':-2000000,'onceDue':due}
    try:
        engine=start('--engine','--simulate','--data-dir',root)
        state=wait_ready(client,engine)
        check(state['simulate'] and state['state']['schemaVersion']==3,'simulation and schema')
        check(client.server_pid()==engine.pid,'IPC belongs to the unique backend process')
        check(client.verify_acl(),'pipe ACL excludes anonymous and everyone')
        check(client.anonymous_denied(),'anonymous token cannot open pipe')
        check(client.invalid_frame_rejected(0),'reject empty frame')
        check(client.invalid_frame_rejected(4*1024*1024+1),'reject oversized frame')
        wrong=start('--engine','--simulate','--owner-sid','S-1-0-0','--data-dir',root/'wrong-owner')
        wrong.wait(timeout=5)
        check(wrong.returncode==2 and not (root/'wrong-owner'/'state.json').exists(),'reject wrong owner before data access')
        duplicate=start('--engine','--simulate','--data-dir',root);duplicate.wait(timeout=5)
        check(duplicate.returncode==0 and engine.poll() is None,'only one engine')
        check(not client.call(protocol=99)['ok'],'reject protocol mismatch')
        check(not client.call(release='0.0.0')['ok'],'reject release mismatch')
        check(client.mutate('upsert',task=task('a',int(time.time()*1000)+45000))['ok'],'create IPC task')
        target=client.state()['groups'][0]
        draft=next(t for t in client.state()['state']['tasks'] if t['id']=='a').copy()
        rename=draft.copy();rename['name']='new backend name'
        check(client.mutate('upsert',task=rename,mustExist=True)['ok'],'edit with matching task version')
        check(not client.mutate('upsert',task=draft,mustExist=True)['ok'] and next(t for t in client.state()['state']['tasks'] if t['id']=='a')['name']=='new backend name','stale editor cannot overwrite newer task')
        missing=task('deleted-editor',int(time.time()*1000)+45000)
        check(not client.mutate('upsert',task=missing,mustExist=True)['ok'],'editor cannot resurrect deleted task')
        ui=start('--ui','--tray','--simulate','--data-dir',root)
        time.sleep(1)
        check(ui.poll() is None and client.state()['groups'][0]['monotonicDue']==target['monotonicDue'],'UI attaches without resetting deadline')
        again=start('--ui','--tray','--simulate','--data-dir',root);again.wait(timeout=6)
        check(again.returncode==0 and ui.poll() is None,'repeat UI reuses window')
        old_version=client.state()['state']['version']
        check(client.mutate('settings',settings={'sound':False,'theme':'dark'})['ok'],'shared settings')
        check(not client.mutate('settings',settings={'theme':'invalid'})['ok'] and client.state()['state']['settings']['theme']=='dark','invalid settings do not corrupt backend')
        check(not client.mutate('execute',command='anything')['ok'],'no arbitrary execution command')
        bad=task('bad',int(time.time()*1000)+45000);bad['minute']=2000
        check(not client.mutate('upsert',task=bad)['ok'],'backend validates task input')
        check(not client.mutate('login-start',enabled=True)['ok'],'simulation cannot change Windows login startup')
        check(not client.call('enable',version=old_version,id='a',enabled=False)['ok'],'reject stale command')
        check(client.mutate('snooze',keys=target['keys'],minutes=30)['ok'],'snooze exact group')
        state=client.state()
        pending=[p for p in state['state']['pending'] if p['taskId']=='a']
        check(len(pending)==1 and pending[0]['detached'] and pending[0]['due']>int(time.time()*1000)+29*60000,'snooze affects stored occurrence')
        check(client.mutate('enable',id='a',enabled=False)['ok'],'disable IPC task')
        check(not client.state()['state']['pending'],'disable clears snoozed occurrence')
        ui.terminate();ui.wait(timeout=6)
        check(engine.poll() is None,'UI exit keeps scheduler alive')
        check(client.mutate('upsert',task=task('b',int(time.time()*1000)+40000))['ok'],'create cancellation task')
        target=client.state()['groups'][0]
        check(client.mutate('cancel',keys=target['keys'])['ok'],'cancel exact group')
        check(not client.mutate('snooze',keys=target['keys'],minutes=10)['ok'],'cannot replay ended occurrence')
        check(client.mutate('upsert',task=task('group-first',int(time.time()*1000)+20000))['ok'],'first independent group')
        first=next(g for g in client.state()['groups'] if g['keys'][0].startswith('group-first/'))
        check(client.mutate('upsert',task=task('group-second',int(time.time()*1000)+35000))['ok'],'second independent group')
        second=next(g for g in client.state()['groups'] if g['keys'][0].startswith('group-second/'))
        check(client.mutate('cancel',keys=second['keys'])['ok'],'cancel a nonselected group')
        check(client.state()['groups'][0]['keys']==first['keys'] and client.state()['groups'][0]['monotonicDue']==first['monotonicDue'],'other group deadline unchanged')
        check(client.mutate('cancel',keys=first['keys'])['ok'],'clear remaining test group')
        blocker=root/'state.tmp.json';blocker.mkdir()
        check(not client.mutate('pause',paused=True)['ok'] and client.state()['blocked'],'save failure blocks scheduler')
        check(not client.mutate('pause',paused=False)['ok'],'blocked engine cannot resume')
        blocker.rmdir()
        check(client.mutate('retry')['ok'] and client.state()['state']['settings']['paused'],'retry saves but remains paused')
        check(client.mutate('pause',paused=True)['ok'],'pause engine')
        check(client.mutate('stop')['ok'],'stop acknowledgement')
        engine.wait(timeout=8)
        restarted=start('--engine','--simulate','--data-dir',root)
        state=wait_ready(client,restarted)
        check(state['state']['settings']['paused'] and not state['groups'],'pause survives restart')
        check(all(t['completed'] for t in state['state']['tasks'] if t['id']=='b'),'cancellation survives restart')
        check(not next(t for t in state['state']['tasks'] if t['id']=='a')['enabled'],'disable survives restart')
        check(client.mutate('pause',paused=False)['ok'],'resume engine')
        check(client.mutate('upsert',task=task('dispatch',int(time.time()*1000)+1500))['ok'],'create simulated dispatch')
        time.sleep(2.2)
        state=client.state()
        logs=[l for l in state['state']['logs'] if l['taskId']=='dispatch']
        check(len(logs)==1 and logs[0]['status']=='requested' and logs[0]['at']>=logs[0]['due'],'one simulated request, never early')
        check(client.mutate('stop')['ok'],'final stop')
        restarted.wait(timeout=8)
        print(json.dumps({'passed':True,'simulate':True,'checks':checks}))
        return checks
    finally:
        for p in processes:
            if p.poll() is None:p.terminate();p.wait(timeout=8)

if __name__=='__main__':run(Path(sys.argv[1]).resolve(),Path(sys.argv[2]))