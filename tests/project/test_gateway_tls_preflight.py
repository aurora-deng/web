"""Mocked TLS preflight contracts; no remote network, certificates or credentials."""
from pathlib import Path
import ssl
import sys
import unittest
from unittest.mock import MagicMock, patch
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.function3.validation import gateway_tls_preflight as probe


def fixture():
    return {"static_resources": {"clusters": [{"name": "nebula_application_tls", "load_assignment": {"endpoints": [{"lb_endpoints": [{"endpoint": {"address": {"socket_address": {"address": "127.0.0.1", "port_value": 18443}}}}]}]},
        "transport_socket": {"typed_config": {"common_tls_context": {"alpn_protocols": ["http/1.1"],
            "tls_params": {"tls_minimum_protocol_version": "TLSv1_2", "tls_maximum_protocol_version": "TLSv1_3"},
            "validation_context": {"trusted_ca": {"filename": "public-ca.crt"}, "match_typed_subject_alt_names": [{"san_type": "IP_ADDRESS", "matcher": {"exact": "192.168.239.135"}}]}}}}}]}}


class GatewayTLS(unittest.TestCase):
    def test_verified_ca_identity_alpn_and_tls_bounds_are_used(self):
        with patch.object(Path, "is_file", return_value=True), patch.object(probe.ssl, "create_default_context") as create, patch.object(probe.socket, "create_connection") as connect:
            secured = create.return_value.wrap_socket.return_value.__enter__.return_value
            secured.selected_alpn_protocol.return_value = "http/1.1"
            secured.version.return_value = "TLSv1.3"
            secured.getpeercert.return_value = b"public-test-certificate"
            result = probe.verify(fixture())
            self.assertTrue(result["upstreamTlsVerified"])
            self.assertFalse(result["configurationChanged"])
            self.assertFalse(result["applicationReadinessProbed"])
            create.assert_called_once_with(cafile="public-ca.crt")
            self.assertEqual(create.return_value.minimum_version, ssl.TLSVersion.TLSv1_2)
            self.assertEqual(create.return_value.maximum_version, ssl.TLSVersion.TLSv1_3)
            connect.assert_called_once_with(("127.0.0.1", 18443), timeout=3)
            self.assertEqual(create.return_value.wrap_socket.call_args.kwargs["server_hostname"], "192.168.239.135")

    def test_certificate_failure_is_not_retried_with_insecure_context(self):
        with patch.object(Path, "is_file", return_value=True), patch.object(probe.ssl, "create_default_context") as create, patch.object(probe.socket, "create_connection"):
            create.return_value.wrap_socket.side_effect = ssl.SSLCertVerificationError(1, "test-only")
            result = probe.verify(fixture())
            self.assertFalse(result["upstreamTlsVerified"])
            self.assertEqual(result["error"], "certificate_verification_failed")
            self.assertEqual(create.call_count, 1)

    def test_arbitrary_host_port_insecure_trust_and_private_key_paths_are_rejected(self):
        for kind in ("host", "port", "trust", "key", "san", "alpn"):
            config = fixture(); cluster = config["static_resources"]["clusters"][0]
            tls = cluster["transport_socket"]["typed_config"]["common_tls_context"]
            address = cluster["load_assignment"]["endpoints"][0]["lb_endpoints"][0]["endpoint"]["address"]["socket_address"]
            if kind == "host": address["address"] = "203.0.113.1"
            elif kind == "port": address["port_value"] = 5432
            elif kind == "trust": tls["validation_context"]["trust_chain_verification"] = "ACCEPT_UNTRUSTED"
            elif kind == "key": tls["validation_context"]["trusted_ca"]["filename"] = "private.key"
            elif kind == "san": tls["validation_context"]["match_typed_subject_alt_names"][0]["matcher"] = {"prefix": "wildcard"}
            else: tls["alpn_protocols"] = []
            with self.subTest(kind=kind), patch.object(Path, "is_file", return_value=True), self.assertRaises(ValueError): probe.upstream(config)


if __name__ == "__main__": unittest.main()
