"""Optional elevated S4U integration test. Uses a unique simulated profile only."""
from pathlib import Path
import hashlib,json,re,subprocess,sys,time
from runtime_integration import Client,SID,STARTUP,wait_ready

def run(exe,root):
    exe=Path(exe).resolve();out=Path(root).resolve()/('s4u-'+str(time.time_ns()));data=out/'data';data.mkdir(parents=True)
    identity=hashlib.sha256(str(data).lower().encode('utf-16le')).hexdigest()
    task_name='Evenfall.Engine.'+SID+'.'+identity
    assert re.fullmatch(r'[A-Za-z0-9.-]+',task_name)
    result=out/'setup.json';client=Client(data);checks=0;engine=None
    try:
        engine=subprocess.Popen([str(exe),'--engine','--simulate','--data-dir',str(data)],startupinfo=STARTUP)
        wait_ready(client,engine)
        subprocess.run([str(exe),'--setup-background','on','--simulate','--owner-sid',SID,'--data-dir',str(data),'--result',str(result)],check=True,timeout=40,startupinfo=STARTUP)
        configured=json.loads(result.read_text(encoding='utf-8'));assert configured['ok'],configured;checks+=1
        state=client.state();assert state['simulate'] and state['session']==0;checks+=1
        assert state['shutdownPrivilege'] and state['state']['settings']['background'];checks+=1
        subprocess.run([str(exe),'--client-test',str(out)],check=True,timeout=30,startupinfo=STARTUP)
        ui=json.loads((out/'client-results.json').read_text(encoding='utf-8'));assert ui['passed'] and ui['simulate'];checks+=1
        pid=client.server_pid()
        from runtime_integration import kernel
        kernel.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong];kernel.OpenProcess.restype=ctypes.c_void_p
        kernel.TerminateProcess.argtypes=[ctypes.c_void_p,ctypes.c_uint]
        handle=kernel.OpenProcess(1,False,pid);assert handle
        assert kernel.TerminateProcess(handle,1);kernel.CloseHandle(handle)
        recovered=False;deadline=time.monotonic()+80
        while time.monotonic()<deadline:
            try:
                state=client.state()
                if state['simulate'] and state['session']==0 and client.server_pid()!=pid:recovered=True;break
            except OSError:pass
            time.sleep(.5)
        assert recovered,'Task Scheduler did not recover simulated engine';checks+=1
        task={'id':'s4u-dispatch','name':'S4U simulated request','revision':1,'kind':'countdown','enabled':True,'completed':False,'day':0,'minute':0,'weekdays':127,'minutes':1,'lastHandledDay':-2000000,'onceDue':int(time.time()*1000)+1500}
        assert client.mutate('upsert',task=task)['ok']
        time.sleep(2.2)
        logs=[l for l in client.state()['state']['logs'] if l['taskId']=='s4u-dispatch']
        assert len(logs)==1 and logs[0]['status']=='requested' and logs[0]['at']>=logs[0]['due'];checks+=1
        assert client.mutate('stop')['ok'];checks+=1
        report={'passed':True,'simulate':True,'checks':checks,'client_checks':ui['checks'],'cold_boot_tested':False,'real_shutdown_tested':False}
        (out.parent/'s4u-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps(report))
    finally:
        try:client.mutate('stop')
        except Exception:pass
        if engine is not None and engine.poll() is None:engine.terminate();engine.wait(timeout=8)
        cleanup=subprocess.run(['schtasks.exe','/Delete','/TN',task_name,'/F'],capture_output=True)
        if cleanup.returncode and result.exists() and json.loads(result.read_text(encoding='utf-8')).get('ok'):
            raise RuntimeError('Temporary S4U task cleanup failed')

if __name__=='__main__':run(sys.argv[1],sys.argv[2])