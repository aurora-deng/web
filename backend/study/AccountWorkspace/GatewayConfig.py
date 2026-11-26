"""Generate a new, separate Envoy entry for NEBULA and its Yantu workspace."""
import argparse
from copy import deepcopy
import ipaddress
import json
from pathlib import Path
from urllib.parse import urlsplit

from backend.study.AccountWorkspace.AccountWorkspace import PREFIX, origin, origin_key
from backend.study.AccountWorkspace.FrontendSnapshot import FRONTEND_PREFIX
from tools.observability.gateway import add_upstream_tls_arguments, address, check_ports, proxy_route, render, upstream_tls_arguments


def add_workspace(config, study_port, origins, *, serve_frontend=False):
    resources = config['static_resources']
    if any(cluster['name'] == 'nebula_study' for cluster in resources['clusters']):
        raise ValueError('已配置研途上游')
    cluster = deepcopy(resources['clusters'][0])
    cluster['name'] = 'nebula_study'
    cluster['circuit_breakers']['thresholds'][0].update(max_connections=40, max_requests=40, max_pending_requests=24)
    cluster['load_assignment'] = {'cluster_name': 'nebula_study', 'endpoints': [
        {'lb_endpoints': [{'endpoint': {'address': address(study_port)}}]}]}
    resources['clusters'].append(cluster)
    hosts = set()
    for value in origins:
        scheme, hostname, port = origin_key(value)
        authority = f'[{hostname}]' if ':' in hostname else hostname
        hosts.add(f'{authority}:{port}')
        if port == (443 if scheme == 'https' else 80):
            hosts.add(authority)
    for listener in resources['listeners']:
        manager = listener['filter_chains'][0]['filters'][0]['typed_config']
        for vhost in manager['route_config']['virtual_hosts']:
            vhost['domains'] = list(dict.fromkeys([*vhost['domains'], *sorted(hosts)]))
            study = proxy_route('study', {'prefix': PREFIX}, '45s')
            study['route']['cluster'] = 'nebula_study'
            routes = vhost['routes']
            api = next(index for index, route in enumerate(routes) if route['name'] == 'api')
            routes.insert(api, study)
            # The native service remains the authority for access and identity.
            control_access = proxy_route('agent_control_access', {'path': '/api/private/agent-control/access'})
            control = proxy_route('agent_control', {'prefix': '/api/private/agent-control/'}, '15s')
            control['route']['cluster'] = 'nebula_study'
            routes.insert(api, control)
            routes.insert(api, control_access)
            # Private control assets must never fall through to a public snapshot.
            page = proxy_route('agent_control_page', {'path_separated_prefix': '/phase11/agent-control'})
            routes.insert(api, page)
            if serve_frontend:
                for route in routes:
                    if route['name'] in ('assistant_page', 'assistant_assets', 'agent_runtime', 'page'):
                        route['route']['cluster'] = 'nebula_study'
                        if route['name'] == 'page':
                            route['name'] = 'study_page'
                root = proxy_route('study_page_root', {'path': FRONTEND_PREFIX.rstrip('/')})
                root['route']['cluster'] = 'nebula_study'
                routes.insert(-1, root)
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--access-log', type=Path, required=True)
    parser.add_argument('--http-port', type=int, default=18095)
    parser.add_argument('--https-port', type=int)
    parser.add_argument('--admin-port', type=int, default=19097)
    parser.add_argument('--upstream-port', type=int, required=True)
    parser.add_argument('--study-port', type=int, default=18772)
    parser.add_argument('--public-origin', type=origin, action='append', required=True)
    parser.add_argument('--listen', type=ipaddress.IPv4Address, default=ipaddress.IPv4Address('127.0.0.1'),
                        help='HTTPS interface; the plain HTTP listener always stays on loopback')
    parser.add_argument('--certificate', type=Path)
    parser.add_argument('--private-key', type=Path)
    add_upstream_tls_arguments(parser)
    parser.add_argument('--serve-frontend', action='store_true', help='Serve the isolated frontend snapshot without replacing the upstream webroot')
    args = parser.parse_args()
    ports = [args.http_port, args.admin_port, args.study_port]
    if args.https_port is not None:
        ports.append(args.https_port)
    check_ports(ports)
    if not 0 < args.upstream_port < 65536 or args.upstream_port in ports:
        parser.error('NEBULA 上游端口必须与本次入口、研途服务和管理端口分开')
    if (not args.listen.is_loopback or any(urlsplit(item).scheme == 'https' for item in args.public_origin)) and not args.https_port:
        parser.error('公开入口需要配置 HTTPS 端口和证书')
    if args.https_port and not all(path and path.is_file() for path in (args.certificate, args.private_key)):
        parser.error('请选择已有的 TLS 证书及私钥')
    config = render(args.http_port, args.admin_port, args.upstream_port, args.access_log,
                    https_port=args.https_port, cert=args.certificate, key=args.private_key, owns_upstream=False,
                    **upstream_tls_arguments(args))
    config = add_workspace(config, args.study_port, args.public_origin, serve_frontend=args.serve_frontend)
    for listener in config['static_resources']['listeners']:
        if listener['name'] == 'nebula_https':
            listener['address']['socket_address']['address'] = str(args.listen)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(config, output, indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
