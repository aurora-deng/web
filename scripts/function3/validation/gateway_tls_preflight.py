"""Read-only preflight for the configured loopback Envoy TLS upstream.

Uses the configured public CA and exact SAN with normal certificate verification.
Never reads private keys, changes trust/configuration, restarts services or sends
an application request. Intended to catch certificate drift after service restart.
"""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import ssl


def upstream(config, expected_port=18443):
    clusters = config.get("static_resources", {}).get("clusters", [])
    selected = [c for c in clusters if c.get("name") == "nebula_application_tls"]
    if len(selected) != 1: raise ValueError("exactly one native TLS upstream cluster is required")
    cluster = selected[0]
    groups = cluster["load_assignment"]["endpoints"]
    if len(groups) != 1 or len(groups[0]["lb_endpoints"]) != 1:
        raise ValueError("preflight only supports one fixed upstream endpoint")
    address = groups[0]["lb_endpoints"][0]["endpoint"]["address"]["socket_address"]
    if address["address"] not in {"127.0.0.1", "::1"} or address["port_value"] != expected_port:
        raise ValueError("preflight requires the expected loopback TLS port")
    tls = cluster["transport_socket"]["typed_config"]["common_tls_context"]
    validation = tls["validation_context"]
    if validation.get("trust_chain_verification", "VERIFY_TRUST_CHAIN") != "VERIFY_TRUST_CHAIN":
        raise ValueError("unverified trust modes are forbidden")
    matches = validation["match_typed_subject_alt_names"]
    if len(matches) != 1 or matches[0].get("san_type") not in {"DNS", "IP_ADDRESS"}:
        raise ValueError("exactly one DNS/IP SAN is required")
    identity = matches[0]["matcher"].get("exact")
    if not isinstance(identity, str) or not identity: raise ValueError("exact SAN is required")
    ca = Path(validation["trusted_ca"]["filename"])
    if not ca.is_file() or ca.suffix.lower() not in {".crt", ".pem", ".cer"}:
        raise ValueError("an existing public CA certificate file is required")
    protocols = tls.get("alpn_protocols")
    if protocols != ["http/1.1"]: raise ValueError("native upstream preflight requires explicit HTTP/1.1 ALPN")
    return address["address"], expected_port, ca, identity, protocols, tls.get("tls_params", {})


def verify(config, expected_port=18443):
    host, port, ca, identity, protocols, bounds = upstream(config, expected_port)
    result = {"upstreamTlsVerified": False, "certificateVerificationEnabled": True,
              "configurationChanged": False, "serviceRestarted": False, "applicationReadinessProbed": False}
    try:
        context = ssl.create_default_context(cafile=str(ca))
        versions = {"TLSv1_2": ssl.TLSVersion.TLSv1_2, "TLSv1_3": ssl.TLSVersion.TLSv1_3}
        if "tls_minimum_protocol_version" in bounds:
            context.minimum_version = versions[bounds["tls_minimum_protocol_version"]]
        if "tls_maximum_protocol_version" in bounds:
            context.maximum_version = versions[bounds["tls_maximum_protocol_version"]]
        context.set_alpn_protocols(protocols)
        with socket.create_connection((host, port), timeout=3) as connection:
            with context.wrap_socket(connection, server_hostname=identity) as secured:
                if secured.selected_alpn_protocol() != "http/1.1":
                    result["error"] = "alpn_mismatch"
                    return result
                result.update(upstreamTlsVerified=True, protocol=secured.version(), alpn=secured.selected_alpn_protocol(),
                              peerCertificateSha256=hashlib.sha256(secured.getpeercert(binary_form=True)).hexdigest())
    except ssl.SSLCertVerificationError as error:
        result.update(error="certificate_verification_failed", verifyCode=getattr(error, "verify_code", None))
    except (OSError, ssl.SSLError):
        result["error"] = "upstream_tls_unavailable"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--expected-upstream-port", type=int, default=18443)
    parser.add_argument("--output", type=Path, help="Optional new report; never overwrite prior evidence")
    args = parser.parse_args()
    if not 1024 <= args.expected_upstream_port <= 65535: parser.error("expected port must be unprivileged")
    try: report = verify(json.loads(args.config.read_text(encoding="utf-8")), args.expected_upstream_port)
    except (ValueError, KeyError, TypeError, IndexError): parser.error("unsupported or unsafe TLS upstream configuration")
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream: json.dump(report, stream, indent=2); stream.write("\n")
    print(json.dumps(report, sort_keys=True))
    return 0 if report["upstreamTlsVerified"] else 2


if __name__ == "__main__": raise SystemExit(main())
