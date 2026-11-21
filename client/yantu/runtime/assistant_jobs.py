"""Bounded, owner-scoped generation jobs; no automatic retry or persistent IO."""
import copy
import secrets
import threading
import time


class AssistantDraftJobs:
    def __init__(self, service, timeout=300):
        self.service, self.timeout = service, timeout
        self.lock, self.jobs, self.stopping = threading.RLock(), {}, False

    def start(self, owner, body):
        from assistant_drafts import identity, digest
        if not isinstance(body, dict) or set(body) != {'requestId', 'body'} or not isinstance(body['body'], dict):
            raise ValueError('生成请求格式无效')
        nonce = identity(body['requestId'])
        signature = digest(body['body'])
        with self.lock:
            for job in self.jobs.values():
                if job['owner'] == owner and job['nonce'] == nonce:
                    if job['signature'] != signature:
                        raise ValueError('生成编号已用于另一份请求；没有重复调用模型')
                    return self._public(job)
            if self.stopping or sum(job['thread'].is_alive() for job in self.jobs.values()) >= 2:
                raise ValueError('已有生成正在结束，请稍后；本次没有调用模型')
            self.jobs = {key: job for key, job in self.jobs.items() if job['thread'].is_alive() or time.monotonic() - job['at'] < 3600}
            if len(self.jobs) >= 40:
                for key in list(self.jobs):
                    if not self.jobs[key]['thread'].is_alive():
                        del self.jobs[key]
                        break
            key = secrets.token_hex(20)
            job = {'id': key, 'owner': owner, 'nonce': nonce, 'signature': signature, 'status': 'running',
                   'at': time.monotonic(), 'cancel': threading.Event(), 'output': None, 'error': ''}
            job['thread'] = threading.Thread(target=self._run, args=(job, copy.deepcopy(body['body'])), daemon=True, name='assistant-draft')
            self.jobs[key] = job
            job['thread'].start()
            return self._public(job)

    def _run(self, job, body):
        deadline = threading.Timer(self.timeout, job['cancel'].set)
        deadline.daemon = True
        deadline.start()
        try:
            value = self.service.generate(body, job['cancel'])
            with self.lock:
                if not job['cancel'].is_set():
                    job.update(status='done', output=value)
        except Exception as error:
            with self.lock:
                job.update(status='error', error=str(error)[:500] if isinstance(error, ValueError) else '本次生成未完成，请检查模型连接；没有写入学习目标')
        finally:
            deadline.cancel()
            with self.lock:
                if job['cancel'].is_set():
                    job.update(status='cancelled', output=None, error='生成已停止，思考与来源草稿仍保留')

    @staticmethod
    def _public(job):
        return {'id': job['id'], 'status': 'cancelled' if job['cancel'].is_set() else job['status'],
                'result': copy.deepcopy(job['output']) if not job['cancel'].is_set() else None, 'error': job['error']}

    def get(self, owner, key):
        with self.lock:
            job = self.jobs.get(key)
            if not job or job['owner'] != owner:
                raise ValueError('生成记录不存在或不属于本次连接')
            return self._public(job)

    def cancel(self, owner, key):
        with self.lock:
            self.get(owner, key)
            job = self.jobs[key]
            job['cancel'].set()
            return self._public(job)

    def close(self):
        with self.lock:
            self.stopping = True
            for job in self.jobs.values():
                job['cancel'].set()
