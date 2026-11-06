"""Describe paired measurements without changing historical acceptance verdicts.

A calibration requires identical binary, configuration, workload and reset fixture
digests in every round. The sample P95 is descriptive, NOT a confidence bound or
a new acceptance threshold. No outliers are discarded. Uses only the stdlib.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


def positive(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(name + ' must be finite and positive')
    return value


def percentile(values, percent):
    ordered = sorted(values)
    if not ordered:
        raise ValueError('empty sample')
    index = (len(ordered) - 1) * percent / 100
    low = math.floor(index)
    high = math.ceil(index)
    return ordered[low] + (ordered[high] - ordered[low]) * (index - low)


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()).hexdigest()


def acceptance_result(document):
    """Only an explicit mode selects policy; labels are evidence names, not switches."""
    if document['mode'] in ('calibration', 'diagnostic'):
        return False, None
    if document['mode'] != 'acceptance':
        raise ValueError('unknown measurement mode')
    pairs = paired_rounds(document)
    if len(pairs) < 3:
        raise ValueError('acceptance requires at least three complete rounds')
    fast = 100 * (1 - statistics.median(c['fastQps'] for _, c in pairs) /
                  statistics.median(b['fastQps'] for b, _ in pairs))
    return True, (fast <= 10 and all(100 * (c['ackP99Ms'] / b['ackP99Ms'] - 1) <= 20 for b, c in pairs))


def paired_rounds(document):
    data = document.get('performance', document)
    variants = []
    for variant in ('baseline', 'current'):
        rows = data.get(variant)
        if not isinstance(rows, list) or not rows:
            raise ValueError('missing ' + variant + ' rounds')
        indexed = {}
        for row in rows:
            number = row.get('round')
            if type(number) is not int or number < 1 or number in indexed:
                raise ValueError('invalid or duplicate round in ' + variant)
            positive(row.get('ackP99Ms'), 'ackP99Ms')
            positive(row.get('fastQps'), 'fastQps')
            if type(row.get('errors')) is not int or row['errors'] != 0:
                raise ValueError('business errors invalidate measurement')
            indexed[number] = row
        variants.append(indexed)
    baseline, current = variants
    if set(baseline) != set(current) or set(baseline) != set(range(1, len(baseline) + 1)):
        raise ValueError('missing or non-contiguous paired round')
    return [(baseline[n], current[n]) for n in sorted(baseline)]


def analyse(document, gate=20):
    positive(gate, 'gate')
    pairs = paired_rounds(document)
    data = document.get('performance', document)
    calibration = data.get('mode') == 'calibration'
    differences = [100 * (c['ackP99Ms'] / b['ackP99Ms'] - 1) for b, c in pairs]
    if not all(math.isfinite(value) for value in differences):
        raise ValueError('non-finite paired ratio')
    result = {
        'mode': 'calibration' if calibration else 'historical-description-only',
        'rounds': len(pairs), 'gatePercentUnchanged': gate,
        'pairedAckIncreasePercent': differences,
        'publishedPassedUnchanged': document.get('passed', document.get('performanceGatePassed', data.get('passed'))),
        'publishedGateEvaluatedUnchanged': data.get('gateEvaluated'),
        'absolutePairDifferenceP95Percent': percentile([abs(x) for x in differences], 95),
        'baselineAckMedianMs': statistics.median(b['ackP99Ms'] for b, _ in pairs),
        'currentAckMedianMs': statistics.median(c['ackP99Ms'] for _, c in pairs),
        'outliersRemoved': 0, 'acceptanceReinterpreted': False,
        'limitation': 'Descriptive sample quantile; not a confidence interval, causal proof or replacement gate.'
    }
    if not calibration:
        result['level'] = 'NOT_CALIBRATION'
        return result
    baseline_hash = data.get('baselineBinarySha256', '')
    if not re.fullmatch('[0-9a-f]{64}', baseline_hash) or baseline_hash != data.get('currentBinarySha256'):
        raise ValueError('calibration requires identical nonempty binary SHA-256')
    config_hash = digest(data['configuration'])
    workload_hash = digest(data['workload'])
    fixture_hash = None
    for b, c in pairs:
        for row in (b, c):
            if row.get('binarySha256') != baseline_hash or row.get('configurationSha256') != config_hash or row.get('workloadSha256') != workload_hash:
                raise ValueError('per-round binary/configuration/workload identity mismatch')
            fixture = row.get('fixtureSha256', '')
            if not re.fullmatch('[0-9a-f]{64}', fixture) or (fixture_hash is not None and fixture != fixture_hash):
                raise ValueError('reset fixture identity mismatch')
            fixture_hash = fixture
            if type(row.get('ackSamples')) is not int or row['ackSamples'] < 1000:
                raise ValueError('calibration requires at least 1000 ACK samples per variant/round')
    if data.get('roundsRequested') != len(pairs) or data.get('calibrationCompleted') is not True or data.get('gateEvaluated') is not False:
        raise ValueError('incomplete calibration or acceptance gate incorrectly applied')
    result['identityVerified'] = True
    result['level'] = ('INSUFFICIENT' if len(pairs) < 10 else
                       'NOISY' if result['absolutePairDifferenceP95Percent'] > gate / 2 else 'DESCRIPTIVE_ONLY')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--gate', type=float, default=20)
    parser.add_argument('--out-dir', type=Path, required=True, help='Separate new directory; existing reports are never overwritten')
    args = parser.parse_args()
    try:
        report = analyse(json.loads(args.summary.read_text(encoding='utf-8')), args.gate)
    except (ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    destination = args.out_dir.resolve()
    if destination == args.summary.resolve().parent or any((destination/n).exists() for n in ('noise.json', 'noise-report.md')):
        parser.error('choose a new separate output directory; retain original evidence')
    destination.mkdir(parents=True, exist_ok=True)
    (destination/'noise.json').write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False)+'\n', encoding='utf-8')
    text = '\n'.join(['# A/A 测量描述', '', f"- 模式：{report['mode']}；轮次：{report['rounds']}；状态：{report['level']}",
        f"- 配对 ACK 差值绝对值样本 P95：{report['absolutePairDifferenceP95Percent']:.2f}%（未剔除异常值）",
        '- 该数值不是置信区间，不据此认定因果、提升或退化。',
        '- 保留原有 /fast ≤10%、配对 ACK P99 ≤20% 门槛及全部历史结果。',
        '- 若为 NOT_CALIBRATION，不从旧 A/B 数据推导 A/A 噪声；若为 NOISY，先改善隔离与样本量。',
        '- 原始文件不变。正式新旧版本验收需要另做同配置对照。', ''])
    (destination/'noise-report.md').write_text(text, encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, allow_nan=False))
    return 0 if report['level'] == 'DESCRIPTIVE_ONLY' else 2


if __name__ == '__main__':
    raise SystemExit(main())
