"""Correlate sampled PostgreSQL waits and Linux disk counters with private COMMITs."""
from bisect import bisect_left, bisect_right
from collections import Counter
import os
from pathlib import Path
import threading
import time

from phase41_database_observer import PostgresQueries, client_wal_snapshot


POLL_NS = 10_000_000
WAL_POLL_NS = 250_000_000
MAX_POLLS = 30_000
SLOW_US = 20_000
WAIT_SQL = """
    SELECT pid, coalesce(wait_event_type || ':' || wait_event, 'active')
    FROM pg_stat_activity
    WHERE datname = current_database() AND pid <> pg_backend_pid()
      AND backend_type = 'client backend' AND state = 'active'
"""
DISK_COUNTERS = {
    'writes': 4, 'sectorsWritten': 6, 'writeMs': 7, 'ioMs': 9,
    'weightedIoMs': 10, 'flushes': 15, 'flushMs': 16,
}


def wal_block_devices(wal_directory):
    device = wal_directory.stat().st_dev
    pending = [Path('/sys/dev/block') / f'{os.major(device)}:{os.minor(device)}']
    devices = []
    while pending and len(devices) < 8:
        try:
            path = pending.pop(0).resolve(strict=True)
            ident = (path / 'dev').read_text().strip()
        except OSError:
            continue
        if ident in {item['device'] for item in devices}:
            continue
        devices.append({'device': ident, 'name': path.name})
        if (path / 'partition').is_file():
            pending.append(path.parent)
        pending.extend(sorted((path / 'slaves').glob('*')))
    return devices


def disk_snapshot(text, device_ids):
    disks = {}
    for line in text.splitlines():
        columns = line.split()
        if len(columns) < 14 or (ident := ':'.join(columns[:2])) not in device_ids:
            continue
        fields = [int(value) for value in columns[3:]]
        disks[ident] = {name: fields[index] for name, index in DISK_COUNTERS.items()
                        if index < len(fields)}
        disks[ident]['inFlight'] = fields[8]
    return disks


def disk_delta(points):
    if len(points) < 2:
        return {}
    first, last = points[0], points[-1]
    elapsed_ms = (last['atNs'] - first['atNs']) / 1_000_000
    if elapsed_ms <= 0:
        return {}
    result = {}
    for device, before in first['disks'].items():
        after = last['disks'].get(device, {})
        delta = {name: after[name] - before[name] for name in DISK_COUNTERS
                 if name in after and name in before}
        if not delta or min(delta.values()) < 0:
            continue
        result[device] = {**delta, 'observedMs': round(elapsed_ms, 3),
                          'inFlightAtEnd': after['inFlight']}
    return result


class CommitObserver:
    """Poll with a dedicated existing libpq connection and bounded in-memory data."""

    def __init__(self, prefix, connection):
        self.prefix = prefix
        self.connection = connection
        self.samples = []
        self.wal = []
        self.errors = 0
        self.truncated = False
        self.stop = threading.Event()

    def __enter__(self):
        self.postgres = PostgresQueries(self.prefix / 'lib/libpq.so',
            self.connection + " application_name=nebula_commit_observer options='-c statement_timeout=2000'")
        try:
            self.settings = dict(self.postgres.query(
                "SELECT name, setting FROM pg_settings WHERE name IN "
                "('server_version_num','fsync','synchronous_commit','track_wal_io_timing','wal_sync_method')"))
            if int(self.settings['server_version_num']) < 180000:
                raise RuntimeError('Commit observation requires PostgreSQL 18+ WAL statistics')
            if self.settings['track_wal_io_timing'] != 'on':
                raise RuntimeError('WAL I/O timing was not enabled in the disposable cluster')
            directory = Path(self.postgres.query('SHOW data_directory')[0][0])
            self.devices = wal_block_devices(directory / 'pg_wal')
            self.device_ids = {item['device'] for item in self.devices}
            self.wal.append(self.wal_snapshot())
            self.sampler = threading.Thread(target=self.sample, daemon=True)
            self.sampler.start()
            return self
        except BaseException:
            self.postgres.close()
            raise

    def wal_snapshot(self):
        counts = client_wal_snapshot(self.postgres)
        counts['walBytes'] = int(self.postgres.query('SELECT wal_bytes FROM pg_stat_wal')[0][0])
        return {'atNs': time.monotonic_ns(), 'counts': counts}

    def sample(self):
        next_wal = time.monotonic_ns() + WAL_POLL_NS
        for _ in range(MAX_POLLS):
            if self.stop.is_set():
                break
            started = time.monotonic_ns()
            try:
                active = dict(self.postgres.query(WAIT_SQL))
                finished = time.monotonic_ns()
                disks = {}
                if self.device_ids:
                    try:
                        disks = disk_snapshot(Path('/proc/diskstats').read_text(), self.device_ids)
                    except OSError:
                        pass
                disk = {'atNs': time.monotonic_ns(), 'disks': disks}
                self.samples.append({'queryStartNs': started, 'queryEndNs': finished,
                                     'active': active, 'disk': disk})
                if finished >= next_wal:
                    self.wal.append(self.wal_snapshot())
                    next_wal = finished + WAL_POLL_NS
            except (RuntimeError, ValueError, IndexError):
                self.errors += 1
            self.stop.wait(max(0, started + POLL_NS - time.monotonic_ns()) / 1_000_000_000)
        else:
            self.truncated = True

    def __exit__(self, exc_type, *_):
        self.stop.set()
        self.sampler.join(timeout=6)
        if self.sampler.is_alive():
            raise RuntimeError('PostgreSQL sampler did not finish within its timeout')
        try:
            if exc_type is None:
                # The owned application has exited. Include late cumulative
                # counter publication while keeping it outside the ACK window.
                time.sleep(1.1)
                self.wal.append(self.wal_snapshot())
        finally:
            self.postgres.close()


def correlate_commits(paired, samples, workload):
    """Match one PID and wholly enclosed polls, never merely overlapping windows."""
    if not samples:
        raise RuntimeError('No successful PostgreSQL samples')
    wait_times = [point['queryStartNs'] for point in samples]
    disk_points = [point['disk'] for point in samples]
    disk_times = [point['atNs'] for point in disk_points]
    work_start, work_end = workload['start'], workload['end']
    wait_counts = Counter()
    observed = slow = observed_slow = 0
    longest = []
    for client, trace in sorted(paired, key=lambda pair: pair[1]['commitUs'], reverse=True):
        start = trace.get('commitStartedMonoNs', -1)
        finish = trace.get('commitFinishedMonoNs', -1)
        pid = trace.get('commitBackendPid', 0)
        if not (pid > 0 and work_start <= client.get('sentMonoNs', -1) <= start < finish
                <= client.get('ackMonoNs', -1) <= work_end):
            raise RuntimeError('COMMIT clock/PID missing or outside its ACK interval. Rebuild the matching Linux binary.')
        contained = [point for point in samples[bisect_left(wait_times, start):bisect_right(wait_times, finish)]
                     if point['queryEndNs'] <= finish]
        events = Counter(point['active'][str(pid)] for point in contained if str(pid) in point['active'])
        observed += bool(events)
        if trace['commitUs'] >= SLOW_US:
            slow += 1
            observed_slow += bool(events)
            wait_counts.update(events)
        if len(longest) < 20:
            disks = disk_points[bisect_left(disk_times, start):bisect_right(disk_times, finish)]
            longest.append({
                'ackMs': round(client['rttMs'], 3), 'queueUs': trace['queueUs'],
                'commitUs': trace['commitUs'], 'commitStartOffsetMs': round((start - work_start) / 1_000_000, 3),
                'backendPid': pid, 'pollsWithinCommit': len(contained),
                'backendWaitSamples': dict(events), 'diskDelta': disk_delta(disks),
            })
    work_polls = [t for t in wait_times if work_start <= t <= work_end]
    return {
        'clock': 'CLOCK_MONOTONIC', 'workloadMonotonicNs': workload,
        'commitsPaired': len(paired), 'commitsWithBackendSample': observed,
        'slowCommitThresholdMs': SLOW_US / 1000, 'slowCommits': slow,
        'slowCommitsWithBackendSample': observed_slow, 'slowCommitWaitSamples': dict(wait_counts),
        'waitPolls': len(work_polls),
        'maxPollGapMs': round(max((b - a for a, b in zip(work_polls, work_polls[1:])), default=0) / 1_000_000, 3),
        'longestCommits': longest,
        'workloadDiskDelta': disk_delta(disk_points[bisect_left(disk_times, work_start):
                                                   bisect_right(disk_times, work_end)]),
    }


def summarize_observation(observer, paired, workload):
    result = correlate_commits(paired, observer.samples, workload)
    before = max((row for row in observer.wal if row['atNs'] <= workload['start']),
                 key=lambda row: row['atNs'])
    after = observer.wal[-1]
    wal_delta = {key: round(value - before['counts'][key], 3) for key, value in after['counts'].items()}
    result.update({
        'sampleIntervalMs': POLL_NS // 1_000_000,
        'sampleErrors': observer.errors, 'sampleLimitReached': observer.truncated,
        'postgresSettings': observer.settings,
        'walApproximateWindowNs': {'start': before['atNs'], 'end': after['atNs']},
        'walDelta': wal_delta if min(wal_delta.values()) >= 0 else None,
        'diskDevices': observer.devices,
    })
    return result
