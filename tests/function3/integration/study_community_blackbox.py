"""Check the sample discussions and search on a dedicated Function3 test instance."""

import argparse
import hashlib
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import time
from urllib.parse import urlencode
from uuid import uuid4

import runtime_blackbox as runtime


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    if not 1 <= options.port < 65536 or options.port in {18090, 18095}:
        parser.error('use a dedicated test listener')
    runtime.CONNECTION = os.environ.get('NEBULA_STUDY_VALIDATION_CONNECTION', runtime.CONNECTION)
    if runtime.sql('SELECT current_database()') != 'function3_runtime_test':
        raise RuntimeError('this test requires the isolated Function3 database')

    connection = HTTPConnection('127.0.0.1', options.port, timeout=5)
    try:
        connection.request('GET', '/phase11/shared/XiaoYan/XiaoYan.png')
        avatar = connection.getresponse()
        assert avatar.status == 200 and avatar.getheader('Content-Type') == 'image/png'
        assert hashlib.sha256(avatar.read(1024**2)).hexdigest() == \
            '075369dcfaf0772aafa7faca9cffbf6df2c8df912f3395e19c303e00dc3e3861'
        for route, marker in (('tree/', b'id="story-search"'), ('assistant/', b'id="private-friend"')):
            connection.request('GET', '/phase11/' + route)
            page = connection.getresponse()
            assert page.status == 200 and marker in page.read(256*1024)
    finally:
        connection.close()

    viewer = runtime.BrowserClient('127.0.0.1', options.port)
    viewer.register_and_login('study_demo_' + uuid4().hex[:10], 'Study-community-test-only!')
    get = lambda uri: runtime.expect(viewer, 'GET', uri)
    category = next(item['id'] for item in get('/api/tree/categories') if item['name'] == '考研')
    prefix = '/api/tree/posts?'
    samples = get(prefix + urlencode({'category': category, 'tag': '站内示例'}))['posts']
    assert len(samples) == 5, 'expected five example posts after seeding twice'
    examples = 0
    nested = 0
    for post in samples:
        assert post['bundledExample'] is True and post['body'].startswith('【站内示例】')
        endpoint = f"/api/tree/posts/{post['id']}/replies"
        replies = get(endpoint)
        assert all(reply['body'].startswith(('【示例评论】', '【示例回复】')) for reply in replies)
        examples += len(replies)
        author = get(endpoint + '?authorOnly=true')
        assert len(author) == 1 and author[0]['authorId'] == post['authorId']
        assert any(reply['id'] == author[0]['parentReplyId'] for reply in replies)
        assert get(endpoint + '?authorOnly=true&before=' + author[0]['id']) == []
        nested += len(author)
    assert (examples, nested) == (12, 5)

    matching = get(prefix + urlencode({'category':category, 'q':'  复盘做成一页  ', 'limit':1}))['posts']
    assert len(matching) == 1 and '复盘做成一页' in matching[0]['body']
    runtime.expect(viewer, 'POST', f"/api/activities/{matching[0]['id']}/favorite", {'enabled':'true'})
    assert get(prefix + urlencode({'ids':matching[0]['id']}))['posts'][0]['favorited'] is True
    sample_ids = {post['id'] for post in samples}
    batch = get(prefix + urlencode({'ids':','.join(sorted(sample_ids)), 'limit':10}))['posts']
    assert {post['id'] for post in batch} == sample_ids
    assert get(prefix + urlencode({'q':'没有这个题目的故事-841'}))['posts'] == []
    runtime.expect(viewer, 'GET', prefix + urlencode({'q':'x' * 65}), code=400)
    for invalid in ('tag=%', 'tag=%GG', 'q=%00'):
        runtime.expect(viewer, 'GET', prefix + invalid, code=400)
    runtime.expect(viewer, 'GET', f"/api/tree/posts/{samples[0]['id']}/replies?authorOnly=wrong", code=400)

    private = runtime.expect(viewer, 'POST', '/api/activities', {
        'requestId':uuid4().hex, 'body':'private-study-search-' + uuid4().hex[:8],
        'destination':'profile', 'visibility':'private',
    }, 201)
    assert get(prefix + urlencode({'q':private['body']}))['posts'] == []
    other = runtime.BrowserClient('127.0.0.1', options.port)
    other.register_and_login('study_acl_' + uuid4().hex[:10], 'Study-community-test-only!')
    runtime.expect(other, 'GET', f"/api/tree/posts/{private['id']}/replies?authorOnly=true", code=404)
    assert runtime.sql("SELECT count(*) FROM users WHERE username LIKE 'nebula_study_example%' AND disabled") == '2'
    assert runtime.sql("SELECT count(*) FROM tree_training_choices c JOIN tree_posts p ON p.id=c.post_id WHERE p.request_key LIKE 'study-example:v1:%'") == '0'

    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        entries = get(f'/api/tree/ranking?category={category}')['entries']
        if sample_ids <= {entry['postId'] for entry in entries}:
            break
        time.sleep(1)
    else:
        raise AssertionError('example discussions did not reach the actual category snapshot')

    punctuation = runtime.expect(viewer, 'POST', '/api/tree/posts', {
        'requestId':uuid4().hex, 'body':'进度 +100% 已复盘', 'categoryId':category, 'tags':'C++%复盘',
    }, 201)
    exact = get(prefix + urlencode({'tag':'C++%复盘', 'q':'+100%'}))['posts']
    assert [post['id'] for post in exact] == [punctuation['id']]

    result = {'passed':True,'posts':len(samples),'replies':examples,'nestedReplies':nested,
              'checks':['direct XiaoYan PNG','idempotent seed','category snapshot','keyword and bounds','single query decoding',
                        'linked reply and author cursor','favorite projection','private visibility',
                        'locked authors','no training authorization']}
    options.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
