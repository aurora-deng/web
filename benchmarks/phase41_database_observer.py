"""Read wait classes/WAL timings through already installed libpq; never read statement text or keys."""
import collections
import ctypes
import threading
import time

class DatabaseObserver:
    def __init__(self,database):
        if database not in ('function3_phase11_test','function3_baseline_test'):raise ValueError('Disposable benchmark database required')
        self.database=database;self.counts=collections.Counter();self.samples=0;self.errors=0;self.stop_event=threading.Event()
        self.lib=ctypes.CDLL('/home/pikachu/phase11-deps/postgresql/18.6/lib/libpq.so')
        ptr=ctypes.c_void_p;integer=ctypes.c_int;string=ctypes.c_char_p
        for name,args,result in [('PQconnectdb',[string],ptr),('PQstatus',[ptr],integer),('PQexec',[ptr,string],ptr),('PQresultStatus',[ptr],integer),('PQntuples',[ptr],integer),('PQnfields',[ptr],integer),('PQgetvalue',[ptr,integer,integer],string),('PQclear',[ptr],None),('PQfinish',[ptr],None)]:
            f=getattr(self.lib,name);f.argtypes=args;f.restype=result
        self.connection=self.lib.PQconnectdb(('host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname='+database).encode())
        if not self.connection or self.lib.PQstatus(self.connection)!=0:raise RuntimeError('Database observer could not connect')
        self.before=self.snapshot();self.thread=threading.Thread(target=self.run,daemon=True);self.started=time.monotonic();self.thread.start()
    def query(self,sql):
        result=self.lib.PQexec(self.connection,sql.encode())
        if not result:raise RuntimeError('Database observer query failed')
        try:
            if self.lib.PQresultStatus(result)!=2:raise RuntimeError('Database observer requires PG18 monitoring views')
            return [[self.lib.PQgetvalue(result,r,c).decode() for c in range(self.lib.PQnfields(result))] for r in range(self.lib.PQntuples(result))]
        finally:self.lib.PQclear(result)
    def snapshot(self):
        self.query('SELECT pg_stat_clear_snapshot()')
        row=self.query("SELECT COALESCE(sum(fsyncs),0),COALESCE(sum(fsync_time),0),COALESCE(sum(writes),0),COALESCE(sum(write_time),0) FROM pg_stat_io WHERE object='wal' AND context='normal' AND backend_type='client backend'")[0]
        return dict(zip(('fsyncCount','fsyncMilliseconds','writeCount','writeMilliseconds'),map(float,row)))
    def run(self):
        while not self.stop_event.wait(.02):
            try:
                for kind,count in self.query("SELECT wait_event_type||':'||wait_event,count(*) FROM pg_stat_activity WHERE datname='"+self.database+"' AND pid<>pg_backend_pid() AND state='active' AND wait_event IS NOT NULL GROUP BY 1"):
                    self.counts[kind]+=int(count)
                self.samples+=1
            except Exception:self.errors+=1
    def stop(self):
        self.stop_event.set();self.thread.join(timeout=5)
        if self.thread.is_alive():raise RuntimeError('Database observer did not stop')
        # Statistics flush asynchronously; wait outside the timed workload before the final snapshot.
        time.sleep(1.1);after=self.snapshot();self.lib.PQfinish(self.connection)
        return {'activeWaitSamples':dict(self.counts),'samples':self.samples,'errors':self.errors,'walClientBackendDelta':{k:round(after[k]-v,3) for k,v in self.before.items()},'scope':'test PostgreSQL cluster; no query text, credentials or private content collected'}
